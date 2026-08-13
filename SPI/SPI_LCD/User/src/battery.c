#include "battery.h"
#include "lcd.h"

// ====== 充电检测引脚定义 ======
// CHRG  PB6: 拉低=充电中 (充电IC CHRG引脚，开漏输出)
// STDBY PB5: 拉低=充满   (充电IC STDBY引脚，开漏输出)
#define CHRG_RCC    RCC_APB2Periph_GPIOB
#define CHRG_PORT   GPIOB
#define CHRG_PIN    GPIO_Pin_6
#define CHRG_EXTI_LINE      EXTI_Line6
#define CHRG_PORT_SOURCE    GPIO_PortSourceGPIOB
#define CHRG_PIN_SOURCE     GPIO_PinSource6

#define STDBY_RCC   RCC_APB2Periph_GPIOB
#define STDBY_PORT  GPIOB
#define STDBY_PIN   GPIO_Pin_5
#define STDBY_EXTI_LINE      EXTI_Line5
#define STDBY_PORT_SOURCE    GPIO_PortSourceGPIOB
#define STDBY_PIN_SOURCE     GPIO_PinSource5

// ====== 屏幕分辨率定义 ======
#ifndef LCD_WIDTH
#define LCD_WIDTH   240
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT  240
#endif

// ====== 电池图标几何参数 (基于屏幕分辨率比例计算) ======
#define BATTERY_W_RATIO      0.30f   // 电池宽度占屏幕宽度的30%
#define BATTERY_H_RATIO      0.133f  // 电池高度占屏幕高度的约13.3%
#define NUB_W_RATIO          0.033f  // 正极突起宽度占屏幕宽度的3.3%
#define NUB_H_RATIO          0.067f  // 正极突起高度占屏幕高度的6.7%

// 计算实际像素值(四舍五入)
#define BATTERY_W       ((uint16_t)(LCD_WIDTH  * BATTERY_W_RATIO + 0.5f))
#define BATTERY_H       ((uint16_t)(LCD_HEIGHT * BATTERY_H_RATIO + 0.5f))
#define BATTERY_X       ((uint16_t)((LCD_WIDTH - BATTERY_W) / 2))  // 水平居中
#define BATTERY_Y       ((uint16_t)((LCD_HEIGHT - BATTERY_H) / 2))  // 垂直居中
#define NUB_W           ((uint16_t)(LCD_WIDTH * NUB_W_RATIO))
#define NUB_H           ((uint16_t)(LCD_HEIGHT * NUB_H_RATIO))

// ====== 颜色定义 ======
#define BATTERY_FRAME_COLOR    LCD_WHITE
#define BATTERY_BLOCK_ON_COLOR LCD_GREEN
#define BATTERY_BLOCK_OFF_COLOR LCD_BLACK

/**
 * 充电电压阈值及滞回 (mV)
 *   0~4格由电压判断，5格仅由PB5(STDBY)拉低(BAT_FULL)控制
 *   滞回30mV防止电压抖动导致档位跳变
 */
#define BAT_CHARGE_HYST_MV         30U
#define BAT_CHARGE_LEVEL_1_MV    3500U  // >= 3500mV → 1格
#define BAT_CHARGE_LEVEL_2_MV    3700U  // >= 3700mV → 2格
#define BAT_CHARGE_LEVEL_3_MV    3900U  // >= 3850mV → 3格
#define BAT_CHARGE_LEVEL_4_MV    4100U  // >= 4100mV → 4格

/**
 * 游戏放电电压阈值 (mV)
 *   只降不升：降格需连续3次采样确认，升格仅充电后允许（ALLOW_RISE 也需3次确认）
 */
#define BAT_GAME_LEVEL_1_MV      3000U  // >= 3000mV → 1格
#define BAT_GAME_LEVEL_2_MV      3600U  // >= 3600mV → 2格
#define BAT_GAME_LEVEL_3_MV      3800U  // >= 3800mV → 3格
#define BAT_GAME_LEVEL_4_MV      4000U  // >= 4000mV → 4格
#define BAT_GAME_LEVEL_5_MV      4150U  // >= 4150mV → 5格

// 游戏电量保存在备份寄存器中，休眠复位后仍保持
#define BAT_GAME_BKP_MAGIC        0xB471U
#define BAT_GAME_BKP_DR_MAGIC     BKP_DR3
#define BAT_GAME_BKP_DR_STATE     BKP_DR4
#define BAT_GAME_BKP_LEVEL_MASK   0x0007U
#define BAT_GAME_BKP_ALLOW_RISE   0x0100U
#define BAT_GAME_DECREASE_CONFIRM 3       /* 升降格均需连续3次采样确认 */

// ====== 动画控制 ======
#define ANIM_FRAMES_PER_STEP  3     // 每步持续的帧数
static uint16_t anim_frame = 0;     // 全局帧计数(永远累加，不因档位变化重置)
static uint8_t  g_need_redraw = 1;  // 需要清屏重绘边框(首次/被打断时)

/**
 * @brief  开启备份寄存器时钟和写访问
 */
static void battery_backup_access_enable(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
}

/**
 * @brief  按游戏电压的精确分界计算本次实测等级
 * @param  voltage_mv  游戏运行时的电池电压，单位为 mV
 * @return 不含历史限制的实测电量等级
 */
static BatteryLevel battery_game_voltage_to_raw_level(uint16_t voltage_mv)
{
    if (voltage_mv >= BAT_GAME_LEVEL_5_MV) return BAT_LEVEL_5;
    if (voltage_mv >= BAT_GAME_LEVEL_4_MV) return BAT_LEVEL_4;
    if (voltage_mv >= BAT_GAME_LEVEL_3_MV) return BAT_LEVEL_3;
    if (voltage_mv >= BAT_GAME_LEVEL_2_MV) return BAT_LEVEL_2;
    if (voltage_mv >= BAT_GAME_LEVEL_1_MV) return BAT_LEVEL_1;
    return BAT_LEVEL_0;
}

/**
 * @brief  初始化充电检测GPIO
 *         CHRG  PB6: 上拉输入 + EXTI下降沿事件 (充电IC拉低=充电)
 *         STDBY PB5: 上拉输入 (充电IC拉低=充满)
 */
void Battery_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    EXTI_InitTypeDef EXTI_InitStructure = {0};

    RCC_APB2PeriphClockCmd(CHRG_RCC | STDBY_RCC | RCC_APB2Periph_AFIO, ENABLE);

    // CHRG PB6: 上拉输入
    GPIO_InitStructure.GPIO_Pin = CHRG_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(CHRG_PORT, &GPIO_InitStructure);

    // STDBY PB5: 上拉输入
    GPIO_InitStructure.GPIO_Pin = STDBY_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(STDBY_PORT, &GPIO_InitStructure);

    // 配置PB6 EXTI下降沿事件（运行时检测充电插入）
    GPIO_EXTILineConfig(CHRG_PORT_SOURCE, CHRG_PIN_SOURCE);
    EXTI_InitStructure.EXTI_Line = CHRG_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;      // 事件模式（非中断）
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling; // 下降沿触发
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);
}

/**
 * @brief  配置PB6 EXTI用于Standby模式唤醒
 *         Standby模式下只有WKUP(PA0)和EXTI事件+WFE能唤醒。
 *         此函数在进入Standby前调用，配置PB6下降沿事件唤醒。
 *         注意：CH32V20x硬件可能不支持EXTI从Standby唤醒，
 *         此时通过Battery_ShouldStayAwake()作为后备方案。
 */
void Battery_StandbyWakeupConfig(void)
{
    EXTI_InitTypeDef EXTI_InitStructure = {0};

    RCC_APB2PeriphClockCmd(CHRG_RCC | RCC_APB2Periph_AFIO, ENABLE);

    // CHRG PB6: 上拉输入
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = CHRG_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(CHRG_PORT, &GPIO_InitStructure);

    // STDBY PB5: 上拉输入
    GPIO_InitStructure.GPIO_Pin = STDBY_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(STDBY_PORT, &GPIO_InitStructure);

    // 配置PB6 CHRG EXTI下降沿事件
    GPIO_EXTILineConfig(CHRG_PORT_SOURCE, CHRG_PIN_SOURCE);
    EXTI_InitStructure.EXTI_Line = CHRG_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    // 配置PB5 STDBY EXTI下降沿事件
    GPIO_EXTILineConfig(STDBY_PORT_SOURCE, STDBY_PIN_SOURCE);
    EXTI_InitStructure.EXTI_Line = STDBY_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    // 清除可能挂起的EXTI事件
    EXTI_ClearFlag(CHRG_EXTI_LINE);
    EXTI_ClearFlag(STDBY_EXTI_LINE);
}

/**
 * @brief  读取充电状态（带去抖）
 * @return BAT_FULL:     STDBY拉低=充满
 *         BAT_CHARGING: CHRG拉低=充电中
 *         BAT_NORMAL:   其他情况
 */
BatteryStatus Battery_GetStatus(void)
{
    // 多次读取去抖，避免充电IC引脚抖动导致漏检
    uint8_t stdby_low = 0, chrg_low = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (GPIO_ReadInputDataBit(STDBY_PORT, STDBY_PIN) == Bit_RESET) stdby_low++;
        if (GPIO_ReadInputDataBit(CHRG_PORT, CHRG_PIN) == Bit_RESET) chrg_low++;
    }
    if (stdby_low >= 6) return BAT_FULL;
    if (chrg_low >= 6)  return BAT_CHARGING;
    return BAT_NORMAL;
}

/**
 * @brief  判断是否正在充电（用于Standby决策）
 * @return 1=正在充电，不应进入Standby
 *         0=未充电，允许进入Standby
 */
uint8_t Battery_IsCharging(void)
{
    return (GPIO_ReadInputDataBit(CHRG_PORT, CHRG_PIN) == Bit_RESET) ? 1 : 0;
}

/**
 * @brief  根据电池电压计算充电电量档位（0~4格）
 * @param voltage_mv 电池电压 (mV)
 * @return BatteryLevel 电量档位 (0~4格)
 *
 *         4格：4.00～4.15V  (4000～4150mV)
 *         3格：3.85～4.00V  (3850～4000mV)
 *         2格：3.70～3.85V  (3700～3850mV)
 *         1格：3.50～3.70V  (3500～3700mV)
 *         0格：< 3.50V
 * @note   5格仅由PB5(STDBY)拉低(BAT_FULL)控制，见Battery_DrawIcon。
 */
BatteryLevel Battery_ChargeVoltageToLevel(uint16_t voltage_mv)
{
    static BatteryLevel prev = BAT_LEVEL_0;
    uint16_t v = voltage_mv;

    if      (v >= BAT_CHARGE_LEVEL_4_MV + BAT_CHARGE_HYST_MV) prev = BAT_LEVEL_4;
    else if (v >= BAT_CHARGE_LEVEL_3_MV + BAT_CHARGE_HYST_MV) prev = BAT_LEVEL_3;
    else if (v >= BAT_CHARGE_LEVEL_2_MV + BAT_CHARGE_HYST_MV) prev = BAT_LEVEL_2;
    else if (v >= BAT_CHARGE_LEVEL_1_MV + BAT_CHARGE_HYST_MV) prev = BAT_LEVEL_1;
    else if (v <= BAT_CHARGE_LEVEL_1_MV - BAT_CHARGE_HYST_MV
             && prev >= BAT_LEVEL_1) prev = BAT_LEVEL_0;
    else if (v <= BAT_CHARGE_LEVEL_2_MV - BAT_CHARGE_HYST_MV
             && prev >= BAT_LEVEL_2) prev = BAT_LEVEL_1;
    else if (v <= BAT_CHARGE_LEVEL_3_MV - BAT_CHARGE_HYST_MV
             && prev >= BAT_LEVEL_3) prev = BAT_LEVEL_2;
    else if (v <= BAT_CHARGE_LEVEL_4_MV - BAT_CHARGE_HYST_MV
             && prev >= BAT_LEVEL_4) prev = BAT_LEVEL_3;

    return prev;
}

/**
 * @brief  按游戏放电阈值计算电量等级
 * @param  voltage_mv  游戏运行时的电池电压，单位为 mV
 * @return 游戏右上角图标使用的电量等级
 *
 *         5格：>= 4.15V    4格：>= 4.00V    3格：>= 3.80V
 *         2格：>= 3.60V    1格：>= 3.00V    0格：< 3.00V
 * @note   ALLOW_RISE 期间只追涨（立即升格），不降（防充电抖动）
 *         电压稳定到 == stored 时关闭 ALLOW_RISE。
 *         正常游戏期间只降不升，降格需连续3次采样确认。
 */
BatteryLevel Battery_GameVoltageToLevel(uint16_t voltage_mv)
{
    BatteryLevel measured =
        battery_game_voltage_to_raw_level(voltage_mv);
    BatteryLevel stored;
    uint16_t state;

    static BatteryLevel s_pending_level = BAT_LEVEL_5;
    static uint8_t     s_pending_cnt    = 0;
    static uint8_t     s_prev_rise      = 0;

    battery_backup_access_enable();
    state = BKP_ReadBackupRegister(BAT_GAME_BKP_DR_STATE);

    if (BKP_ReadBackupRegister(BAT_GAME_BKP_DR_MAGIC)
        != BAT_GAME_BKP_MAGIC
        || (state & BAT_GAME_BKP_LEVEL_MASK) > BAT_LEVEL_4) {
        stored = measured;
        s_pending_level = measured;
        s_pending_cnt    = 0;
    } else {
        stored = (BatteryLevel)(state & BAT_GAME_BKP_LEVEL_MASK);
        uint8_t allow_rise = (state & BAT_GAME_BKP_ALLOW_RISE) != 0;

        /* ALLOW_RISE 上升沿：强制重置降格待确认，防止旧计数误提交 */
        if (allow_rise && !s_prev_rise) {
            s_pending_level = measured;
            s_pending_cnt    = 0;
        }
        s_prev_rise = allow_rise;

        if (allow_rise) {
            /* 充电追涨窗口：立即升格，不降格 */
            if (measured > stored) {
                stored = measured;
            } else if (measured == stored) {
                /* 电压已稳定到当前格位，关闭追涨 */
                state &= ~BAT_GAME_BKP_ALLOW_RISE;
            }
            /* measured < stored：忽略，可能是充电抖动 */
        } else {
            /* 正常游戏：只降不升，需连续确认 */
            if (measured < stored) {
                if (measured == s_pending_level) {
                    s_pending_cnt++;
                    if (s_pending_cnt >= BAT_GAME_DECREASE_CONFIRM) {
                        stored = measured;
                        s_pending_cnt = 0;
                    }
                } else {
                    s_pending_level = measured;
                    s_pending_cnt    = 1;
                }
            } else {
                /* measured >= stored：稳定或回升（无 ALLOW_RISE 则忽略回升） */
                s_pending_level = measured;
                s_pending_cnt    = 0;
            }
        }
    }

    BKP_WriteBackupRegister(BAT_GAME_BKP_DR_MAGIC,
                            BAT_GAME_BKP_MAGIC);
    BKP_WriteBackupRegister(BAT_GAME_BKP_DR_STATE,
                            (uint16_t)stored | (state & BAT_GAME_BKP_ALLOW_RISE));
    return stored;
}

/**
 * @brief  充电后写入 ALLOW_RISE 标记，授权游戏电量升格
 * @note   仅充电屏调用（main.c:1149）。标记由 Battery_GameVoltageToLevel 在
 *         连续3次采样确认升格后消费清零，同时重置待确认计数。
 */
void Battery_GameLevelAllowIncrease(void)
{
    uint16_t state;

    battery_backup_access_enable();
    if (BKP_ReadBackupRegister(BAT_GAME_BKP_DR_MAGIC)
        != BAT_GAME_BKP_MAGIC) {
        return;
    }

    state = BKP_ReadBackupRegister(BAT_GAME_BKP_DR_STATE);
    if ((state & BAT_GAME_BKP_LEVEL_MASK) <= BAT_LEVEL_4
        && (state & BAT_GAME_BKP_ALLOW_RISE) == 0) {
        BKP_WriteBackupRegister(BAT_GAME_BKP_DR_STATE,
                                state | BAT_GAME_BKP_ALLOW_RISE);
    }
}

/**
 * @brief  绘制电池图标
 *         5个方块，根据电量档位显示填充动画（左起填充）：
 *         1格：█ → ██ → ███ → ████ → █████ → 循环
 *         2格：██ → ███ → ████ → █████ → 循环
 *         3格：███ → ████ → █████ → 循环
 *         4格：████ → █████ → 循环
 *         充满：5个全亮
 *
 * @param status 充电状态
 * @param level  电量档位
 */
void Battery_DrawIcon(BatteryStatus status, BatteryLevel level)
{
    uint16_t x = BATTERY_X;
    uint16_t y = BATTERY_Y;
    uint16_t w = BATTERY_W;
    uint16_t h = BATTERY_H;

    // 从电池框尺寸自动推导方块尺寸(边框壁厚2px, 5个方块+4间隙+2边距=电池宽度)
    uint16_t blk_pad = 1;                           // 内边距
    uint16_t blk_gap = 1;                           // 方块间隙
    uint16_t blk_w   = (w - 2 * blk_pad - 4 * blk_gap) / 5; // 方块宽度
    uint16_t blk_h   = h - 2 * blk_pad - 2;        // 方块高度(扣除上下边框各2px)

    // 帧计数驱动动画(永远累加，不受档位变化影响)
    anim_frame++;

    // ADC档位更新：走完当前动画周期后才切换，避免中途跳变
    static BatteryLevel displayed_level = BAT_LEVEL_0;
    static BatteryLevel pending_level   = BAT_LEVEL_0;
    if (level != displayed_level) {
        pending_level = level;                       // ADC请求新档位
    }
    uint8_t n = (uint8_t)displayed_level;            // 当前显示的档位
    uint8_t cycle_len = (n > 0) ? (6 - n) : 6;     // 0格:6步(0-1-2-3-4-5), 其他:(6-n)步
    uint8_t cycle_step = (anim_frame / ANIM_FRAMES_PER_STEP) % cycle_len;
    if (cycle_step == 0 && pending_level != displayed_level) {
        displayed_level = pending_level;             // 周期结束，切换到新档位
        n = (uint8_t)displayed_level;
        anim_frame = 0;                              // 重置帧计数，从新档位第0步开始
        cycle_step = 0;
    }

    uint8_t blocks_lit[5] = {0};  // 5个方块亮灭标志

    if (status == BAT_FULL) {
        for (uint8_t i = 0; i < 5; i++) blocks_lit[i] = 1;
    } else {
        // 填充动画：方块数从n递增到5，循环（含0格:0→1→2→3→4→5）
        uint8_t lit = n + cycle_step;
        for (uint8_t i = 0; i < lit; i++) {
            blocks_lit[i] = 1;
        }
    }

    // 打断/首次时清屏重绘边框，之后只增量更新方块不闪屏
    if (g_need_redraw) {
        g_need_redraw = 0;
        LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
        LCD_Fill(x, y, x + w, y + 1, BATTERY_FRAME_COLOR);                 // 上边
        LCD_Fill(x + w - 1, y, x + w, y + h, BATTERY_FRAME_COLOR);         // 右边
        LCD_Fill(x, y + h - 1, x + w, y + h, BATTERY_FRAME_COLOR);         // 下边
        LCD_Fill(x, y, x + 1, y + h, BATTERY_FRAME_COLOR);                 // 左边

        uint16_t nub_x = x + w;
        uint16_t nub_y = y + (h - NUB_H) / 2;
        LCD_Fill(nub_x, nub_y, nub_x + NUB_W, nub_y + NUB_H, BATTERY_FRAME_COLOR);
        LCD_Fill(nub_x + 1, nub_y + 1, nub_x + NUB_W - 1, nub_y + NUB_H - 1, BATTERY_BLOCK_OFF_COLOR);
    }

    // 增量更新方块：只改变化的，不闪屏
    static uint8_t prev_blocks[5] = {0};
    uint16_t block_y = y + blk_pad;
    for (uint8_t i = 0; i < 5; i++) {
        if (prev_blocks[i] != blocks_lit[i]) {
            uint16_t bx = x + blk_pad + i * (blk_w + blk_gap);
            uint16_t color = blocks_lit[i] ? BATTERY_BLOCK_ON_COLOR : BATTERY_BLOCK_OFF_COLOR;
            LCD_Fill(bx, block_y, bx + blk_w, block_y + blk_h, color);
            prev_blocks[i] = blocks_lit[i];
        }
    }
}

/**
 * @brief  打断动画时调用，强制下次绘制时清屏重绘边框
 */
void Battery_ResetDisplay(void)
{
    g_need_redraw = 1;
}
