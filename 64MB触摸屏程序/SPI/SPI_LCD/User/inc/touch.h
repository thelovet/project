#ifndef __TOUCH_H__
#define __TOUCH_H__

#include "ch32v20x.h"

/**
 * @brief 触摸 IC 选择宏: 1=IT7259(新触摸IC), 0=CST816S(旧触摸IC)
 * @note  两种 IC 接线完全相同(RST=PB8 / INT=PA8 / SCL=PB10 / SDA=PB11);
 *        差异在 I2C 地址与读点协议: CST816S 用 FT6x36 寄存器(0x02/0x03~0x06);
 *        IT7259 用点数据报告(从 0xE0 读, 12位坐标 + Point Information 判抬手), 纯I2C轮询。
 */
#define TOUCH_IC_IT7259   0

/**
 * @brief 触摸坐标调试显示开关: 1=屏幕左上角实时显示(行1=按下标志, 行2=X, 行3=Y)
 * @note  仅调试用; 调试完请改回 0, 避免占 Flash 和遮挡画面(约 270B, 当前 Flash 还有余量)
 */
#define TOUCH_DEBUG_SHOW   0

#if TOUCH_DEBUG_SHOW
void Touch_DebugShow(void);
#define TOUCH_DEBUG_FRAME()   Touch_DebugShow()
#else
#define TOUCH_DEBUG_FRAME()
#endif

#define CST816S_REG_LP_SCAN_TH       0xF5
#define CST816S_LP_SCAN_THRESHOLD     32U

/**
 * @brief CST816S 触摸引脚定义
 */
#define TOUCH_RST_RCC     RCC_APB2Periph_GPIOB
#define TOUCH_RST_PORT    GPIOB
#define TOUCH_RST_PIN     GPIO_Pin_8          // RST1: PB8

#define TOUCH_INT_RCC     RCC_APB2Periph_GPIOA
#define TOUCH_INT_PORT    GPIOA
#define TOUCH_INT_PIN     GPIO_Pin_8          // INT: PA8 (EXTI8)

/**
 * @brief 软件I2C引脚 (SCL:PB10, SDA:PB11)
 */
#define TOUCH_SCL_PORT    GPIOB
#define TOUCH_SCL_PIN     GPIO_Pin_10
#define TOUCH_SDA_PORT    GPIOB
#define TOUCH_SDA_PIN     GPIO_Pin_11

/**
 * @brief 触摸 IC 差异寄存器 (I2C地址/芯片ID/中断/休眠), 由 TOUCH_IC_IT7259 宏切换
 * @note  IT7259 寄存器表兼容 FT6x36: 0x80~0x89 为灵敏度/时序阈值, 0xA4=G_MODE, 0xA5=G_PMODE。
 */
#if TOUCH_IC_IT7259
/* ---- IT7259 (新): Command/缓冲区协议, 非FT6x36寄存器 ----
   点数据报告(Point Data Report)从 0xE0(Point Information Buffer)读出, 14字节:
     Byte0 [7:4]=Format Tag(0000b), bit3=Finger, [2:0]=Point Information(Point0~2有效)
     Byte2 = Point0 X[7:0]
     Byte3 [3:0] = Point0 X[11:8], [7:4] = Point0 Y[11:8]
     Byte4 = Point0 Y[7:0]
   Point Information==0 即无触摸(抬手)。中断非必须, 这里用纯I2C轮询(与厂家例程一致)。 */
#define CST816S_ADDR               0x46       // 7位地址 (手册6.2.1 默认46h)
#define CST816S_REG_CHIPID         0xE0       // 通信探测: 读点数据报告寄存器
#define CST816S_REG_QUERY_BUF      0x80       // Query Buffer(只读, 缓冲区索引100b): bit7-6=包信息状态(1xb=有新包, 01b=无新包但手指仍按下, 00b=无触摸), bit1-0=命令状态(00b=Done)
#define CST816S_REG_CMD_BUF        0x20       // Command Buffer(只写, 索引001b): 命令格式 = [命令, 子命令/长度, 参数...] 全部小端
#define CST816S_REG_CMD_RSP        0xA0       // Command Response Buffer(只读, 索引101b): 命令执行结果
#define CST816S_REG_POINT          0xE0       // Point Information Buffer: 点数据报告
#define CST816S_POINT_LEN          32         // 与厂家例程一致: 每次读满32字节, 读少了缓冲不推进(一直读到旧包)
#define CST816S_POINT_POLL_TIMES   1          // 每帧连读次数(实测与"移动才更新"无关, 保持1次省I2C时间)
/* 算法参数(手册5.3.17 命令0x15): 决定坐标上报密度。默认阈值下"移动 distance>resolution*阈值/1000"才上报新坐标,
   表现为滑动时坐标一格一格跳(如118→129), 故初始化时下发下面3条命令把阈值调小。 */
#define CST816S_IT7259_TH_ENABLE   1          // 是否在 Touch_Init 里下发算法参数命令
#define CST816S_IT7259_POINT_TH    0          // Point Threshold(0x1D): 移动> resolution*值/1000 才上报, 0=每个采样点都上报
#define CST816S_IT7259_SCAN_PERIOD 0x05       // Engine Scan Period(0x18): 引擎扫描周期, 单位ms
#define CST816S_IT7259_LAST_POINT  0x01       // Report the Last Point(0x06): bit0=1输出最后一点, bit7-1=跳过范围(0=不跳过, 否则要移动够远才补最后一点)
/* Gain Setting(手册5.3.17.2 命令0x15/子命令0x07, 1字节): bit7-4=FW CDC Compensation,
   bit3-2=AFE Gain @AC Shielding Mode(浮空/放桌上), bit1-0=AFE Gain @Ground Mode(接地/拿手里), 0~3越大越灵敏。
   实测"拿着只能到200、不拿到239"且"轻触时按下标志会跳" → 两档都拉高试。
   手册只给位定义、无默认值/量程, 只能台阶式试: 0x0F(两档最高) → 0x0A → 0x05, 找到够用又不误触的值。 */
#define CST816S_IT7259_GAIN        0x0F
#define CST816S_IT7259_GAIN_EN     1          // 必须为1: 实测关掉(0=不下发增益)后"触摸非常不灵敏", 说明芯片默认增益对本屏偏低
/* 信号链前端(手册5.3.22.2 / 5.3.22.5): 针对"轻触判不到按下"且基线(CSEL)正常的现象。
   0x1A/0x02 模拟前端滤波系数(平均长度): 0x00=默认 / 0x03=96pt / 0x05=160pt / 0x06=192pt
                                         / 0x08=256pt / 0x0A=320pt, pt 越大越平滑(SNR 好, 响应略慢)
   0x1A/0x04 引擎控制: 0x03=切到接地模式(拿在手里) / 0x04=切到屏蔽模式(放桌上) */
#define CST816S_IT7259_AFE_FILTER  0x03       // 先温和(96pt); 不够再试 0x05 / 0x06
#define CST816S_IT7259_ENGINE_MODE 0x03       // 03=接地模式(正常拿在手里用) / 04=屏蔽模式
/* Command 0x04: 在 Active/Idle 间切换(资料写"切换", 疑为无参数命令) */
#define CST816S_REG_POWER_MODE     0x04       // 无参数命令: 发一次切换一次模式
#define CST816S_POWER_ACTIVE       0x01        // 若改用带参数写法, 用这个值
#define CST816S_IT7259_WRITE_TH    0           // 是否写厂家例程那套 0x80~0x89 阈值(IT7259是Command协议, 写这些未知地址很可能把"坐标上报阈值"改大, 默认不写)
#define CST816S_IT7259_USE_POINT_REPORT 1      // 读点来源: 1=0xE0点数据报告(实测能读到点, 但坐标随事件更新会跳); 0=FT6x36风格 0x02+0x03~0x06(实测IT7259读不到点→唤醒确认失败直接回待机, 勿用)
/* 灵敏度/时序阈值 (厂家ESP32例程 touch_CTP.c 的配置值) */
#define CST816S_REG_THGROUP        0x80       // 触摸检测阈值 70
#define CST816S_REG_THPEAK         0x81       // 峰值检测阈值 60
#define CST816S_REG_THCAL          0x82       // 对焦阈值 16
#define CST816S_REG_THWATER        0x83       // 防水阈值 60
#define CST816S_REG_THTEMP         0x84       // 温度补偿阈值 10
#define CST816S_REG_THDIFF         0x85       // 差值阈值 20
#define CST816S_REG_TIME_MONITOR   0x87       // 进入Monitor延时 2(s)
#define CST816S_REG_PERIODACTIVE   0x88       // Active周期 12(ms)
#define CST816S_REG_PERIODMONITOR  0x89       // Monitor空闲超时 40(ms)
#else
/* ---- CST816S (旧) ---- */
#define CST816S_ADDR            0x15           // 7位地址
#define CST816S_REG_CHIPID      0xA7           // 芯片ID (预期0xB4或0xB5)
#define CST816S_REG_IRQCTL      0xFA           // 中断控制 (必须配置!)
//   0xFA: [7]=使能 [6]=触摸产生中断方式 [5:0]=保留
#define CST816S_IRQ_ENABLE      0x40           // 使能并周期产生中断
#define CST816S_REG_AUTOSLEEP   0xFE           // 自动休眠控制
#define CST816S_SLEEP_ENABLE    0x00           // 启用自动休眠(无触摸约2秒后休眠)
#define CST816S_SLEEP_DISABLE   0x01           // 禁用自动休眠(始终保持活跃)
/* 灵敏度阈值(FT6x36 兼容寄存器, 816S/D/T 通用): 值越小越灵敏。
   0x80=触摸检测阈值(816T 实测出厂默认 0)、0x85=差值阈值。 */
#define CST816S_REG_THGROUP     0x80           // 触摸检测阈值
#define CST816S_REG_THDIFF      0x85           // 差值阈值
/* 816 系列灵敏度微调: 只改"触摸检测阈值"这一项, 便于 A/B(改多了分不清是哪项起作用)。
   ★ 816T 实测: 0x80 的出厂默认就是 0(=阈值最小/最灵敏), 写 20 反而更钝 → 默认关闭写入。
   只有在别的批次上读到默认值明显偏大(如 70)时, 才把它打开并把 TOUCH_816_THGROUP 调到比默认小。 */
#define TOUCH_816_TH_ENABLE     0              // 0=保持芯片默认(816T 默认已是最灵敏)
#define TOUCH_816_THGROUP       20             // 仅 ENABLE=1 时生效; 必须小于芯片默认值才有意义
#endif

/**
 * @brief 触摸坐标寄存器 (两种 IC 相同)
 */
#define CST816S_REG_GESTURE    0x01           // 手势ID
#define CST816S_REG_FINGERNUM  0x02           // 手指数量
#define CST816S_REG_XH         0x03           // X坐标高4位 [11:8]
#define CST816S_REG_XL         0x04           // X坐标低8位 [7:0]
#define CST816S_REG_YH         0x05           // Y坐标高4位 [11:8]
#define CST816S_REG_YL         0x06           // Y坐标低8位 [7:0]

/**
 * @brief 触摸状态结构体
 */
typedef struct {
    uint8_t  pressed;       // 1=有触摸, 0=无触摸
    uint16_t x;             // X坐标
    uint16_t y;             // Y坐标
} TouchState;

/**
 * @brief INT标志位 (ISR置位)
 */
extern volatile uint8_t g_touch_int_flag;
extern uint8_t g_touch_pressed;

/**
 * @brief 函数声明
 */
void    Touch_Init(void);
void    Touch_WakeResumeInit(void);              // 唤醒确认前恢复I2C，不复位CST816S
uint8_t Touch_IsPressed(void);                    // INT trigger: press->1, release->0, cache coords
void    Touch_Scan(TouchState *state);            // read cached coords (no I2C)
void    Touch_UpdateCoords(void);                 // poll I2C for real-time coords (during hold)
void    CST816S_SleepEnable(void);                // 启用自动休眠 (待机前调用)
void    CST816S_SleepDisable(void);               // 禁用自动休眠 (唤醒后调用)

/**
 * @brief Standby唤醒配置
 */
void    Touch_StandbyWakeupConfig(void);          // 配置PA8 EXTI事件唤醒
void    Touch_WaitWakeInputsReleased(void);        // 等待触摸完全释放

#endif
