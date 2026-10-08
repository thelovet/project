#include "touch.h"
#include "debug.h"
#include "settings.h"
#include "lcd.h"
#include "sok_img_win.h"
#include "timer.h"      /* 调试显示里的 GetTimerCount 限流用 */

/**
 * @brief 软件I2C引脚控制宏
 */
#define SCL_H()   GPIO_SetBits(TOUCH_SCL_PORT, TOUCH_SCL_PIN)
#define SCL_L()   GPIO_ResetBits(TOUCH_SCL_PORT, TOUCH_SCL_PIN)
#define SDA_H()   GPIO_SetBits(TOUCH_SDA_PORT, TOUCH_SDA_PIN)
#define SDA_L()   GPIO_ResetBits(TOUCH_SDA_PORT, TOUCH_SDA_PIN)
#define SDA_RD()  GPIO_ReadInputDataBit(TOUCH_SDA_PORT, TOUCH_SDA_PIN)

/**
 * @brief 触摸坐标映射参数
 */
#define TOUCH_MAX_RAW  240
#define TOUCH_MAP_MAX  240
#define TOUCH_JUMP_LIMIT_RAW          80
#define TOUCH_JUMP_CONFIRM_LIMIT_RAW  40
#define TOUCH_IT7259_DEAD_ZONE        1    /* IT7259静止死区(原始像素): 只滤±1噪声; 设3会造成拖动时"每3像素动一下" */
/* IT7259 坐标归一化窗口: 圆屏实测滑到最左/最右时原始 X 只到 23~230、原始 Y 只到 8~239 ——
   两端各有十几到二十像素够不到。若按原始值直接用, 翻转 90°/270° 后 UI 的 Y 是由原始 X 推出来的
   (case1: 239-mx, case3: mx), 那一端就永远够不到 "屏幕最下", 表现为"翻转后上/下划菜单都很难
   滑出来, 要很下面滑才行"(上划菜单要求起点 y>210/220)。
   故按 [TOUCH_NORM_MIN, TOUCH_NORM_MAX] 线性拉满到 0~239, 倍率由 MapCoord 按窗口自动算
   (必须跟着窗口走: 曾只把 MAX 改小、倍率不动, 输出范围被一起压缩 —— 例如 MAX=210 配
    239 档倍率时 UI 最大只到 208, 而底部上划要求 y>210 → 那条带子永远够不到)。
   两个宏就是"标定手指实际能到的两端":
   MAX 太大 → 右端判定带/右侧点击落在够不到或信号弱的外圈(右侧难划、右侧不灵敏);
   MAX 太小 → 倍率变大(位置失真变大), 且超过 MAX 的最外圈变成"位置不再变化"的粘区。
   左侧同理由 MIN 决定。调参口诀: 右边难划/点不到 → MAX 减小; 左边变难 → MAX 加回。
   当前 X=20~212 / Y=0~239 为实测确认值:
   X: "拿在手里(正常用法)"原始X只能到约20~211、放桌上能到0~239, 故 X 单独收窄到 20~212
      (保证拿在手里时两端都够得到); 代价是倍率 ×1.24 → 沿 X 的慢速拖动会有"1px、2px"交替的节奏。
   Y: 原始 Y 在增益命令后已稳定到约 10~239(健康轴), 故直接用满量程 0~239 →
      倍率正好 1.000(1:1 映射), 慢速拖动严格 1px 跟随, 没有任何步进感。
   注: 等硬件把面板参考地做好(见开发注意事项 §27.4), X 也能回到 0~239, 那时两轴都无步进。 */
#define TOUCH_NORM_MIN_X  20
#define TOUCH_NORM_MAX_X  212
#define TOUCH_NORM_MIN_Y  0
#define TOUCH_NORM_MAX_Y  239

/**
 * @brief 全局状态变量
 * @note  g_touch_int_flag: INT触发标志, ISR置1, Touch_IsPressed清零
 */
static uint16_t g_touch_x = 0;
static uint16_t g_touch_y = 0;
uint8_t  g_touch_pressed = 0;  /* 触摸状态, 1=有触摸, 0=无触摸 */
volatile uint8_t g_touch_int_flag = 0;

#if TOUCH_DEBUG_SHOW
static uint8_t g_dbg_point_buf[8];   /* 调试: 最近一次点报告原始字节(Byte0~) */
static uint8_t g_dbg_cmd_stat = 0xFF; /* 调试: 初始化下发命令后 Query Buffer 的命令状态位 bit1-0 (0=Done,1=Busy,2=Error) */
#endif
#if TOUCH_IC_IT7259
static uint16_t g_med_raw_x[3] = {0, 0, 0};   /* IT7259: 3点中值滤波缓存(最新在[2]) */
static uint16_t g_med_raw_y[3] = {0, 0, 0};
static uint8_t  g_med_inited = 0;             /* 中值缓存是否已用首个采样填满 */
#endif
static uint16_t g_touch_last_raw_x = 0;
static uint16_t g_touch_last_raw_y = 0;
static uint16_t g_touch_pending_raw_x = 0;
static uint16_t g_touch_pending_raw_y = 0;
static uint8_t g_touch_have_valid_sample = 0;
static uint8_t g_touch_have_pending_sample = 0;
/* 1=本次启动是"待机触摸唤醒", Touch_WakeResumeInit() 已经正确复位过一次 IC */
static uint8_t s_wake_resumed = 0;
/* 连续I2C读失败计数(兜底自愈用) */
static uint16_t s_i2c_fail_cnt = 0;

/**
 * @brief 待机唤醒后恢复软件I2C和触摸输入，但不复位CST816S
 * @note  用于读取触发唤醒的原始触摸状态，确认完成后仍需调用Touch_Init
 * @param  无
 * @retval 无
 */
void Touch_WakeResumeInit(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(
        RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /*
     * 先设置输出寄存器再切换引脚模式，避免PB8产生短低脉冲，
     * 否则会在读取唤醒触摸前复位CST816S。
     */
    GPIO_SetBits(TOUCH_SCL_PORT, TOUCH_SCL_PIN);
    GPIO_SetBits(TOUCH_SDA_PORT, TOUCH_SDA_PIN);
    GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);

    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Pin   = TOUCH_SCL_PIN;
    GPIO_Init(TOUCH_SCL_PORT, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = TOUCH_SDA_PIN;
    GPIO_Init(TOUCH_SDA_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Pin   = TOUCH_RST_PIN;
    GPIO_Init(TOUCH_RST_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Pin  = TOUCH_INT_PIN;
    GPIO_Init(TOUCH_INT_PORT, &GPIO_InitStructure);

#if TOUCH_IC_IT7259
    /* IT7259: 待机时 PB8 高阻 → RST 被拉低, 芯片实际一直处于复位态; 唤醒后 RST 刚被拉高,
       芯片需要启动时间。这里补一次复位时序(脉宽须<1ms, 否则会进sleep)并等它启动完成,
       否则紧接着的"触摸唤醒确认"(Main_ConfirmTouchWake, 仅30ms窗口)会读不到点 →
       判为无效触摸 → 直接复位回Standby(现象: 屏幕不亮、像没唤醒)。 */
    GPIO_ResetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Us(100);
    GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Ms(100);
#endif

    g_touch_int_flag = 0;
    g_touch_pressed = 0;
    g_touch_have_valid_sample = 0;
    g_touch_have_pending_sample = 0;
    s_wake_resumed = 1;      /* 已复位过 IC, Touch_Init 里不必再拉低 RST */
    s_i2c_fail_cnt = 0;
}

/**
 * @brief SDA方向: 输出
 * @param  无
 * @retval 无
 */
static void SDA_OUT(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin   = TOUCH_SDA_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TOUCH_SDA_PORT, &GPIO_InitStructure);
    SDA_H();
}

/**
 * @brief SDA方向: 输入(上拉)
 * @param  无
 * @retval 无
 */
static void SDA_IN(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin  = TOUCH_SDA_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(TOUCH_SDA_PORT, &GPIO_InitStructure);
}

/**
 * @brief 软件I2C: START信号
 * @param  无
 * @retval 无
 */
/* 试过把下面的半周期延时从 5us 降到 2us(约 200kHz) —— 实测"更差": 抓包里 Touch_Init
   被反复重入(I2C 连读失败触发自愈), 上下滑动无反应, 已回退到 5us(约 100kHz)。
   结论: 这颗 IT7259 + 本板走线, 软件 I2C 不要提速。 */
static void I2C_Start(void)
{
    SDA_OUT();
    SDA_H();
    SCL_H();
    Delay_Us(10);
    SDA_L();
    Delay_Us(5);
    SCL_L();
}

/**
 * @brief 软件I2C: STOP信号
 * @param  无
 * @retval 无
 */
static void I2C_Stop(void)
{
    SDA_OUT();
    SCL_L();
    SDA_L();
    Delay_Us(5);
    SCL_H();
    SDA_H();
}

/**
 * @brief 软件I2C: 等待从机ACK
 * @param  无
 * @retval 0=ACK, 1=超时NACK
 */
static uint8_t I2C_WaitAck(void)
{
    uint8_t timeout = 0;
    SDA_IN();
    SDA_H();
    SCL_H();
    Delay_Us(5);
    while (SDA_RD())
    {
        if (++timeout > 250)
        {
            I2C_Stop();
            return 1;
        }
    }
    SCL_L();
    return 0;
}

/**
 * @brief 软件I2C: 发送ACK
 * @param  无
 * @retval 无
 */
static void I2C_Ack(void)
{
    SCL_L();
    SDA_OUT();
    Delay_Us(5);
    SDA_L();
    Delay_Us(5);
    SCL_H();
    Delay_Us(5);
    SCL_L();
}

/**
 * @brief 软件I2C: 发送NACK
 * @param  无
 * @retval 无
 */
static void I2C_Nack(void)
{
    SCL_L();
    SDA_OUT();
    Delay_Us(5);
    SDA_H();
    Delay_Us(5);
    SCL_H();
    Delay_Us(5);
    SCL_L();
}

/**
 * @brief 软件I2C: 发送一个字节
 * @param  dat 待发送的8位数据
 * @retval 无
 */
static void I2C_SendByte(uint8_t dat)
{
    uint8_t i;
    SDA_OUT();
    SCL_L();
    for (i = 0; i < 8; i++)
    {
        if (dat & 0x80)
            SDA_H();
        else
            SDA_L();
        dat <<= 1;
        SCL_H();
        Delay_Us(5);
        SCL_L();
        Delay_Us(5);
    }
}

/**
 * @brief 软件I2C: 读取一个字节
 * @param  ack 1=发送应答ACK, 0=发送非应答NACK
 * @retval 读取到的8位数据
 */
static uint8_t I2C_ReadByte(uint8_t ack)
{
    uint8_t i, dat = 0;
    SDA_IN();
    for (i = 0; i < 8; i++)
    {
        SCL_L();
        Delay_Us(5);
        SCL_H();
        Delay_Us(5);
        dat <<= 1;
        if (SDA_RD())
            dat++;
    }
    if (ack)
        I2C_Ack();
    else
        I2C_Nack();
    return dat;
}

#if !TOUCH_IC_IT7259 || CST816S_IT7259_WRITE_TH
/**
 * @brief CST816S 写寄存器
 * @param  reg  寄存器地址
 * @param  data 写入值
 * @retval 1=成功, 0=失败
 */
static uint8_t CST816S_WriteReg(uint8_t reg, uint8_t data)
{
    I2C_Start();
    I2C_SendByte(CST816S_ADDR << 1);
    if (I2C_WaitAck()) goto err;
    I2C_SendByte(reg);
    if (I2C_WaitAck()) goto err;
    I2C_SendByte(data);
    if (I2C_WaitAck()) goto err;
    I2C_Stop();
    return 1;
err:
    I2C_Stop();
    return 0;
}
#endif

#if TOUCH_IC_IT7259
/**
 * @brief  向 IT7259 命令缓冲区写入 3 字节命令(命令+子命令+参数)。
 * @param  cmd   命令号 (如 0x15=Set Algorithm Parameter, 0x04=Set Power Mode)
 * @param  sub   子命令号 (如 0x1D=Point Threshold, 0x18=Engine Scan Period)
 * @param  param 参数值
 * @retval 1=写入成功, 0=I2C 失败
 * @note   手册3.1.6.1 示例 "Write 0x01 0x00 to Command Buffer" = S 0x8C(W) 0x20 0x01 0x00 P,
 *         即把 [命令, 子命令, 参数] 依次写入 0x20(Command Buffer); 命令执行状态见 Query Buffer
 *         的 bit1-0 (00b=Done, 01b=Busy, 10b=Error)。
 */
static uint8_t CST816S_ReadReg(uint8_t reg, uint8_t *buf, uint8_t len);   /* 等命令状态要用, 定义在下面 */
static uint8_t IT7259_WriteCmd3(uint8_t cmd, uint8_t sub, uint8_t param)
{
    uint8_t retry;
    for (retry = 0; retry < 3; retry++)
    {
        I2C_Start();
        I2C_SendByte(CST816S_ADDR << 1);
        if (I2C_WaitAck()) goto err;
        I2C_SendByte(CST816S_REG_CMD_BUF);
        if (I2C_WaitAck()) goto err;
        I2C_SendByte(cmd);
        if (I2C_WaitAck()) goto err;
        I2C_SendByte(sub);
        if (I2C_WaitAck()) goto err;
        I2C_SendByte(param);
        if (I2C_WaitAck()) goto err;
        I2C_Stop();
        /* ★ 写完必须等命令执行完再返回: Query Buffer 的 bit1-0 = 00b(Done)。
           实测(逻辑分析仪): 不等就写下一条时, 芯片"忙"会 NACK 整个写入 —— 唤醒初始化里
           0x1A/0x04(引擎模式) 与 电源模式两条命令就是这样悄悄丢掉的, 芯片没进期望状态。 */
        {
            uint8_t q, i;
            for (i = 0; i < 20; i++)
            {
                if (CST816S_ReadReg(CST816S_REG_QUERY_BUF, &q, 1) && (q & 0x03) == 0x00)
                    return 1;
                Delay_Ms(1);
            }
        }
        return 0;                       /* 命令没执行完/状态异常: 不再往下写 */
    err:
        I2C_Stop();
        Delay_Ms(2);                    /* 芯片忙: 稍等重发 */
    }
    return 0;
}
#endif

/**
 * @brief CST816S 读寄存器
 * @param  reg 起始寄存器地址
 * @param  buf 数据缓冲区
 * @param  len 读取长度
 * @retval 1=成功, 0=失败
 */
static uint8_t CST816S_ReadReg(uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t i;
    I2C_Start();
    I2C_SendByte(CST816S_ADDR << 1);
    if (I2C_WaitAck()) goto err;
    I2C_SendByte(reg);
    if (I2C_WaitAck()) goto err;
    I2C_Start();
    I2C_SendByte((CST816S_ADDR << 1) | 0x01);
    if (I2C_WaitAck()) goto err;
    for (i = 0; i < len; i++)
    {
        buf[i] = I2C_ReadByte(i < (len - 1) ? 1 : 0);
    }
    I2C_Stop();
    return 1;
err:
    I2C_Stop();
    return 0;
}

#if TOUCH_IC_IT7259
/**
 * @brief  读取 IT7259 触摸数据并解析 Point0 原始坐标。
 * @param  raw_x 输出原始X坐标 (12位)
 * @param  raw_y 输出原始Y坐标 (12位)
 * @return 1=读到有效点, 0=无点(抬手), -1=非点报告/读失败(状态未知)
 * @note   两种数据源, 用 CST816S_IT7259_USE_POINT_REPORT 切换:
 *         - 0(默认): FT6x36 风格 —— 读 0x02 手指数 + 0x03~0x06 坐标, 与厂家例程
 *           `esp_lcd_touch_touch_read_data` 一致, 属连续坐标;
 *         - 1: 读 0xE0 点数据报告(厂家例程做手势演示那套)。实测其坐标只在事件(位移够大/
 *           手势)时才更新, 表现为"移动很长才跳"; 长按时还会变成 0x80 手势报告(不含坐标),
 *           不能当抬手, 否则断触。
 */
static int8_t IT7259_ReadPoint(uint16_t *raw_x, uint16_t *raw_y)
{
    uint8_t buf[CST816S_POINT_LEN];
    uint8_t rd;
    uint8_t q = 0;

#if CST816S_IT7259_USE_POINT_REPORT
    /* 手册4.1: 必须先读 Query Buffer(0x80) 的状态, 才知道该不该/能不能取数据包。
       bit7-6: 1xb=有新数据包; 01b=无新包但手指仍按着; 00b=无手指。
       直接读点缓冲区(不查 Query)会一直取到旧包 → 坐标要移动很长才更新。 */
    if (!CST816S_ReadReg(CST816S_REG_QUERY_BUF, &q, 1))
        return -1;                      /* 读失败: 保持原状态 */
    if ((q & 0xC0) == 0x00)
        return 0;                       /* 00b: 无手指, 真抬手 */
    if ((q & 0x80) == 0)
        return -1;                      /* 01b: 手指仍按着但无新包 → 保持上次坐标, 不可判抬起 */
    rd = CST816S_ReadReg(CST816S_REG_POINT, buf, CST816S_POINT_LEN);
#else
    /* 0x02 手指数, 0x03~0x06 为第1点 XH/XL/YH/YL */
    (void)q;
    rd = CST816S_ReadReg(CST816S_REG_FINGERNUM, buf, 5);
#endif
    if (!rd)
        return -1;

#if TOUCH_DEBUG_SHOW
    {
        uint8_t k;
        for (k = 0; k < 5; k++) g_dbg_point_buf[k] = buf[k];
    }
#endif

#if CST816S_IT7259_USE_POINT_REPORT
    /* 非"点数据报告"(如 0x80 手势报告)不含坐标: 保持状态, 不可判抬手 */
    if ((buf[0] >> 4) != 0x00)
        return -1;
    /* Byte0[2:0] = Point Information (Point0~2 是否有效), 0 即无点(真抬手) */
    if ((buf[0] & 0x07) == 0)
        return 0;
    /* Byte2=X[7:0], Byte3[3:0]=X[11:8], Byte3[7:4]=Y[11:8], Byte4=Y[7:0] */
    *raw_x = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[2];
    *raw_y = ((uint16_t)(buf[3] >> 4)   << 8) | buf[4];
#else
    /* buf[0]=手指数(低4位), buf[1]=XH, buf[2]=XL, buf[3]=YH, buf[4]=YL */
    if ((buf[0] & 0x0F) == 0)
        return 0;
    *raw_x = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
    *raw_y = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
#endif
    return 1;
}
#endif


/**
 * @brief 等待触摸输入释放
 * @param  无
 * @retval 无
 */
void Touch_WaitWakeInputsReleased(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t stable_count = 0;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /*
     * ★ 待机前先复位一次触摸IC —— 本函数是"等用户松手"的循环, 而只要用户一直按着屏幕,
     * IC 就会持续上报、INT 持续为低, 循环会永远等不到释放 → 卡死在待机流程里
     * (现象: 背光已关却不进待机, 松手才睡; 循环里一直在喂狗, IWDG 也救不了)。
     * 复位后 IC 回到刚启动的状态, 不再主动产生点报告, 循环第一次判断即视为"已释放"。
     * 这也正是"休眠时手指就一直按着"能正常到点休眠的原因(唤醒流程里已复位过一次)。
     * 两种 IC 都要做, 只是复位脉宽要求不同。
     */
    {
        GPIO_InitTypeDef rst = {0};
        rst.GPIO_Pin   = TOUCH_RST_PIN;
        rst.GPIO_Mode  = GPIO_Mode_Out_PP;
        rst.GPIO_Speed = GPIO_Speed_2MHz;
        GPIO_Init(TOUCH_RST_PORT, &rst);
        GPIO_ResetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
#if TOUCH_IC_IT7259
        /* IT7259: 脉宽必须 <1ms —— 手册7章"RST 低脉冲>1ms 进 sleep(0.1uA)":
           那是彻底停机, 点屏幕也唤不醒 */
        Delay_Us(100);
#else
        /* CST816S: 低电平脉宽要求较宽(沿用 Touch_Init 的复位时序) */
        Delay_Ms(10);
#endif
        GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
        Delay_Ms(100);
    }

    gpio.GPIO_Pin = TOUCH_INT_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(TOUCH_INT_PORT, &gpio);

    gpio.GPIO_Pin = TOUCH_SCL_PIN | TOUCH_SDA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio);
    SCL_H();
    SDA_H();

    /*
     * INT只表示触摸事件，不能直接代表手指释放。
     * 持续读取手指数和坐标，消费未处理事件，直到稳定释放100ms。
     */
    while (stable_count < 20) {
        uint8_t touch_released;

#if TOUCH_IC_IT7259
        /* IT7259: 读点数据报告, 无点=已释放; 读失败则退回INT电平判断 */
        {
            uint16_t rx, ry;
            int8_t ret = IT7259_ReadPoint(&rx, &ry);
            if (ret < 0)
                touch_released =
                    (GPIO_ReadInputDataBit(TOUCH_INT_PORT, TOUCH_INT_PIN) == Bit_SET);
            else
                touch_released = (ret == 0);
        }
#else
        uint8_t fingers = 0;
        if (CST816S_ReadReg(CST816S_REG_FINGERNUM, &fingers, 1)) {
            touch_released = (fingers == 0);
            if (fingers != 0) {
                uint8_t coords[5];
                CST816S_ReadReg(CST816S_REG_XH, coords, 5);
            }
        } else {
            /* 触摸掉电或I2C不可用时退回INT电平判断。 */
            touch_released =
                (GPIO_ReadInputDataBit(TOUCH_INT_PORT, TOUCH_INT_PIN) == Bit_SET);
        }
#endif

        if (touch_released
#if TOUCH_IC_IT7259
            /* IT7259: INT 只在有事件时拉低, 可作"无触摸"的旁证 */
            && GPIO_ReadInputDataBit(TOUCH_INT_PORT, TOUCH_INT_PIN) == Bit_SET
#else
            /* CST816S: 0xFA=0x40 配的是"周期产生中断", 按住期间 INT 会周期性拉低,
               拿它当"是否释放"的判据会把计数反复清零 → 只看手指数寄存器 */
#endif
           ) {
            stable_count++;
        } else {
            stable_count = 0;
        }

        IWDG_ReloadCounter();
        Delay_Ms(5);
    }
}

/**
 * @brief 坐标映射: raw 按"该轴的窗口"[nmin,nmax] 线性拉满到 0~239
 * @param  raw  该轴原始坐标 (0~240)
 * @param  is_y 0=用X轴窗口, 1=用Y轴窗口
 * @retval 映射后坐标 (0~239)
 * @note   X/Y 分开标定的原因见文件头 TOUCH_NORM_*_X/Y 的说明。
 */
static uint16_t MapCoord(uint16_t raw, uint8_t is_y)
{
    uint16_t nmin = is_y ? TOUCH_NORM_MIN_Y : TOUCH_NORM_MIN_X;
    uint16_t nmax = is_y ? TOUCH_NORM_MAX_Y : TOUCH_NORM_MAX_X;
    uint16_t mapped;
#if TOUCH_IC_IT7259
    /* 把 [nmin, nmax] 线性拉满到 0~239。
       倍率必须跟着窗口算(不能用固定移位近似): 否则单独改 MAX 会把输出范围一起压缩,
       例如 MAX=210 配 239 档倍率时 UI 最大只到 208 → 底部上划要求 y>210 永远够不到。 */
    if (raw < nmin) raw = nmin;
    if (raw > nmax) raw = nmax;
    mapped = (uint16_t)((uint32_t)(raw - nmin) * 239 / (nmax - nmin));
#else
    (void)nmin; (void)nmax;
    mapped = (uint32_t)raw * TOUCH_MAP_MAX / TOUCH_MAX_RAW;
#endif
    if (mapped >= TOUCH_MAP_MAX) mapped = TOUCH_MAP_MAX - 1;
    return mapped;
}
/**
 * @brief  两数差绝对值。
 * @param  a 第一个数
 * @param  b 第二个数
 * @return |a-b|
 */

static uint16_t Touch_AbsDiff(uint16_t a, uint16_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

#if TOUCH_IC_IT7259
/**
 * @brief  取 3 个数中的中值(IT7259 坐标中值滤波用)。
 * @param  a 值1
 * @param  b 值2
 * @param  c 值3
 * @return 三者的中值
 */
static uint16_t Touch_Mid3(uint16_t a, uint16_t b, uint16_t c)
{
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return (a > b) ? a : b;
}
#endif
/**
 * @brief  重置触摸坐标滤波/去抖状态。
 */

static void Touch_ResetCoordFilter(void)
{
    g_touch_have_valid_sample = 0;
    g_touch_have_pending_sample = 0;
#if TOUCH_IC_IT7259
    g_med_inited = 0;
#endif
}
/**
 * @brief  应用一次原始坐标(滤波/去抖, 判定是否有效)。
 * @param  raw_x 原始 X
 * @param  raw_y 原始 Y
 * @return 1=坐标有效, 0=忽略
 */

static uint8_t Touch_TryApplyCoords(uint16_t raw_x, uint16_t raw_y)
{
    uint16_t dx;
    uint16_t dy;
#if TOUCH_IC_IT7259
    /* IT7259: 实测原始坐标偶发大跳变, 原80阈值会把连续采样成片丢弃 → 表现为"移动很长才更新";
       放宽到远超屏宽(近似不做跳变过滤) */
    const uint16_t jump_limit = 300;
#else
    const uint16_t jump_limit = TOUCH_JUMP_LIMIT_RAW;
#endif

    if (raw_x > TOUCH_MAX_RAW || raw_y > TOUCH_MAX_RAW) {
        g_touch_have_pending_sample = 0;
        return 0;
    }

    if (g_touch_have_valid_sample) {
        dx = Touch_AbsDiff(raw_x, g_touch_last_raw_x);
        dy = Touch_AbsDiff(raw_y, g_touch_last_raw_y);
        if (dx > jump_limit || dy > jump_limit) {
            if (!g_touch_have_pending_sample
                || Touch_AbsDiff(raw_x, g_touch_pending_raw_x)
                       > TOUCH_JUMP_CONFIRM_LIMIT_RAW
                || Touch_AbsDiff(raw_y, g_touch_pending_raw_y)
                       > TOUCH_JUMP_CONFIRM_LIMIT_RAW) {
                g_touch_pending_raw_x = raw_x;
                g_touch_pending_raw_y = raw_y;
                g_touch_have_pending_sample = 1;
                return 0;
            }
        }
    }

#if TOUCH_IC_IT7259
    /* 3点中值滤波: 芯片阈值调小后, 静止时会有孤立的大幅跳动, 取3点中值可直接把它丢掉;
       慢速滑动只延迟1帧(约16ms), 且输出的是真实采样值而不是量化值, 不产生台阶。 */
    if (!g_med_inited) {
        g_med_raw_x[0] = g_med_raw_x[1] = g_med_raw_x[2] = raw_x;
        g_med_raw_y[0] = g_med_raw_y[1] = g_med_raw_y[2] = raw_y;
        g_med_inited = 1;
    } else {
        g_med_raw_x[0] = g_med_raw_x[1];
        g_med_raw_x[1] = g_med_raw_x[2];
        g_med_raw_x[2] = raw_x;
        g_med_raw_y[0] = g_med_raw_y[1];
        g_med_raw_y[1] = g_med_raw_y[2];
        g_med_raw_y[2] = raw_y;
    }
    raw_x = Touch_Mid3(g_med_raw_x[0], g_med_raw_x[1], g_med_raw_x[2]);
    raw_y = Touch_Mid3(g_med_raw_y[0], g_med_raw_y[1], g_med_raw_y[2]);

    /* 静止死区滤波: IT7259 点阈值调小后静止时会有轻微抖动, 若相邻采样在 X/Y 上的变化
       都小于死区, 就沿用上次坐标(把抖动吃掉); 一旦任一轴超过死区即视为真实移动,
       立即跟随 —— 属滞回/死区滤波, 不像一阶滤波那样引入跟手延迟。 */
    if (g_touch_have_valid_sample
        && Touch_AbsDiff(raw_x, g_touch_last_raw_x) < TOUCH_IT7259_DEAD_ZONE
        && Touch_AbsDiff(raw_y, g_touch_last_raw_y) < TOUCH_IT7259_DEAD_ZONE) {
        raw_x = g_touch_last_raw_x;
        raw_y = g_touch_last_raw_y;
    }
#endif

    {
        uint16_t mx = MapCoord(raw_x, 0);
        uint16_t my = MapCoord(raw_y, 1);
        /* 根据屏幕方向旋转触摸坐标 (坐标已在 MapCoord 里按各轴窗口归一化, 四方向一致) */
        switch (g_orientation_index) {
        case 0: g_touch_x = mx;        g_touch_y = my;        break;
        case 1: g_touch_x = my;        g_touch_y = 239 - mx;  break;
        case 2: g_touch_x = 239 - mx;  g_touch_y = 239 - my;  break;
        case 3: g_touch_x = 239 - my;  g_touch_y = mx;        break;
        default: g_touch_x = mx;       g_touch_y = my;        break;
        }
    }
    g_touch_last_raw_x = raw_x;
    g_touch_last_raw_y = raw_y;
    g_touch_have_valid_sample = 1;
    g_touch_have_pending_sample = 0;
    return 1;
}

/**
 * @brief 触摸初始化 (软件I2C + INT中断)
 * @note  1. GPIO初始化 (SCL/SDA/RST)  2. 硬件复位  3. 检测芯片ID并确认I2C
 *        4. 写0xFA=0x40使能INT  5. 配置PA8 EXTI中断检测
 * @param  无
 * @retval 无
 */
void Touch_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    /* 1. GPIO初始化 (SCL/SDA/RST) */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /* SCL: PB10 推挽输出 */
    GPIO_InitStructure.GPIO_Pin   = TOUCH_SCL_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TOUCH_SCL_PORT, &GPIO_InitStructure);
    SCL_H();

    /* SDA: PB11 推挽输出 */
    GPIO_InitStructure.GPIO_Pin   = TOUCH_SDA_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TOUCH_SDA_PORT, &GPIO_InitStructure);
    SDA_H();

    /* RST: PB8 推挽输出 */
    GPIO_InitStructure.GPIO_Pin   = TOUCH_RST_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(TOUCH_RST_PORT, &GPIO_InitStructure);
    GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);

    /* 2. 硬件复位时序 (拉低再拉高)
       - CST816S: 低电平>50ms 后拉高, 等100~200ms
       - IT7259 : 复位脉宽须 <1ms (手册7章: 低电平脉宽>1ms 会使芯片进sleep模式),
                  故低电平只保持100us, 拉高后再等100ms */
#if TOUCH_IC_IT7259
    /* ★ IT7259 的 RST 低脉冲必须 <1ms(手册7章): >1ms 会让芯片进 sleep(0.1uA) —— 彻底停机,
       I2C 不再应答、INT 不再输出, 症状正是"唤醒后触摸没反应, 之后点屏幕也唤不醒"。
       而 Delay_Us 是 SysTick 忙等、会被中断打断, 偶发冲过 1ms(唤醒时 TIMER2/LCD DMA 中断
       都已使能) → 这就是"有时候"的来源。
       待机触摸唤醒路径上 Touch_WakeResumeInit() 已经做过一次正确复位(那时中断还没使能),
       这里不再重复拉低, 只把 RST 保持高电平让芯片跑起来。 */
    if (!s_wake_resumed) {
        GPIO_ResetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
        Delay_Us(100);
    }
    GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Ms(100);
    s_wake_resumed = 0;
#else
    GPIO_ResetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Ms(10);
    GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Ms(100);   /* CST816S 复位要求: 低电平>50ms, 等待100~200ms */
#endif

    /* 3. 验证I2C通信: 读芯片ID, 失败重试 */
    {
        uint8_t chip_id, retry = 0;
        do {
            Delay_Ms(10);
        } while (!CST816S_ReadReg(CST816S_REG_CHIPID, &chip_id, 1) && ++retry < 5);
    }

#if TOUCH_IC_IT7259
#if CST816S_IT7259_WRITE_TH
    /* 3.1 阈值写入: 厂家例程那套 FT6x36 寄存器(0x80~0x89); 实测无效且可能改坏配置, 默认关闭 */
    CST816S_WriteReg(CST816S_REG_THGROUP,        70);
    CST816S_WriteReg(CST816S_REG_THPEAK,         60);
    CST816S_WriteReg(CST816S_REG_THCAL,          16);
    CST816S_WriteReg(CST816S_REG_THWATER,        60);
    CST816S_WriteReg(CST816S_REG_THTEMP,         10);
    CST816S_WriteReg(CST816S_REG_THDIFF,         20);
    CST816S_WriteReg(CST816S_REG_TIME_MONITOR,    2);
    CST816S_WriteReg(CST816S_REG_PERIODACTIVE,   12);
    CST816S_WriteReg(CST816S_REG_PERIODMONITOR,  40);
    Delay_Ms(10);
#endif

    /* 3.2 算法参数(手册5.3.17 命令0x15): 决定坐标上报密度。
       手册原文(5.3.17.4): "Point Threshold —— 若移动距离 > threshold*resolution/1000
       才上报该点", 默认值下要移动十几像素才更新一次 → 滑动时坐标一格一格跳(如118→129)。
       注: 每条命令的"执行完成等待 + 失败重试"已并入 IT7259_WriteCmd3, 故下面不再各自 Delay_Ms。 */
#if CST816S_IT7259_TH_ENABLE
    IT7259_WriteCmd3(0x15, 0x1D, CST816S_IT7259_POINT_TH);    /* 子命令0x1D: Point Threshold */
    IT7259_WriteCmd3(0x15, 0x18, CST816S_IT7259_SCAN_PERIOD); /* 子命令0x18: 引擎扫描周期(ms) */
    IT7259_WriteCmd3(0x15, 0x06, CST816S_IT7259_LAST_POINT);  /* 子命令0x06: 输出最后一点 */
    /* 子命令0x07: AFE 增益(bit3-2=浮空/放桌上档, bit1-0=接地/拿手里档, 越大越灵敏)。
       实测"轻触时按下标志会跳(判不到按下)"、"拿着只能到200/不拿到239", 故把两档都调高试,
       让轻触也能稳定检出、两种握持状态下手感接近。
       A/B 验证(可关): 加上它之后"Y 稳定到239、X 仍随手变" —— 关掉(GAIN_EN=0)可确认
       Y 的稳定是不是靠这条命令来的。 */
#if CST816S_IT7259_GAIN_EN
    IT7259_WriteCmd3(0x15, 0x07, CST816S_IT7259_GAIN);
#endif
#endif

    /* 3.2b 信号链前端(手册5.3.22.2 / 5.3.22.5):
       - 0x1A/0x02 模拟前端滤波系数(平均长度): 0x00=默认 / 0x03=96pt / 0x05=160pt / 0x06=192pt
         / 0x08=256pt / 0x0A=320pt, pt 越大越平滑(SNR 更好, 响应略慢);
       - 0x1A/0x04 引擎控制: 0x03=切到接地模式(拿在手里) / 0x04=切到屏蔽模式(放桌上)。
       背景: 实测"轻触时按下标志会跳(判不到按下)"、而基线(CSEL)正常且不随握持变化,
       说明问题在"手指→面板→地"的信号链前端, 故从这两条入手改善信噪比/一致性。 */
    IT7259_WriteCmd3(0x1A, 0x02, CST816S_IT7259_AFE_FILTER);
    IT7259_WriteCmd3(0x1A, 0x04, CST816S_IT7259_ENGINE_MODE);

    /* 3.3 置为 Active 模式(手册5.3.4: Command 0x04/子命令0x00, 参数0x01=Idle, 0x00=Active)。
       Idle 是省电态(25uA), 不上报任何数据包、也不接受命令, 直到用户触摸才唤醒。 */
    IT7259_WriteCmd3(CST816S_REG_POWER_MODE, 0x00, 0x00);

#if TOUCH_DEBUG_SHOW
    /* 调试: 回读 Query Buffer 的命令状态位(bit1-0): 0=Done(命令已被接受), 1=Busy, 2=Error */
    {
        uint8_t qs = 0;
        if (CST816S_ReadReg(CST816S_REG_QUERY_BUF, &qs, 1))
            g_dbg_cmd_stat = (uint8_t)(qs & 0x03);
    }
#endif
#endif

#if !TOUCH_IC_IT7259
    /* 4. 配置中断控制寄存器: 0xFA=0x40 周期产生中断 (IT7259 走纯I2C轮询, 不配置中断) */
    CST816S_WriteReg(CST816S_REG_IRQCTL, CST816S_IRQ_ENABLE);
    Delay_Ms(10);
#endif

    /* 5. 禁用自动休眠 (保证运行期间触摸响应即时) */
    CST816S_SleepDisable();

    /* 6. 配置PA8 EXTI中断检测 */
    {
        EXTI_InitTypeDef EXTI_InitStructure = {0};
        NVIC_InitTypeDef NVIC_InitStructure = {0};

        /* PA8: 上拉输入 */
        GPIO_InitStructure.GPIO_Pin  = TOUCH_INT_PIN;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
        GPIO_Init(TOUCH_INT_PORT, &GPIO_InitStructure);

        /* EXTI8 中断配置 */
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
        GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource8);
        EXTI_InitStructure.EXTI_Line    = EXTI_Line8;
        EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
        EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
        EXTI_InitStructure.EXTI_LineCmd = ENABLE;
        EXTI_Init(&EXTI_InitStructure);

        NVIC_InitStructure.NVIC_IRQChannel                   = EXTI9_5_IRQn;
        NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority  = 1;
        NVIC_InitStructure.NVIC_IRQChannelSubPriority         = 1;
        NVIC_InitStructure.NVIC_IRQChannelCmd                 = ENABLE;
        NVIC_Init(&NVIC_InitStructure);

        EXTI_ClearITPendingBit(EXTI_Line8);
    }

    g_touch_int_flag = 0;
}

/**
 * @brief EXTI9_5中断服务: PA8 INT引脚 触摸
 * @param  无
 * @retval 无
 */
void EXTI9_5_IRQHandler(void)
    __attribute__((interrupt("WCH-Interrupt-fast"), used, externally_visible));
/**
 * @brief  触摸外部中断(EXTI9_5)处理。
 * @note   标准 ISR, 唤醒/标记触摸事件。
 */
void EXTI9_5_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line8) != RESET)
    {
        g_touch_int_flag = 1;
        EXTI_ClearITPendingBit(EXTI_Line8);
    }
}

/*********************************************************************
 * @brief  启用CST816S自动休眠 (待机前调用, 降低功耗)
 * @param  无
 * @retval 无
 **********************************************************************/
void CST816S_SleepEnable(void)
{
#if TOUCH_IC_IT7259
    /* IT7259: 待机前不能让它进 sleep —— 手册7章的"RST 低脉冲>1ms 进 sleep"是深睡眠(0.1uA),
       芯片会完全停止响应触摸、INT 不再输出 → 待机后点屏幕唤不醒(实测)。故这里保持芯片工作,
       只靠 INT 在触摸时唤醒 MCU(功耗换可用性); 若以后要低功耗, 需改用 Idle 模式命令。 */
#else
    CST816S_WriteReg(CST816S_REG_LP_SCAN_TH,
                     CST816S_LP_SCAN_THRESHOLD);
    CST816S_WriteReg(CST816S_REG_AUTOSLEEP, CST816S_SLEEP_ENABLE);
#endif
}

/*********************************************************************
 * @brief  禁用CST816S自动休眠 (初始化/唤醒后调用, 保证触摸即时响应)
 * @param  无
 * @retval 无
 **********************************************************************/
void CST816S_SleepDisable(void)
{
#if TOUCH_IC_IT7259
    /* IT7259: 复位(脉宽<1ms)后即为Active态, 无需额外命令 */
#else
    CST816S_WriteReg(CST816S_REG_AUTOSLEEP, CST816S_SLEEP_DISABLE);
#endif
}

/**
 * @brief 获取触摸状态 (读取缓存, 无I2C, 快速返回)
 * @param  state TouchState结构体指针, 用于填充数据
 * @retval 无
 */
void Touch_Scan(TouchState *state)
{
    state->pressed = g_touch_pressed;
    state->x       = g_touch_x;
    state->y       = g_touch_y;
}


/**
 * @brief INT引脚触摸检测 (由INT触发, 中断调用)
 * @note  CST816S模式(0xFA=0x40): 按下发INT, 持续按发INT, 抬起发INT, 都发INT
 *        g_triggered逻辑: 首次按下返回1, 松开重置0, 持续按下返回0
 * @param  无
 * @retval 1=首次按下, 0=无触摸
 */
uint8_t Touch_IsPressed(void)
{
    static uint8_t g_triggered = 0;  /* 触发状态, 用于防止重复上报 */

#if TOUCH_IC_IT7259
    /* IT7259: 纯I2C轮询(不依赖INT), 每帧读点数据报告 */
        g_touch_int_flag = 0;
    {
        static uint8_t s_no_point_cnt = 0;   /* 连续无点计数(释放去抖) */
        uint16_t raw_x = 0, raw_y = 0;
        int8_t got = 0;                      /* 1=读到点, 0=全是无点, -1=读失败 */
        uint8_t i;

        /* 连读多次取最新: 点报告若是队列, 每帧只取一包会持续滞后(滑动一跳一跳) */
        for (i = 0; i < CST816S_POINT_POLL_TIMES; i++)
        {
            uint16_t tx, ty;
            int8_t r = IT7259_ReadPoint(&tx, &ty);
            if (r < 0) { got = -1; break; }
            if (r == 1) { got = 1; raw_x = tx; raw_y = ty; }
        }

        if (got < 0)
        {
            /* I2C读失败: 状态未知, 保持原状态(否则误判抬手, 拖动一卡一卡) */
            /* ★ 兜底自愈: 连续失败(每次轮询+1)说明 IC 通信已经坏掉 —— 例如被复位脉冲
               弄进 sleep(见 Touch_Init 的说明)。这里主动重新初始化一次, 不必等用户重启设备;
               否则通信一坏就坏到底, 表现就是"触摸彻底没反应"。 */
            if (++s_i2c_fail_cnt >= 120)
            {
                s_i2c_fail_cnt = 0;
                s_no_point_cnt = 0;
                g_triggered = 0;
                g_touch_pressed = 0;
                Touch_Init();
            }
            return 0;
        }
        s_i2c_fail_cnt = 0;
        if (got == 0)
        {
            /* 无点: 连续2次才判抬手(报点间隙/空包会误判断触) */
            if (++s_no_point_cnt >= 2)
            {
                /* ★ 残留包只在"刚判抬手"那一帧清一次(s_no_point_cnt 到 2 那一刻)!
                    这个计数在抬手后不会归零、会一直涨, 若放在 >=2 里, 空闲时就会**每帧**
                    多读 32 字节软件 I2C(约3ms) → 帧时间凭空多两成, 所有动画/滚动都变慢。
                    点缓冲是"读满 32 字节才推进"的单包缓冲: 抬手那一刻芯片还会生成本次触摸的
                    最后一个包(含抬起位置), 而本函数在"无手指"分支直接返回、从不消费它;
                    残留包会在下一次按下时被当成第一个采样 → 按压位置被记成"上一次触摸的坐标"
                    (表现: 我明明点这里, 执行的却是上一次的位置)。读掉它即可。 */
                if (s_no_point_cnt == 2)
                {
                    uint8_t flush[CST816S_POINT_LEN];
                    (void)CST816S_ReadReg(CST816S_REG_POINT, flush, CST816S_POINT_LEN);
                }
                Touch_ResetCoordFilter();
                g_triggered = 0;
                g_touch_pressed = 0;
            }
            return 0;
        }
        s_no_point_cnt = 0;
        if (!Touch_TryApplyCoords(raw_x, raw_y))
            return 0;
        g_touch_pressed = 1;
        if (!g_triggered)
        {
            g_triggered = 1;
            return 1;
        }
        return 0;
    }
#else
    if (g_touch_int_flag)
    {
        g_touch_int_flag = 0;

        /* 读取触摸点数量, 做释放/按下判断 */
        uint8_t fingers;
        if (!CST816S_ReadReg(CST816S_REG_FINGERNUM, &fingers, 1))
        {
            return 0;
        }

        if (fingers == 0)
        {
            /* Release: unlock trigger, keep last polled coords from Touch_UpdateCoords */
            Touch_ResetCoordFilter();
            g_triggered = 0;
            g_touch_pressed = 0;
            return 0;
        }

        /* 按下: 读取触摸坐标到全局变量 (映射后坐标) */
        {
            uint8_t buf[5];
            if (!CST816S_ReadReg(CST816S_REG_XH, buf, 5)) {
                return 0;
            }
            uint16_t raw_x = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
            uint16_t raw_y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
            if (!Touch_TryApplyCoords(raw_x, raw_y))
                return 0;
        }

        g_touch_pressed = 1;

        /* 首次检测到 才返回1 (首次按下) */
        if (!g_triggered)
        {
            g_triggered = 1;
            return 1;
        }
        /* 持续按 才返回0 (已触发过, 避免重复上报) */
    }
    /* 无INT但状态仍为按下: 轮询I2C检测是否漏掉了释放INT */
    else if (g_touch_pressed)
    {
        uint8_t fingers;
        uint8_t read_ok = CST816S_ReadReg(CST816S_REG_FINGERNUM, &fingers, 1);
        if (!read_ok) {
            /* Keep cached state when an I2C read fails. */
        }
        else if (fingers == 0)
        {
            Touch_ResetCoordFilter();
            g_triggered = 0;
            g_touch_pressed = 0;
        }
    }
#endif
    return 0;
}

/**
 * @brief Poll I2C for real-time touch coords (call during hold, not INT-dependent)
 * @note  Reads finger count and XY from CST816S. Updates g_touch_x/y and g_touch_pressed.
 *        Does NOT rely on INT flag - direct I2C polling.
 * @param  None
 * @retval None
 */
void Touch_UpdateCoords(void)
{
#if TOUCH_IC_IT7259
    /* IT7259: 轮询点数据报告刷新长按期间的实时坐标 */
    {
        uint16_t raw_x, raw_y;
        int8_t ret = IT7259_ReadPoint(&raw_x, &raw_y);
        if (ret < 0)
            return;              /* 读失败: 保持原状态 */
        if (ret == 0)
            return;              /* 无点: 抬手统一由 Touch_IsPressed 去抖后处理 */
        if (!Touch_TryApplyCoords(raw_x, raw_y))
            return;
        g_touch_pressed = 1;
    }
#else
    uint8_t fingers;
    if (!CST816S_ReadReg(CST816S_REG_FINGERNUM, &fingers, 1)) {
        return;
    }

    if (fingers == 0) {
        Touch_ResetCoordFilter();
        g_touch_pressed = 0;
        return;
    }

    uint8_t buf[5];
    if (!CST816S_ReadReg(CST816S_REG_XH, buf, 5)) {
        return;
    }

    uint16_t raw_x = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
    uint16_t raw_y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    if (!Touch_TryApplyCoords(raw_x, raw_y))
        return;
    g_touch_pressed = 1;
#endif
}

/**
 * @brief  配置 PA8 触摸唤醒功能 (Standby模式)
 * @param  无
 * @retval 无
 */
void Touch_StandbyWakeupConfig(void)
{
    EXTI_InitTypeDef EXTI_InitStructure = {0};

    RCC_APB2PeriphClockCmd(TOUCH_INT_RCC | RCC_APB2Periph_AFIO, ENABLE);

    /* PA8: 上拉输入 */
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin   = TOUCH_INT_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(TOUCH_INT_PORT, &GPIO_InitStructure);

    /* EXTI8 事件配置 (无中断, 仅用于Standby唤醒) */
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource8);
    EXTI_InitStructure.EXTI_Line    = EXTI_Line8;
    EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Event;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    EXTI_ClearFlag(EXTI_Line8);
}

#if TOUCH_DEBUG_SHOW
/**
 * @brief  绘制 3 位数字(000~999, 前导0补位), 供坐标调试显示用。
 * @param  x     左上角X
 * @param  y     左上角Y
 * @param  v     数值
 * @param  color 颜色
 */
static void Touch_DebugNum3(uint16_t x, uint16_t y, uint16_t v, uint16_t color)
{
    sok_draw_digit_scaled(x,      y, (uint8_t)((v / 100) % 10), color, 8, 14);
    sok_draw_digit_scaled(x + 8,  y, (uint8_t)((v / 10) % 10),  color, 8, 14);
    sok_draw_digit_scaled(x + 16, y, (uint8_t)(v % 10),         color, 8, 14);
}

#if TOUCH_IC_IT7259
/* 基线(CSEL)诊断: 用于判断"轻触不灵敏/坐标随握持变化"是否与芯片基线补偿有关 */
static uint16_t g_dbg_csel_min = 0;
static uint16_t g_dbg_csel_max = 0;
static uint8_t  g_dbg_csel_ok  = 0;
static uint32_t g_dbg_csel_us  = 0;

/**
 * @brief  调试: 读一次"当前模式 CSEL"(各通道基线补偿), 统计最小/最大值。
 * @note   命令 0x1A/子命令0x05/参数0x00 = Get Current Mode CSEL(手册5.3.22.6),
 *         返回 28 个 WORD, 小端: 低字节=CSEL, 高字节=CIN(通道号), 从 Command Response Buffer(0xA0) 读。
 *         只在无触摸时读(手指按着时 CSEL 本身会变化), 并限流到约 0.5s 一次(一次要读 56 字节)。
 */
static void Touch_DebugReadBaseline(void)
{
    uint8_t buf[56];
    uint8_t i, qs = 0;
    uint16_t mn = 0xFFFF, mx = 0;

    if (g_touch_pressed) { g_dbg_csel_ok = 0; return; }
    if ((uint32_t)(GetTimerCount() - g_dbg_csel_us) < 500000UL) return;
    g_dbg_csel_us = GetTimerCount();

    if (!IT7259_WriteCmd3(0x1A, 0x05, 0x00)) { g_dbg_csel_ok = 0; return; }
    for (i = 0; i < 20; i++) {                    /* 等命令完成: Query 的 bit1-0 == 00b(Done) */
        if (!CST816S_ReadReg(CST816S_REG_QUERY_BUF, &qs, 1)) break;
        if ((qs & 0x03) == 0x00) break;
        Delay_Ms(1);
    }
    if (!CST816S_ReadReg(CST816S_REG_CMD_RSP, buf, 56)) { g_dbg_csel_ok = 0; return; }
    for (i = 0; i < 56; i += 2) {                  /* 小端 WORD: 低字节=CSEL */
        uint16_t v = (uint16_t)buf[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    g_dbg_csel_min = mn;
    g_dbg_csel_max = mx;
    g_dbg_csel_ok = 1;
}
#endif

/**
 * @brief  调试: 屏幕上显示触摸状态、点报告原始字节与解析后的坐标/基线诊断值。
 * @note   行1=按下标志(红) 行2=Byte0标志(白) 行3=Byte2(黄) 行4=Byte3(浅蓝) 行5=Byte4(白)
 *         行6=映射后X(绿) 行7=映射后Y(红) 行8=CSEL最小(青) 行9=CSEL最大(蓝) 行10=基线读取成功(白);
 *         圆形屏四角在圆外不可见, 故放在左中部(距圆心<120);
 *         sok_draw_digit_scaled 不带CS控制, LCD_Fill 之后须先 LCD_CS_CLR(见§8)。
 */
void Touch_DebugShow(void)
{
#if TOUCH_IC_IT7259
    Touch_DebugReadBaseline();   /* 刷新基线诊断值(内部已限流、且仅无触摸时读) */
#endif
    /* 位置在菜单框(x 45~195)左侧之外的那条竖带里: 菜单打开时不会被盖掉。
       圆屏限制: x=11~35 处可见 y 约 35~205, 故从 y=60 起、行距 14、共 10 行到 y=186。 */
    LCD_Fill(11, 58, 35, 200, LCD_BLACK);
    LCD_CS_CLR;
    Touch_DebugNum3(11,  60, (uint16_t)(g_touch_pressed ? 1U : 0U), LCD_RED);   /* 行1 按下标志 */
    Touch_DebugNum3(11,  74, g_dbg_point_buf[0], LCD_WHITE);                    /* 行2 Byte0(格式标志) */
    Touch_DebugNum3(11,  88, g_dbg_point_buf[2], LCD_YELLOW);                   /* 行3 Byte2 = 原始X低8位 */
    Touch_DebugNum3(11, 102, g_dbg_point_buf[3], LCD_GBLUE);                    /* 行4 Byte3 = X/Y高位 */
    Touch_DebugNum3(11, 116, g_dbg_point_buf[4], LCD_WHITE);                    /* 行5 Byte4 = 原始Y低8位 */
    Touch_DebugNum3(11, 130, g_touch_x, LCD_GREEN);                             /* 行6 映射后X */
    Touch_DebugNum3(11, 144, g_touch_y, LCD_RED);                               /* 行7 映射后Y */
#if TOUCH_IC_IT7259
    /* 行8/9: 当前模式各通道 CSEL(基线补偿)的最小/最大值 —— 贴到 0 或 255 说明基线顶到边界,
       正是"轻触判不到/坐标随握持变化"的典型特征; 行10: 1=本帧读取成功 */
    Touch_DebugNum3(11, 158, g_dbg_csel_min, LCD_CYAN);
    Touch_DebugNum3(11, 172, g_dbg_csel_max, LCD_BLUE);
    Touch_DebugNum3(11, 186, (uint16_t)g_dbg_csel_ok, LCD_WHITE);
#endif
    LCD_CS_SET;
}
#endif
