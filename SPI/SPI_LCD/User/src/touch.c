#include "touch.h"
#include "debug.h"
#include "settings.h"

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

/**
 * @brief 全局状态变量
 * @note  g_touch_int_flag: INT触发标志, ISR置1, Touch_IsPressed清零
 */
static uint16_t g_touch_x = 0;
static uint16_t g_touch_y = 0;
uint8_t  g_touch_pressed = 0;  /* 触摸状态, 1=有触摸, 0=无触摸 */
volatile uint8_t g_touch_int_flag = 0;
static uint16_t g_touch_last_raw_x = 0;
static uint16_t g_touch_last_raw_y = 0;
static uint16_t g_touch_pending_raw_x = 0;
static uint16_t g_touch_pending_raw_y = 0;
static uint8_t g_touch_have_valid_sample = 0;
static uint8_t g_touch_have_pending_sample = 0;

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

    g_touch_int_flag = 0;
    g_touch_pressed = 0;
    g_touch_have_valid_sample = 0;
    g_touch_have_pending_sample = 0;
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
        uint8_t fingers = 0;
        uint8_t touch_released;

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

        if (touch_released
            && GPIO_ReadInputDataBit(TOUCH_INT_PORT, TOUCH_INT_PIN) == Bit_SET) {
            stable_count++;
        } else {
            stable_count = 0;
        }

        IWDG_ReloadCounter();
        Delay_Ms(5);
    }
}

/**
 * @brief 坐标映射: raw 映射到 0~239
 * @param  raw    原始坐标 (0~240)
 * @param  mirror 是否镜像
 * @retval 映射后坐标 (0~239)
 */
static uint16_t MapCoord(uint16_t raw, uint8_t mirror)
{
    uint16_t mapped;
    if (mirror)
        raw = TOUCH_MAX_RAW - raw;
    mapped = (uint32_t)raw * TOUCH_MAP_MAX / TOUCH_MAX_RAW;
    if (mapped >= TOUCH_MAP_MAX) mapped = TOUCH_MAP_MAX - 1;
    return mapped;
}

static uint16_t Touch_AbsDiff(uint16_t a, uint16_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static void Touch_ResetCoordFilter(void)
{
    g_touch_have_valid_sample = 0;
    g_touch_have_pending_sample = 0;
}

static uint8_t Touch_TryApplyCoords(uint16_t raw_x, uint16_t raw_y)
{
    uint16_t dx;
    uint16_t dy;

    if (raw_x > TOUCH_MAX_RAW || raw_y > TOUCH_MAX_RAW) {
        g_touch_have_pending_sample = 0;
        return 0;
    }

    if (g_touch_have_valid_sample) {
        dx = Touch_AbsDiff(raw_x, g_touch_last_raw_x);
        dy = Touch_AbsDiff(raw_y, g_touch_last_raw_y);
        if (dx > TOUCH_JUMP_LIMIT_RAW || dy > TOUCH_JUMP_LIMIT_RAW) {
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

    {
        uint16_t mx = MapCoord(raw_x, 0);
        uint16_t my = MapCoord(raw_y, 0);
        /* 根据屏幕方向旋转触摸坐标 */
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

    /* 2. 硬件复位时序 (拉低再拉高) */
    GPIO_ResetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Ms(10);
    GPIO_SetBits(TOUCH_RST_PORT, TOUCH_RST_PIN);
    Delay_Ms(100);   /* CST816S 复位要求: 低电平>50ms, 等待100~200ms */

    /* 3. 验证I2C通信: 读芯片ID (0xA7, 预期0xB4或0xB5), 失败重试 */
    {
        uint8_t chip_id, retry = 0;
        do {
            Delay_Ms(10);
        } while (!CST816S_ReadReg(CST816S_REG_CHIPID, &chip_id, 1) && ++retry < 5);
    }

    /* 4. 配置中断控制寄存器: 0xFA = 0x40 (启用INT触发) */
    CST816S_WriteReg(CST816S_REG_IRQCTL, CST816S_IRQ_ENABLE);
    Delay_Ms(10);

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
    CST816S_WriteReg(CST816S_REG_LP_SCAN_TH,
                     CST816S_LP_SCAN_THRESHOLD);
    CST816S_WriteReg(CST816S_REG_AUTOSLEEP, CST816S_SLEEP_ENABLE);
}

/*********************************************************************
 * @brief  禁用CST816S自动休眠 (初始化/唤醒后调用, 保证触摸即时响应)
 * @param  无
 * @retval 无
 **********************************************************************/
void CST816S_SleepDisable(void)
{
    CST816S_WriteReg(CST816S_REG_AUTOSLEEP, CST816S_SLEEP_DISABLE);
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
