/**
 * @brief  待机低功耗模式说明（PA8 触摸 + PB5/PB6 充电唤醒）
 * @note   参考 EVT/EXAM/PWR/Standby_Mode
 *
 *    管脚分配:
 *    FLASH_CS   -- PA2   (1脚)
 *    FLASH_DO   -- PA6(SPI1_MISO)(2脚)
 *    FLASH_WP   -- 3.3V (3脚)
 *    FLASH_DI   -- PA7(SPI1_MOSI) (5脚)
 *    FLASH_CLK  -- PA5(SPI1_SCK) (6脚)
 *    FLASH_HOLD -- 3.3V (7脚)
 *    VCC -- 3.3V (8脚)
 *    GND -- GND (4脚)
 *
 *    LCD_LED    -- PB9
 *    LCD_DC     -- PB0
 *    LCD_RST    -- PB1
 *    LCD_CS     -- PB12
 *    LCD_SCK    -- PB13(SPI2_SCK)
 *    LCD_MOSI   -- PB15(SPI2_MOSI)
 *
 *    触摸 RST   -- PB8
 *    触摸 INT   -- PA8
 *    触摸 SDA   -- PB11 (I2C2_SDA)
 *    触摸 SCL   -- PB10 (I2C2_SCL)
 *
 *    KEY        -- PA0 (旧版预留，当前固件未启用)
 *
 *    电池检测：
 *    CHRG       -- PB6 (低=充电中)
 *    STDBY      -- PB5 (低=充满)
 *
 *    电压测量开关：
 *    VOLT_SW    -- PA15 (低=允许测量, 高=关闭)
 *
 *    电池电压：
 *    BAT_ADC    -- PA1 (ADC1_Channel_1, 分压输入, 为电池电压的一半)
 *                  4.2V 满电 → 2.1V ADC 输入
 *                  仅当 VOLT_SW 为低时才测量
 *    语音 IC：
 *    VOICE_SW   -- PB3 (低=允许语音, 高=关闭)
 */

//恢复正常播放，运行 switch_mode.ps1 game
//进入烧录模式运行 burn_all.ps1
#include "debug.h"
#include "string.h"
#include "lcd.h"
#include "flash.h"
#include "config.h"
#include "decompress.h"    // 视频解压缩库
#include "timer.h"        // 定时器驱动
#include "ch32v20x_pwr.h" // 电源控制相关
#include "ch32v20x_bkp.h" // 备份寄存器相关
#include "adc.h"          // 电池检测ADC相关
#include "voice.h"        // 语音IC驱动
#include "display.h"       // 显示相关(ShowNum/ShowLowBattery)
#include "battery.h"       // 电池管理
#include "touch.h"         // 触摸屏驱动
#include "sokoban.h"       // 推箱子游戏
#include "d2048.h"         // 2048游戏
#include "tetris.h"         // 俄罗斯方块游戏
#include "klotski.h"        // 华容道游戏
#include "fish.h"           // 大鱼吃小鱼游戏
#include "menu.h"            // 菜单界面显示
#include "game_save.h"       // 游戏状态存档
#include "sok_img_win.h"    // 32x32中文字库
#include "settings.h"       // 设置界面
#include "flash_store.h"    // Flash数据存储程序(字库/图片资源烧录)

 // ====== 调试配置 ======
 // 调试模式：保留SWJ调试接口。如果不需要调试，注释掉下面这行即可省电
#define DEBUG_SWJ_KEEP
// 注意：屏幕背光极性在 config.h 中定义 TEST_SCREEN_BACKLIGHT，此处不修改背光相关宏

// 烧录模式宏定义在 config.h 中切换:
//   FLASH_DATA_STORE_MODE  = 全量自动烧录字库/图片到外部Flash末尾(绿色100%)
//   VIDEO_CHUNK_STORE_MODE = 烧录嵌入固件的视频数据块到外部Flash地址0(视频区域)
// 两个宏都注释即恢复正常游戏程序。

#define HOLD_THRESHOLD    40   /* 触摸按住移动阈值 */
#define RELEASE_THRESHOLD 15   /* 触摸释放移动阈值 */
#define STANDBY_WAKE_ARMED_MAGIC  ((uint16_t)0xE817)
#define STANDBY_TOUCH_WAKE_MAGIC  ((uint16_t)0xE818)
#define STANDBY_CHARGE_WAKE_MAGIC ((uint16_t)0xE819)
#define CHARGE_EXIT_CONFIRM_COUNT 5U  /* 连续约500ms未充电才确认拔出 */
#define CHARGE_EXIT_CHECK_MS     100U

// 切换视频用的Standby模式RAM变量
uint8_t next_bank = 0x00;
uint16_t next_higvh = 0x00;
uint16_t next_high = 0x00;
uint16_t next_low = 0x00;

uint8_t current_state = 0;//播放状态0:视频播放1:菜单显示
uint8_t first_boot = 1;           // 首次启动标志

/* ================================================================
 *  触摸相关变量
 * ================================================================ */
MainMode s_main_mode = MAIN_MODE_VIDEO;
static uint8_t  s_menu_drawn = 0;

static uint8_t  s_touch_down = 0;
static uint16_t s_touch_start_y = 0;
static uint8_t  s_touch_started_top = 0;    /* 触摸起点是否在顶部区域 */
static uint8_t  s_touch_started_bottom = 0; /* 触摸起点是否在底部区域 */
static uint8_t  s_game_inited = 0;         /* 当前游戏是否已初始化 */
static uint8_t  s_menu_dragging = 0;        /* 手指拖拽菜单中 */
static uint8_t  s_menu_sliding = 0;         /* 松手后菜单自动下滑动画中 */
static uint8_t  s_drag_rel_cnt = 0;         /* 拖拽释放防抖: 连丢2帧才确认 */
static int16_t  s_menu_drag_y = -230;       /* 拖拽/动画中菜单顶部Y坐标 (-MENU_H) */
static int16_t  s_menu_drag_y_prev = -999;  /* 上一帧已绘制的Y (变化检测) */
static uint16_t s_menu_drag_touch_y = 0;    /* 上一帧拖拽触摸Y */
static uint16_t s_menu_drag_max_y = 0;     /* 本次拖拽手指到达的最远Y */
static uint8_t  s_video_menu_dragging = 0;   /* 视频上划菜单拖拽中 */
int16_t  s_video_menu_slide_y = 240;  /* 视频上划菜单顶部Y (初始在屏幕底部外) */
static uint16_t s_video_menu_drag_start_y = 0; /* 视频菜单拖拽起始触摸Y */
uint8_t  s_video_menu_visible = 0;     /* 视频上划菜单已完全可见 */
static uint8_t  s_m2v_swipe = 0;              /* 下划菜单→上划菜单打断中 */
uint8_t  vm_touch_down = 0;            /* 视频块上划检测触摸追踪 */
static uint16_t vm_start_x = 0, vm_start_y = 0;
static uint8_t  vm_sliding = 0;               /* 视频上划菜单松手回滑动画中 */
static SettingsMode s_settings_mode = SETTINGS_NONE; /* 当前设置界面 */
static TouchState ts;                        /* 触摸状态, Touch_GetXY()更新 */

static uint8_t  g_charge_pending = 0;       /* 充电倒计时中 */
static uint32_t g_charge_pending_time = 0;   /* 充电倒计时起点 */

static uint32_t s_game_last_activity_us = 0;
static MainMode s_game_sleep_mode = MAIN_MODE_VIDEO;
static uint8_t s_game_wait_release = 0;
static uint8_t s_game_release_count = 0;
static uint8_t s_wake_wait_release = 0;
static uint8_t s_wake_release_count = 0;
static uint8_t s_standby_touch_wake = 0;
static uint8_t s_standby_charge_wake = 0;
static uint8_t s_standby_unknown_wake = 0;


static void IWDG_Init(void);

/**
 * @brief  获取触摸坐标到变量 ts (调用 Touch_Scan)
 */
static void Touch_GetXY(void)
{
    Touch_Scan(&ts);
}


/**
 * @brief  恢复待处理的游戏存档
 * @return 1 表示恢复成功，0 表示没有待恢复存档或存档无效
 */
static uint8_t GameRestore_ApplyPending(void)
{
    MainMode restore_mode = MAIN_MODE_VIDEO;
    GameSavePendingResult result = GameSave_RestorePending(&restore_mode);

    if (result == GAME_SAVE_PENDING_NONE) return 0;
    if (result == GAME_SAVE_PENDING_INVALID) {
        s_main_mode = MAIN_MODE_VIDEO;
        s_game_inited = 0;
        s_game_wait_release = 0;
        s_game_release_count = 0;
        return 0;
    }

    s_main_mode = restore_mode;
    s_game_inited = 1;
    s_menu_drawn = 0;
    s_touch_down = 0;
    s_game_wait_release = 1;
    s_game_release_count = 0;
    s_game_sleep_mode = s_main_mode;
    s_game_last_activity_us = GetTimerCount();
    if (!s_standby_charge_wake) {
        LCD_Backlight_PWM_SetDuty(g_brightness);
    }
    return 1;
}


/**
 * @brief  保存当前游戏并进入待机流程
 */
static void GameAutoSleep_Enter(void)
{
    LCD_Backlight_PWM_Stop();
#ifndef DEBUG_NO_STANDBY
    IWDG_Disable();
#endif
    if (GameSave_IsSupportedMode(s_main_mode)) {
        if (!GameSave_Write(s_main_mode)) {
#ifndef DEBUG_NO_STANDBY
            IWDG_Init();
#endif
            LCD_Backlight_PWM_Init();
            LCD_Backlight_PWM_SetDuty(g_brightness);
            s_game_last_activity_us = GetTimerCount();
            return;
        }
        GameSave_SaveGameWakeState(s_main_mode);
    }
    System_PrepareForStandby();
    NVIC_SystemReset();
}

/**
 * @brief  统计游戏无触摸时间，并隔离恢复游戏时的唤醒触摸
 * @return 1 表示本轮跳过游戏更新，0 表示可以继续处理游戏
 */
static uint8_t GameAutoSleep_AfterTouch(void)
{
    uint32_t now = GetTimerCount();
    uint8_t is_game = GameSave_IsSupportedMode(s_main_mode);

    /* 纯视频播放不休眠, 菜单拖拽/弹出/设置界面/游戏中允许休眠 */
    if (s_main_mode == MAIN_MODE_VIDEO && s_settings_mode == SETTINGS_NONE
        && !s_video_menu_visible && !s_video_menu_dragging
        && !s_menu_dragging) {
        s_game_last_activity_us = now;
        return 0;
    }

    if (s_game_sleep_mode != s_main_mode) {
        s_game_sleep_mode = s_main_mode;
        s_game_last_activity_us = now;
        s_game_wait_release = 0;
        s_game_release_count = 0;
    }

    if (is_game && s_game_wait_release) {
        if (ts.pressed) {
            s_game_release_count = 0;
            return 1;
        }
        if (++s_game_release_count < 2) return 1;
        s_game_wait_release = 0;
        s_game_release_count = 0;
        s_game_last_activity_us = now;
        return 1;
    }

    {
        static uint8_t  s_last_pressed = 0;
        static uint16_t s_last_x = 0, s_last_y = 0;
        if (ts.pressed && (s_settings_mode != SETTINGS_NONE || is_game || s_video_menu_visible || s_video_menu_dragging || s_menu_dragging)) {
            if (ts.x != s_last_x || ts.y != s_last_y) {
                s_game_last_activity_us = now;   /* 坐标变化才保活 */
            }
            s_last_x = ts.x;
            s_last_y = ts.y;
        } else if (!ts.pressed && s_last_pressed) {
            s_game_last_activity_us = now;       /* 松开时重置 */
        }
        s_last_pressed = ts.pressed;
    }
    if ((uint32_t)(now - s_game_last_activity_us)
        >= ((uint32_t)g_sleep_times[g_sleep_time_index] * 1000000UL)) {
        GameAutoSleep_Enter();
        return 1;
    }

    return 0;
}

/**
 * @brief  设置页面/菜单交互中调用, 保持不休眠
 */
void GameAutoSleep_KeepAlive(void)
{
    s_game_last_activity_us = GetTimerCount();
}

/*********************************************************************
 * @brief  初始化所有GPIO为上拉模式（降低功耗）
 **********************************************************************/
static void GPIO_ALL_IPU(void) {
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
        RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_All;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_Init(GPIOD, &GPIO_InitStructure);
}

static void LCD_PWR_Init(void) {
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };//GPIO初始化结构体
    RCC_APB2PeriphClockCmd(LCD_PWR_RCC | LCD_LED_RCC, ENABLE);//使能LCD_PWR+背光时钟

    /* PB4: LCD电源控制, 先置ODR高(关)再切推挽输出, 防止上电瞬间拉低 */
    LCD_PWR_PORT->BSHR = LCD_PWR_PIN;  // ODR=1 (切输出前)
    GPIO_InitStructure.GPIO_Pin = LCD_PWR_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_PWR_PORT, &GPIO_InitStructure);

    /* PB9: 背光引脚，仅置ODR高(关)防止闪亮，GPIO初始化由LCD_Backlight_PWM_Init完成 */
    LCD_LED_SET;  // BSHR=ODR置1
}

static void IWDG_Init(void)//独立看门狗初始化，超时约1s
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);//解锁寄存器
    IWDG_SetPrescaler(IWDG_Prescaler_256);//设置预分频256
    IWDG_SetReload(155);//设置重载值 (~1s: (155+1)*256/40000≈999ms)
    //IWDG_ReloadCounter();//喂狗
    IWDG_Enable();//启动看门狗
}

/*********************************************************************
 * @brief  Standby模式引脚配置
 *         PA0/PA1设为模拟输入，PA2设为推挽输出(Flash CS)
 *         PB4设为推挽输出(LCD电源), PB5/PB6设为上拉输入(CHRG/STDBY)
 *         PB9设为推挽输出(LED), PB12设为推挽输出(LCD_CS)
 *         其余引脚全部设为模拟输入(降低功耗)
 **********************************************************************/
static void Standby_PinConfig(void) {
    // GPIOA CFGLR: PA0/PA1=AIN(0x0) PA2=PP50(0x3) PA3~7=AIN(0x0)
    GPIOA->CFGLR = 0x00000300;
    GPIOA->BSHR = GPIO_Pin_2;       // PA2置高(Flash CS)

    // GPIOA CFGHR: PA8~15=AIN(0x0)
    GPIOA->CFGHR = 0x00000000;

    // PB4设为推挽输出(LCD电源), PB5/PB6=IPU(0x8), PB7=AIN(0x0)
    GPIOB->CFGLR = 0x08830000;
#ifdef KEEP_LCD_POWER_IN_STANDBY
    GPIOB->BCR = GPIO_Pin_4;       // PB4拉低(保持LCD供电, 防止触摸误触发)
#else
    GPIOB->BSHR = GPIO_Pin_4;       // PB4置高(LCD断电)
#endif

    // GPIOB CFGHR: PB8=AIN(0x0) PB9=PP50(0x3) PB10~11=AIN(0x0) PB12=PP50(0x3) PB13~15=AIN(0x0)
    GPIOB->CFGHR = 0x00030030;
    LCD_LED_SET;                                 // 关背光(避免待机闪亮/降低功耗)
    GPIOB->BSHR = GPIO_Pin_12;                  // PB12置高(LCD_CS)
}

/**
 * @brief  读取EXTI标志寄存器识别唤醒源并写入BKP
 * @note   必须在PWR_FLAG_WU清零前调用，因为后续操作可能改变引脚状态
 */
static void Standby_RecordWakeSource(void)
{
    uint16_t wake_magic = STANDBY_WAKE_ARMED_MAGIC;

    if (EXTI_GetFlagStatus(EXTI_Line5) != RESET
        || EXTI_GetFlagStatus(EXTI_Line6) != RESET) {
        wake_magic = STANDBY_CHARGE_WAKE_MAGIC;
    }
    else if (EXTI_GetFlagStatus(EXTI_Line8) != RESET) {
        wake_magic = STANDBY_TOUCH_WAKE_MAGIC;
    }
    BKP_WriteBackupRegister(BKP_DR9, wake_magic);
}

/*********************************************************************
 * @brief  进入Standby模式，PA8触摸和PB6充电均可唤醒
 *         唤醒后根据备份寄存器判断唤醒源
 **********************************************************************/
void Enter_Standby(void) {
    // 关定时器
    TIM_ITConfig(TIM1, TIM_IT_Update, DISABLE);
    NVIC_DisableIRQ(TIM1_UP_IRQn);
    TIM_Cmd(TIM1, DISABLE);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
    SPI_Cmd(SPI1, DISABLE);
    SPI_Cmd(SPI2, DISABLE);

    // 关闭PA8 INT中断(EXTI8)
    NVIC_DisableIRQ(EXTI9_5_IRQn);
    EXTI->INTENR &= ~EXTI_Line8;
    EXTI_ClearFlag(EXTI_Line8);

    // GPIO全部上拉输入，降低Standby功耗，避免引脚浮空产生漏电流
    GPIO_ALL_IPU();

    Standby_PinConfig();//Standby模式引脚配置

    // GPIOC/D已在GPIO_ALL_IPU中设为上拉输入，此处确保CFGLR/CFGHR也为零
    GPIOC->CFGLR = 0x00000000;
    GPIOC->CFGHR = 0x00000000;
    GPIOD->CFGLR = 0x00000000;
    GPIOD->CFGHR = 0x00000000;

    // 禁用SWJ调试接口以降低功耗，可通过 DEBUG_SWJ_KEEP 宏保留
#ifndef DEBUG_SWJ_KEEP
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, ENABLE);
#endif

    // 使能PWR时钟
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    PWR_WakeUpPinCmd(DISABLE);  // PA0作为KEY未启用，禁用WKUP引脚

    // 禁用PVD检测，避免进入待机前触发PVD_CheckLowVoltage中断
    PWR_PVDCmd(DISABLE);

    // 清除WKUP标志
    PWR_ClearFlag(PWR_FLAG_WU);

    // 停止SysTick并等待串口发送完成
    SysTick->CTLR = 0;
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);

    // 清除所有中断标志, 准备WFI进入
    NVIC_ClearPendingIRQ(SysTick_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(TIM1_UP_IRQn);
    NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    NVIC_ClearPendingIRQ(DMA1_Channel2_IRQn);  // SPI1_RX
    NVIC_ClearPendingIRQ(DMA1_Channel3_IRQn);  // SPI1_TX
    NVIC_ClearPendingIRQ(DMA1_Channel5_IRQn);  // SPI2_TX

    Battery_StandbyWakeupConfig();
#ifdef KEEP_LCD_POWER_IN_STANDBY
    Touch_WaitWakeInputsReleased();
    Touch_StandbyWakeupConfig();
#endif

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    BKP_WriteBackupRegister(BKP_DR9, STANDBY_WAKE_ARMED_MAGIC);
    PWR_EnterSTANDBYMode_RAM();
    Standby_RecordWakeSource();

    PWR->CTLR &= ~PWR_CTLR_PDDS;
    NVIC->SCTLR &= ~(1U << 2);
    NVIC_SystemReset();
    while (1);
}

/*********************************************************************
 * @brief  检查唤醒源并恢复Standby前的状态
 * @param  touch_confirmed PA8中断已通过CST816S I2C确认有效触摸
 **********************************************************************/
static void CheckWakeupSource(uint8_t touch_confirmed) {
    GameSaveWakeType restore_type;

    // 检查WKUP标志(PWR_FLAG_WU)判断是否为Standby唤醒
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    if (s_standby_unknown_wake) {
        /*
         * EXTI Event触发唤醒但源不明，回退检查DR1/DR2标志
         * 按上电复位处理，不切换视频
         */
        first_boot = 0;
        PWR_ClearFlag(PWR_FLAG_WU);
        current_state = 0;
        return;
    }
    if (s_standby_charge_wake) {
        /*
         * 充电唤醒时从DR1/DR2中恢复游戏/视频状态，清除BKP标志
         * 充电/满电时忽略触摸，防误操作，仅恢复模式不切换视频
         */
        first_boot = 0;
        PWR_ClearFlag(PWR_FLAG_WU);
        current_state = 0;
        return;
    }
    if (s_standby_touch_wake && !touch_confirmed) {
        /* PA8误触发但IC未确认有效触摸 */
        PWR_ClearFlag(PWR_FLAG_WU);
        current_state = 0;
        return;
    }
    if (s_standby_touch_wake) {
        restore_type =
            GameSave_RestoreWakeState(&current_video_index, max_video_count);
        if (restore_type == GAME_SAVE_WAKE_VIDEO) {
            current_video_index =
                (current_video_index + 1) % max_video_count;
        }
        first_boot = 0;
        PWR_ClearFlag(PWR_FLAG_WU);
        current_state = 1;
        return;
    }
    if (PWR_GetFlagStatus(PWR_FLAG_WU) == SET) {
        // printf("\r\n Standby wake up reset \r\n");
        // 检查BKP唤醒标记
        restore_type =
            GameSave_RestoreWakeState(&current_video_index, max_video_count);
        if (restore_type == GAME_SAVE_WAKE_VIDEO) {
            current_video_index = (current_video_index + 1) % max_video_count;  // 切换下一个视频
            // printf("Switch to video index: %d\r\n", current_video_index);
        }
        first_boot = 0;          // 非首次启动
        // 清除WKUP标志
        PWR_ClearFlag(PWR_FLAG_WU);
        // Standby唤醒复位
        current_state = 1;
    }
    else {
        // PWR_FLAG_WU未置位，但BKP记录表明是Standby唤醒
        // (EXTI事件触发导致PWR_FLAG_WU未置位，但BKP已记录Standby唤醒)
        restore_type =
            GameSave_RestoreWakeState(&current_video_index, max_video_count);
        if (restore_type != GAME_SAVE_WAKE_NONE) {
            if (restore_type == GAME_SAVE_WAKE_VIDEO) {
                current_video_index =
                    (current_video_index + 1) % max_video_count;
            }
            first_boot = 0;
            current_state = 1;
        } else {
            // printf("\r\n Power on reset \r\n");
            Delay_Ms(1000);          // 上电复位，延时1秒展示Logo
        }
    }
}

/**
 * @brief  禁用触摸输入，关闭PA8中断和Event
 * @note   充电或待机唤醒后，Standby期间PA8中断未使能需要此步骤
 */
static void Main_DisableTouchInput(void)
{
    NVIC_DisableIRQ(EXTI9_5_IRQn);
    EXTI->INTENR &= ~EXTI_Line8;
    EXTI->EVENR &= ~EXTI_Line8;
    EXTI_ClearFlag(EXTI_Line8);
    NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    s_touch_down = 0;
    s_touch_started_top = 0;
}

/**
 * @brief  通过I2C触摸IC确认CST816S有效触摸唤醒
 * @return 1 确认有效触摸, 0 误触发
 */
static uint8_t Main_ConfirmTouchWake(void)
{
    uint8_t i;
    uint8_t pressed_count = 0;

    for (i = 0; i < 6; i++) {
        Touch_UpdateCoords();
        Touch_GetXY();
        if (ts.pressed) {
            if (++pressed_count >= 2) {
                return 1;
            }
        }
        else {
            pressed_count = 0;
        }
        Delay_Ms(5);
    }
    return 0;
}

/*********************************************************************
 * @brief  PVD低电压检测
 *         < 2.5V: 强制低电复位
 *         2.5~2.6V: 显示低电并禁止进入待机
 *         >= 2.6V: 正常启动
 **********************************************************************/
static void PVD_CheckLowVoltage(void) {
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);  // 使能PWR时钟
    PWR_PVDLevelConfig(PWR_PVDLevel_2V5);  // 阈值2.5V
    PWR_PVDCmd(ENABLE);
    Delay_Ms(1);

    // 当VDD < 2.5V时强制系统复位
    // 尚未初始化外设，直接复位避免异常
    if (PWR_GetFlagStatus(PWR_FLAG_PVDO) == SET
        && !Battery_IsCharging() && Battery_GetStatus() != BAT_FULL) {
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
        RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
        SPI_Flash_PowerDown();
        LCD_PWR_OFF;
        Touch_WaitWakeInputsReleased();
        NVIC_SystemReset();    // 强制系统复位 (main判断SFTRST后进入Standby)
    }
    // VDD >= 2.5V正常启动流程
    CheckWakeupSource(0);    // PA8未确认，由Touch_Init后再次判断
    LCD_Init();              // 初始化LCD
    //TFT_init();              // 初始化TFT

    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);// 全屏填充黑色，避免LCD未初始化残留花屏
    //LCD_LED_CLR;         // 开背光
    LCD_LED_SET;             // 关背光

    // 检测2.5V <= VDD < 2.6V低电显示
    CheckAndShowLowBattery();

    // VDD >= 2.6V继续正常启动
}

/*********************************************************************
 * @brief  根据当前视频索引加载视频地址
 **********************************************************************/
void LoadVideoAddress(void) {
    next_bank = video_ranges[current_video_index][0];
    next_higvh = video_ranges[current_video_index][1];
    next_high = video_ranges[current_video_index][2];
    next_low = video_ranges[current_video_index][3];
}

/**
 * @brief  主菜单关闭返回视频播放 (底部/中间区域上划关闭主菜单)
 * @note   复位菜单/游戏状态, 重新加载视频第一帧避免黑屏闪烁,
 *         置 s_m2v_swipe 防止松手帧被误判为"点击切换下一视频"。
 */
static void main_menu_close_to_video(void)
{
    s_main_mode = MAIN_MODE_VIDEO;
    s_menu_drawn = 0;
    s_menu_drag_y_prev = -999;
    s_touch_started_top = 0;  /* 复位顶部标志, 防误触 */
    s_game_inited = 0;
    s_game_sleep_mode = MAIN_MODE_VIDEO;
    vm_touch_down = 0;      /* 重置上划检测, 避免视频上划误触发 */
    vm_sliding = 0;
    /* 视频第一帧为完整帧, 直接填充背景, 避免黑屏闪烁 */
    LoadVideoAddress();
    flash_decompress_and_show(&next_bank, &next_higvh,
                              &next_high, &next_low);
    s_m2v_swipe = 1;
    /* 保留 s_touch_down: 在video轮询中vm_touch_down需要它来判断swipe,
       菜单返回视频时一并重置swipe状态; 此处swipe帧清屏后已保证下一帧不重复 */
}

/**
 * @brief  处理触摸输入：菜单拖拽、滑动、点击及游戏触摸
 * @return 1 触摸已处理(跳过本帧), 0 正常处理
 */
static uint8_t Main_HandleTouchInput(void)
{
    Touch_IsPressed();          /* 读取INT引脚, 有触摸则读坐标 */
    Touch_GetXY();              /* 获取触摸坐标到 ts (含 pressed) */

    /* PA8触摸唤醒后等待释放，避免误触发 */
    if (s_wake_wait_release) {
        if (ts.pressed) {
            s_wake_release_count = 0;
        }
        else if (++s_wake_release_count >= 2) {
            s_wake_wait_release = 0;
            s_wake_release_count = 0;
            s_touch_down = 0;
        }
        return 1;
    }

    if (current_state == 1 && end_flag == 0
        && Battery_GetStatus() == BAT_NORMAL
        && GameAutoSleep_AfterTouch()) {
        return 1;
    }

    /* 滑动动画中跳过触摸检测 */
    if (s_menu_sliding) {
        s_touch_down = 0;
        return 0;
    }

    if (ts.pressed && !s_touch_down) {
        /* 触摸按下: 记录起始位置 */
        s_touch_down = 1;
        s_touch_start_y = ts.y;
        s_menu_drag_touch_y = ts.y;
        s_touch_started_top =
            (ts.y < MENU_SWIPE_START_Y
             && (s_settings_mode == SETTINGS_NONE || !g_settings_from_game)  /* 视频设置页也允许顶部下滑弹主菜单 */
             && s_main_mode != MAIN_MODE_MENU
             && s_main_mode != MAIN_MODE_SOKOBAN
             && s_main_mode != MAIN_MODE_D2048
             && s_main_mode != MAIN_MODE_TETRIS
             && s_main_mode != MAIN_MODE_KLOTSKI
             && s_main_mode != MAIN_MODE_FISH) ? 1 : 0;
        s_touch_started_bottom =
            (ts.y > 210) ? 1 : 0;  /* >210 底部区域, 与video_menu_swipe_up_poll一致 */
        if (s_touch_started_top) {
            uint8_t need_clear = s_video_menu_dragging || vm_sliding || s_video_menu_visible;
            s_menu_drag_max_y = ts.y;    /* 记录手指起始位置作为最远Y */
            s_video_menu_dragging = 0;   /* 停止上划拖拽/滑动 */
            vm_sliding = 0;
            s_video_menu_visible = 0;    /* 下滑菜单时清除上划菜单 */
            vm_touch_down = 0;           /* 重置swipe检测, 防假触发 */
            if (need_clear) {
                /* 上划菜单存在时才需要重绘背景, 纯视频模式当前帧已是背景 */
                LoadVideoAddress();
                flash_decompress_and_show(&next_bank, &next_higvh,
                                          &next_high, &next_low);
            }
        }
        s_menu_dragging = 0;  /* 重置: 关闭拖拽 */
        s_menu_sliding = 0;   /* 重置: 终止滑动 */
        s_menu_drag_y = -MENU_H;  /* 菜单初始位置 */
        s_drag_rel_cnt = 0;   /* 复位释放计数 */
        s_m2v_swipe = 0;      /* 清除打断标志 */
    }
    else if (ts.pressed && s_touch_down && s_touch_started_top
             && s_main_mode != MAIN_MODE_MENU
             && s_main_mode != MAIN_MODE_SOKOBAN
             && s_main_mode != MAIN_MODE_D2048
             && s_main_mode != MAIN_MODE_TETRIS
             && s_main_mode != MAIN_MODE_KLOTSKI
             && s_main_mode != MAIN_MODE_FISH
             && (int16_t)ts.y - (int16_t)s_touch_start_y
                > MENU_SWIPE_TRIGGER_DIST) {
        /* 顶部拖拽: 根据手指总位移算目标位置, 逐帧追过去 */
        {
            int16_t total_dy = (int16_t)ts.y - (int16_t)s_touch_start_y;
            int16_t target_y = (int16_t)(-MENU_H) + total_dy;
            s_menu_drag_touch_y = ts.y;
            if (target_y > MENU_Y - MENU_RELEASE_GAP)
                target_y = MENU_Y - MENU_RELEASE_GAP;
            if (target_y > s_menu_drag_y) {
                int16_t diff = target_y - s_menu_drag_y;
                if (diff > MENU_DRAG_MAX_STEP)
                    diff = MENU_DRAG_MAX_STEP;
                s_menu_drag_y += diff;
                s_menu_dragging = 1;
                /* 视频设置页顶部下滑拖主菜单即退出设置页, 背景重载视频第一帧(同视频菜单清背景) */
                if (s_settings_mode != SETTINGS_NONE && !g_settings_from_game) {
                    s_settings_mode = SETTINGS_NONE;
                    LoadVideoAddress();
                    flash_decompress_and_show(&next_bank, &next_higvh,
                                              &next_high, &next_low);
                }
            }
            if (ts.y > s_menu_drag_max_y)
                s_menu_drag_max_y = ts.y;  /* 更新手指最远位置 */
        }
    }
    else if (ts.pressed && s_touch_down && s_main_mode == MAIN_MODE_MENU
             && (int16_t)s_touch_start_y - (int16_t)ts.y > MENU_SWIPE_TRIGGER_DIST) {
        /* 主菜单显示时上划关闭返回视频 (任意起点, 用主菜单自身按下起点检测更可靠) */
        main_menu_close_to_video();
        g_menu_cooldown = 15;
    }
    else if (ts.pressed && s_touch_down && s_main_mode == MAIN_MODE_VIDEO
             && s_video_menu_visible && s_settings_mode == SETTINGS_NONE
             && game_menu_swipe_mid_down_close(MENU_SWIPE_START_Y, MENU_SWIPE_TRIGGER_DIST)) {
        /* 视频上滑菜单显示时, 中间/底部下滑关闭返回视频; 顶部(起点<40)下滑保留给打开主菜单(上下可打断) */
        s_video_menu_visible = 0;
        vm_touch_down = 0;
        LoadVideoAddress();
        flash_decompress_and_show(&next_bank, &next_higvh,
                                  &next_high, &next_low);
        s_m2v_swipe = 1;
    }
    else if (!ts.pressed && s_touch_down) {
        /* 触摸释放: 处理逻辑 */
        uint8_t handled = 0;

        /* 拖拽释放: 连丢2帧确认 (防触摸IC抖动) */
        if (s_menu_dragging && ++s_drag_rel_cnt < 2) {
            return 0;
        }

        /* 菜单拖拽: 判断滑动距离, 决定是否弹出菜单 */
        if (s_menu_dragging) {
            int16_t drag_dist;

            s_menu_dragging = 0;
            drag_dist = (int16_t)ts.y - (int16_t)s_touch_start_y;
            /* 上划回退超过10像素: 强制隐藏菜单 */
            if ((int16_t)s_menu_drag_max_y - (int16_t)ts.y > 10)
                drag_dist = 0;
            if (drag_dist >= MENU_SWIPE_COMMIT_DIST) {
                /* 滑动提交: 菜单弹出动画 */
                s_menu_sliding = 1;
            } else {
                /* 距离不足: 回弹到初始位置, 继续播放视频 */
                LoadVideoAddress();
                s_menu_drag_y_prev = -999;
            }
            handled = 1;
        }

        /* 1. 视频播放或游戏模式顶部快速下划 → 弹出菜单 (无需拖拽, 静默拖动距离已达标) */
        if (!handled && s_touch_started_top
            && (int16_t)ts.y - (int16_t)s_touch_start_y
               >= MENU_SWIPE_COMMIT_DIST
            && (int16_t)s_menu_drag_max_y - (int16_t)ts.y <= 10) {
            if (s_main_mode != MAIN_MODE_MENU
                && s_main_mode != MAIN_MODE_SOKOBAN
                && s_main_mode != MAIN_MODE_D2048
                && s_main_mode != MAIN_MODE_TETRIS
                && s_main_mode != MAIN_MODE_KLOTSKI
                && s_main_mode != MAIN_MODE_FISH) {
                /* 顶部下划, 切换菜单模式 (弹出动画) */
                s_menu_sliding = 1;
                /* 切换游戏复位参数, 清除游戏初始化标志 */
                handled = 1;
            }
        }

        /* 菜单5行点击 (2048 / 推箱子 / 俄罗斯方块 / 华容道 / 设置) */
        if (!handled && s_main_mode == MAIN_MODE_MENU) {
            handled = main_menu_handle(ts.x, s_touch_start_y,
                                       &s_main_mode, &s_menu_drawn,
                                       &s_game_inited, &s_touch_down,
                                       &key_response);
        }

        /* 设置界面BACK键→回视频/游戏 (任意位置生效) */
        if (!handled && s_settings_mode != SETTINGS_NONE
            && ts.x >= BACK_X && ts.x <= BACK_X + BACK_W
            && ts.y >= BACK_Y && ts.y <= BACK_Y + BACK_H) {
            handled = 1;
            s_settings_mode = SETTINGS_NONE;
            if (!g_settings_from_game) {
                LoadVideoAddress();
            }
            key_response = 1;
            end_flag = 0;
        }

        /* 低电闪屏时点击, 不做任何处理直接释放 */
        if (!handled && blink_touched) {
            blink_touched = 0;
            s_touch_down = 0;
            return 0;
        }

        /* 4. 特殊情况释放: 不做任何模式切换 / 清屏+重载视频 */
        if (!handled && s_m2v_swipe) {
            handled = 1;              /* 菜单→视频打断标记, 已经处理 */
            s_m2v_swipe = 0;
        }
        if (!handled && s_video_menu_dragging) {
            handled = 1;  /* 视频划动菜单拖拽中释放不切换 */
        }
        if (!handled && s_video_menu_visible) {
            int8_t hit = -1;
            if (ts.x >= VIDEO_MENU_X && ts.x < VIDEO_MENU_X + VIDEO_MENU_W) {
                uint16_t iy0 = VIDEO_MENU_Y + VIDEO_MENU_BDR;
                uint16_t iye = VIDEO_MENU_Y + VIDEO_MENU_H4 - VIDEO_MENU_BDR;
                if (ts.y >= iy0 && ts.y < iye) {
                    hit = (int8_t)((ts.y - iy0) / ((iye - iy0) / 4));
                }
            }
            handled = 1;
            s_video_menu_visible = 0;
            vm_touch_down = 0;
            if (hit == -1 || hit == 3) {
                if (s_settings_mode != SETTINGS_NONE) {
                    if (hit == 3) s_settings_mode = SETTINGS_NONE;
                    else Settings_Enter(s_settings_mode);
                }
                if (hit != -1 || s_settings_mode == SETTINGS_NONE) {
                    if (!g_settings_from_game) {
                        LoadVideoAddress();
                        flash_decompress_and_show(&next_bank, &next_higvh,
                                                  &next_high, &next_low);
                    }
                }
            } else if (hit >= 0 && hit <= 2) {
                s_settings_mode = (SettingsMode)((uint8_t)hit + 1);
                Settings_Enter(s_settings_mode);
            }
        }

        /* 5. 普通触摸释放, 点击切换下一视频 (设置/菜单可见时不触发) */
        if (!handled && s_main_mode == MAIN_MODE_VIDEO
            && s_settings_mode == SETTINGS_NONE && !s_video_menu_visible) {
            current_video_index =
                (current_video_index + 1) % max_video_count;
            LoadVideoAddress();
            key_response = 1;
        }

        s_touch_down = 0;
    }

    return 0;
}

/**
 * @brief  低电图标灭段(On->Off)后, 恢复被其覆盖的底层内容(游戏场景+当前菜单框)
 * @note   不用 LCD_ClearLowBatteryOverlay 把图标轮廓刷黑(会在彩色菜单/场景留下黑/红块),
 *         改为置脏由各游戏自身重绘覆盖: 退出/重开菜单用 *_menu_mark_dirty(置
 *         game_menu_command 的 bg_redraw 所读的脏, 使其下帧重绘场景+菜单框), 纯游戏用
 *         *_full_redraw; fish 用局部清除。注意菜单态不能用 *_full_redraw(d2048/klotski
 *         只置 g_full_redraw 而 bg_redraw 读 g_grid_dirty, 会漏重绘导致红色残留)。
 */
static void main_restore_game_under_icon(void)
{
    switch (s_main_mode) {
        case MAIN_MODE_SOKOBAN: sokoban_lowbatt_restore(); break;
        case MAIN_MODE_FISH:    fish_clear_lowbattery(); break;
        case MAIN_MODE_D2048:   d2048_lowbatt_restore(); break;
        case MAIN_MODE_TETRIS:  tetris_lowbatt_restore(); break;
        case MAIN_MODE_KLOTSKI: klotski_lowbatt_restore(); break;
        default: break;
    }
}

static void main_lowbatt_restore_under_icon(void)
{
    if (g_settings_from_game) {
        /* 设置模式(从游戏进入): 屏幕显示设置界面而非游戏, 恢复时应重绘设置界面.
           - 二级"设置选项"条带可见, 且已打开设置页: 先重绘设置页再画条带(条带压在页上);
           - 条带可见但未打开设置页: 恢复游戏场景再画条带(条带压在游戏上);
           - 设置页显示: 重绘设置页(整屏填充, 覆盖电池区). */
        if (s_video_menu_visible) {
            if (s_settings_mode != SETTINGS_NONE) Settings_RedrawRegion();
            else main_restore_game_under_icon();
            menu_draw_nrow_frame_only(s_video_menu_slide_y,
                                      g_video_menu_row_types, 4);
        } else if (s_settings_mode != SETTINGS_NONE) {
            Settings_RedrawRegion();
        } else {
            /* 推箱子二级菜单跟手滑出期间(s_video_menu_visible=0, 无设置页):
               屏幕显示设置条带+游戏, 凸点在条带外, 须由游戏恢复 */
            main_restore_game_under_icon();
        }
        return;
    }
    main_restore_game_under_icon();
}
/**
 * @brief  电量0级时低频闪烁低电图标: 灭2秒亮1秒
 * @note   仅4种游戏模式有效, BAT_LEVEL_0时触发
 *         闪烁时叠加LowBattery图标, SPI占用时可能产生撕裂
 */
static void Main_FlashLowBattery(void)
{
    static uint8_t  s_flash_on = 0;
    static uint32_t s_flash_tick = 0;

    /* 非游戏模式关闭闪烁 */
    if (s_main_mode != MAIN_MODE_SOKOBAN
        && s_main_mode != MAIN_MODE_D2048
        && s_main_mode != MAIN_MODE_TETRIS
        && s_main_mode != MAIN_MODE_KLOTSKI
        && s_main_mode != MAIN_MODE_FISH) {
        s_flash_on = 0;
        return;
    }

    if (game_battery_icon_get_level() != BAT_LEVEL_0) {
        s_flash_on = 0;
        return;
    }

    uint32_t now = GetTimerCount();

    if (s_flash_on) {
        /* 亮屏段，持续1秒 */
        if ((uint32_t)(now - s_flash_tick) >= 1000000UL) {
            s_flash_on = 0;
            s_flash_tick = now;
            /* 灭屏时段(On->Off): 恢复被低电图标覆盖的底层内容(场景+当前菜单框) */
            main_lowbatt_restore_under_icon();
        } else {
            LCD_ShowLowBatteryOverlay();
        }
    } else {
        /* 灭屏段，持续2秒 */
        if ((uint32_t)(now - s_flash_tick) >= 2000000UL) {
            s_flash_on = 1;
            s_flash_tick = now;
        }
    }
}

/**
 * @brief  视频上划菜单是否活跃
 */
static uint8_t VideoMenu_IsActive(void) {
    return s_video_menu_dragging || vm_sliding || s_video_menu_visible;
}

/**
 * @brief  视频上划菜单统一轮询+绘制 (视频模式和游戏模式共用)
 * @param  ts             当前触摸状态
 * @param  is_video_mode  是否为视频模式 (决定need_clear时是否播帧)
 */
static void VideoMenu_Update(const TouchState *ts, uint8_t is_video_mode)
{
    /* 检测上划手势触发 (设置模式下仅底部允许) */
    if (!s_video_menu_dragging && !vm_sliding && !s_video_menu_visible
        && (s_settings_mode == SETTINGS_NONE || s_touch_started_bottom)) {
        if (video_menu_swipe_up_poll(ts, &vm_touch_down,
                                      &vm_start_x, &vm_start_y)) {
            s_video_menu_dragging = 1;
            s_video_menu_slide_y = VIDEO_MENU_Y + VIDEO_MENU_H4;
            s_video_menu_drag_start_y = vm_start_y;
            vm_touch_down = 0;
            if (is_video_mode && (s_menu_dragging || s_menu_sliding)) {
                /* 有主菜单覆盖需重绘, 视频帧填充背景(避免黑屏) */
                uint8_t  save_bank  = next_bank;
                uint16_t save_higvh = next_higvh;
                uint16_t save_high  = next_high;
                uint16_t save_low   = next_low;
                LoadVideoAddress();
                flash_decompress_and_show(&next_bank, &next_higvh,
                                          &next_high, &next_low);
                next_bank  = save_bank;
                next_higvh = save_higvh;
                next_high  = save_high;
                next_low   = save_low;
            }
            s_menu_dragging = 0;
            s_menu_sliding = 0;
        }
    }
    /* 拖拽跟手, 取消时重新加载视频第一帧完整覆盖 */
    if (s_video_menu_dragging) {
        uint8_t poll_ret = video_menu_drag_poll(&s_video_menu_slide_y,
                                                 s_video_menu_drag_start_y,
                                                 &s_video_menu_dragging, ts);
        if (poll_ret == 1) {
            vm_sliding = 1;
        } else if (poll_ret == 2) {
            if (s_settings_mode != SETTINGS_NONE) {
                Settings_Enter(s_settings_mode);
            } else if (!g_settings_from_game) {
                LoadVideoAddress();
                flash_decompress_and_show(&next_bank, &next_higvh,
                                          &next_high, &next_low);
            }
        }
    }
    /* 松手后回弹到最终位置 */
    else if (vm_sliding) {
        s_video_menu_slide_y -= MENU_SLIDE_SPEED;
        if (s_video_menu_slide_y <= VIDEO_MENU_Y) {
            s_video_menu_slide_y = VIDEO_MENU_Y;
            vm_sliding = 0;
            s_video_menu_visible = 1;
        }
        menu_draw_nrow_frame_only(s_video_menu_slide_y,
                                   g_video_menu_row_types, 4);
    }
}

/**
 * @brief  从游戏"二次上划"进入设置菜单并跟手滑出 (推箱子用)
 * @param  start_y  触摸起点Y (上划手势的按下起始位置)
 * @note   复用 VideoMenu_Update 的跟手拖拽状态机: 置 s_video_menu_dragging=1,
 *         s_video_menu_slide_y 从底部(VIDEO_MENU_Y+VIDEO_MENU_H4)开始, 随后
 *         每帧由 video_menu_drag_poll 依据手指位移往上跟手滑出, 松手滑到顶部。
 *         与静止的 menu_enter_game_settings() 区别: 后者直接显示, 无跟手动画。
 */
void menu_enter_game_settings_swipe(uint16_t start_y)
{
    g_settings_from_game = 1;
    s_video_menu_visible = 0;
    s_video_menu_dragging = 1;
    s_video_menu_slide_y = VIDEO_MENU_Y + VIDEO_MENU_H4;
    s_video_menu_drag_start_y = start_y;
    vm_touch_down = 0;
}

/**
 * @brief  从游戏进入设置的通用处理: 菜单绘制/设置页面更新/返回游戏
 * @note   仅在 g_settings_from_game != MAIN_MODE_VIDEO 时生效
 */
static void GameSettings_Update(void)
{
    /* 条带静止态去抖: 仅在滑动动画(位置变化)或刚变可见时重绘, 避免每帧全幅重绘
       与低电图标闪烁争夺(导致菜单与红色图标飞快交替闪烁)。与游戏菜单保持静态,
       低电图标才能正常几秒闪一次。离开设置时复位, 保证下次进入强制首个重绘。 */
    static int16_t s_strip_prev_y = -1;
    static uint8_t s_strip_prev_vis = 0;

    if (!g_settings_from_game) {
        s_strip_prev_vis = 0;
        return;
    }

    if (g_menu_cooldown > 0) g_menu_cooldown--;  /* 设置菜单下滑关闭防重复触发冷却 */

    VideoMenu_Update(&ts, 0);

    /* 设置页面: 顶部下滑触发退出菜单(游戏切换) */
    {
        static uint8_t  gs_t_dn;
        static uint16_t gs_sy;
        static uint8_t  gs_rel_db;  /* 释放防抖: 避免单帧丢触摸重置追踪 */
        if (!s_video_menu_dragging && !vm_sliding
            && (s_video_menu_visible || s_settings_mode != SETTINGS_NONE)) {
            if (!ts.pressed) {
                if (gs_t_dn && ++gs_rel_db < 2) {
                    /* 防抖: 单帧丢触摸不重置, 等待下一帧确认 */
                } else {
                    gs_t_dn = 0;
                    gs_rel_db = 0;
                }
            } else {
                gs_rel_db = 0;
                if (!gs_t_dn) { gs_t_dn = 1; gs_sy = ts.y; }
                else if (gs_sy < MENU_SWIPE_START_Y
                         && (int16_t)ts.y - (int16_t)gs_sy > MENU_SWIPE_COMMIT_DIST) {
                    gs_t_dn = 0;
                    gs_rel_db = 0;
                    uint8_t was_strip = s_video_menu_visible;
                    s_video_menu_visible = 0;
                    s_settings_mode = SETTINGS_NONE;
                    if (was_strip) {
                        /* 二级设置条带压在游戏上: 只清条带区(条带区外游戏场景保留),
                           参考一级菜单切换/返回游戏做法, 不整屏刷黑(否则游戏场景丢失→黑闪一帧) */
                        LCD_Fill_DMA(VIDEO_MENU_X, VIDEO_MENU_Y,
                                     VIDEO_MENU_X + VIDEO_MENU_W, 240, g_menu_clear_color);
                    } else {
                        LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);  /* 设置页整屏: 需清屏 */
                    }
                    g_settings_from_game = 0;
                    g_game_enter_exit_menu = 1;
                    g_exit_menu_visible = 1;
                    g_exit_sliding = 0;
                    game_menu_drag_reset();
                    g_exit_dragging = 1;
                    g_exit_drag_y = -MENU_H + MENU_DRAG_MAX_STEP;
                    return;
                }
            }
        }
    }

    /* 中间/底部区域下滑 → 关闭二级"设置选项"菜单返回游戏 (与四游戏重菜下滑返回一致)
       仅作用于设置选项菜单(非设置页); 置 s_video_menu_visible=0 后不 return,
       立即落入下方绘制分支清条带+重绘游戏, 避免闪屏。 */
    if (s_video_menu_visible && s_settings_mode == SETTINGS_NONE
        && !s_video_menu_dragging && !vm_sliding
        && game_menu_swipe_mid_down_close(MENU_SWIPE_START_Y, MENU_SWIPE_TRIGGER_DIST)) {
        game_menu_fix_release_after_close();  /* 下滑关闭回游戏, 松手不传入游戏 */
        s_video_menu_visible = 0;
        s_settings_mode = SETTINGS_NONE;
    }

    int16_t strip_now_y = s_video_menu_slide_y;

    if (s_video_menu_visible) {
        if (!s_strip_prev_vis) s_strip_prev_y = -1;  /* 刚变可见: 强制首个重绘 */
        if (s_settings_mode == SETTINGS_NONE
            && s_strip_prev_y != strip_now_y) {
            s_strip_prev_y = strip_now_y;
            menu_draw_nrow_frame_only(strip_now_y,
                                       g_video_menu_row_types, 4);
        }
        s_strip_prev_vis = 1;
    } else {
        s_strip_prev_vis = 0;
        s_strip_prev_y = -1;
        /* 跟手拖拽/回滑期间由 VideoMenu_Update 绘制, 不得误判为"退出设置" */
        if (s_video_menu_dragging || vm_sliding)
            return;

        if (s_settings_mode != SETTINGS_NONE) {
            IWDG_ReloadCounter();
            Settings_Update();
        } else {
            g_settings_from_game = 0;
            /* 二级"设置选项"菜单返回游戏: 只用游戏背景色清除菜单条带区,
               并在同一帧重绘场景。二级菜单是底部覆盖条(与一级菜单一样),
               场景始终在它后面, 无需整屏刷黑(那会造成一帧黑闪)。
               返回键在此处松开(ts.pressed==0), 因此 update() 在手势层面是安全的无操作。 */
            LCD_Fill_DMA(VIDEO_MENU_X, VIDEO_MENU_Y,
                         VIDEO_MENU_X + VIDEO_MENU_W, 240, g_menu_clear_color);
            if (s_main_mode == MAIN_MODE_SOKOBAN) { sokoban_menu_settings_restore(); }
            else if (s_main_mode == MAIN_MODE_D2048) { d2048_full_redraw(); d2048_update(); }
            else if (s_main_mode == MAIN_MODE_TETRIS) { tetris_menu_mark_dirty(); tetris_menu_bg_redraw(); }
            else if (s_main_mode == MAIN_MODE_FISH) { fish_full_redraw(); fish_update(); }
            else if (s_main_mode == MAIN_MODE_KLOTSKI) { klotski_full_redraw(); klotski_update(); }
        }
    }
}

/**
 * @brief  活动模式更新: 菜单/游戏/视频主循环处理
 * @param  charging_game_saved 充电时游戏已存档标志
 */
/* 统一游戏主循环分支: 首次进入设置背光+init, 进设置(g_settings_from_game)时跳过刷新
   (由 GameSettings_Update 处理设置界面); update 返回的切换命令交给 game_handle_ret */
static void main_run_game(void (*init_fn)(void), uint8_t (*update_fn)(void))
{
    if (!s_game_inited) {
        LCD_Backlight_PWM_SetDuty(g_brightness);
        init_fn();
        s_game_inited = 1;
    }
    if (!g_settings_from_game)
        game_handle_ret(update_fn(), &s_main_mode, &s_game_inited,
                        &s_touch_down, &key_response);
}

static void Main_UpdateActiveMode(uint8_t *charging_game_saved)
{
    BatteryStatus active_bat = Battery_GetStatus();

    /* 充电/满电时10秒无触摸则自动存档待机 */
    if (active_bat == BAT_CHARGING || active_bat == BAT_FULL) {
        if (!g_charge_pending) {
            g_charge_pending = 1;
            g_charge_pending_time = GetTimerCount();
            s_game_last_activity_us = g_charge_pending_time;  /* 充电时重置游戏无触摸计时 */
            game_battery_set_charge_flash(1);
            Battery_GameLevelAllowIncrease();  /* 确保<10s不会重复允许 */
        }
        if ((uint32_t)(GetTimerCount() - g_charge_pending_time)
            >= ((uint32_t)g_sleep_times[g_sleep_time_index] * 1000000UL)) {
            Main_DisableTouchInput();
            *charging_game_saved = 0;
            if (GameSave_IsSupportedMode(s_main_mode)
                && GameSave_Write(s_main_mode)) {
                GameSave_SaveGameWakeState(s_main_mode);
                *charging_game_saved = 1;
            }
            end_flag = 1;
            g_charge_pending = 0;
            game_battery_set_charge_flash(0);
            return;
        }
        /* 充电中持续刷新10秒计时 */
        {
            TouchState charge_ts;
            Touch_Scan(&charge_ts);
            if (charge_ts.pressed) {
                g_charge_pending_time = GetTimerCount();
            }
        }
        /* 充电中I2C可能被暂停，通过Touch_IsPressed重置触摸状态 */
        Touch_IsPressed();
        /* 充电状态未变，继续充电流程 */
    } else {
        /* 非充电状态，清除充电相关标志 */
        if (g_charge_pending) {
            g_charge_pending = 0;
            s_game_last_activity_us = GetTimerCount();  /* 拔电时重置游戏无触摸计时 */
            game_battery_set_charge_flash(0);
        }
    }

    if (s_main_mode == MAIN_MODE_MENU) {
        /* 菜单模式: 首次绘制菜单 */
        if (!s_menu_drawn) {
            main_menu_draw();
            s_menu_drawn = 1;
        }
        /* 菜单已绘制无需重复绘制 */
        if (g_menu_cooldown > 0) g_menu_cooldown--;  /* 主菜单上划关闭防重复触发冷却 */
    }
    else if (s_main_mode == MAIN_MODE_SOKOBAN) {
        /* 推箱子游戏 */
        if (!s_game_inited) {
            LCD_Backlight_PWM_SetDuty(g_brightness);
            sokoban_init_level(0);
            s_game_inited = 1;
        }
        if (!g_settings_from_game)
            game_handle_ret(sokoban_update(), &s_main_mode, &s_game_inited,
                            &s_touch_down, &key_response);
    }
    else if (s_main_mode == MAIN_MODE_D2048) {
        main_run_game(d2048_init, d2048_update);
    }
    else if (s_main_mode == MAIN_MODE_TETRIS) {
        main_run_game(tetris_init, tetris_update);
    }
    else if (s_main_mode == MAIN_MODE_KLOTSKI) {
        main_run_game(klotski_init, klotski_update);
    }
    else if (s_main_mode == MAIN_MODE_FISH) {
        main_run_game(fish_init, fish_update);
    }
    else {
        /* 视频模式: 上划菜单 + 视频主循环 */
        if (g_menu_cooldown > 0) g_menu_cooldown--;  /* 菜单/上滑菜单防重复触发冷却 */
        VideoMenu_Update(&ts, 1);

        /* 设置界面: 优先处理(返回键由触摸释放处理)
         * 视频菜单覆盖时暂停设置更新, 防止滑块等重绘破坏菜单显示 */
        if (s_settings_mode != SETTINGS_NONE && !s_video_menu_visible) {
            IWDG_ReloadCounter();
            Settings_Update();
        }
        else if (s_settings_mode == SETTINGS_NONE
                 && !VideoMenu_IsActive() && !s_menu_dragging && !s_menu_sliding) {
            flash_decompress_and_show(&next_bank, &next_higvh,
                                      &next_high, &next_low);
        }

        /* 主菜单拖拽中: 位置变化时绘制, 避免重复绘制 */
        if (s_menu_dragging && s_menu_drag_y != s_menu_drag_y_prev) {
            main_menu_draw_frame_only(s_menu_drag_y);
            s_menu_drag_y_prev = s_menu_drag_y;
        }

        /* 主菜单下滑动画: delta=1逐帧下滑, 到达目标位置, 切换到菜单模式 */
        if (s_menu_sliding) {
            s_menu_drag_y += MENU_SLIDE_SPEED;
            if (s_menu_drag_y >= MENU_Y) {
                s_menu_drag_y = MENU_Y;
                s_menu_sliding = 0;
                s_menu_drag_y_prev = -999;
                s_main_mode = MAIN_MODE_MENU;
                /* 菜单已到达位置无需重绘, 下次直接绘制 */
                s_menu_drawn = 1;
            }
            main_menu_draw_frame_only(s_menu_drag_y);
            s_menu_drag_y_prev = s_menu_drag_y;
        }
    }
    /* 从游戏进入设置: 通用处理 (所有模式尾部调用, 视频模式下为no-op) */
    GameSettings_Update();
}

int main(void) {
#ifdef VIDEO_CHUNK_STORE_MODE
    /* ====== 视频数据块烧录模式 ======
     * 将嵌入固件的视频数据块(g_video_chunk)烧录到外部Flash地址0(视频区域),
     * 块内容由 gen_video_chunk.py 生成; 注释掉宏即恢复正常程序 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);//设置中断优先级分组1
    SystemCoreClockUpdate();//更新系统时钟
    Delay_Init();//初始化延时函数

    LCD_PWR_Init();          // LCD电源+背光引脚初始化
    LCD_PWR_ON;              // LCD 从OFF切ON
    Delay_Ms(20);            // 等待LCD复位完成和SPI稳定
    SPI_FLASH_Init();        // 初始化外部Flash SPI
    SPI_Flash_WAKEUP();      // 从PowerDown中唤醒Flash
    LCD_Init();              // 初始化LCD

    LCD_Backlight_PWM_Stop();// PB9回GPIO模式, 停PWM
    LCD_LED_CLR;             // 打开背光

    VideoChunkStore_BurnAll();  // 烧录嵌入的视频数据块到外部Flash, 完成后常驻显示100%
    while (1) {
        Delay_Ms(1000);
    }
#elif defined(FLASH_DATA_STORE_MODE)
    /* ====== Flash数据存储模式 ======
     * 仅执行字库/图片资源烧录, 不进入任何游戏逻辑;
     * 注释掉main.c顶部的 FLASH_DATA_STORE_MODE 宏即恢复正常游戏程序 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);//设置中断优先级分组1
    SystemCoreClockUpdate();//更新系统时钟
    Delay_Init();//初始化延时函数

    LCD_PWR_Init();          // LCD电源+背光引脚初始化
    LCD_PWR_ON;              // LCD 从OFF切ON
    Delay_Ms(20);            // 等待LCD复位完成和SPI稳定
    SPI_FLASH_Init();        // 初始化外部Flash SPI
    SPI_Flash_WAKEUP();      // 从PowerDown中唤醒Flash
    LCD_Init();              // 初始化LCD

    LCD_Backlight_PWM_Stop();// PB9回GPIO模式, 停PWM
    LCD_LED_CLR;             // 打开背光

    FlashStore_BurnAll();    // 烧录+CRC校验, 完成后常驻显示100%
    while (1) {
        Delay_Ms(1000);
    }
#else
    // 主循环局部变量
    uint16_t peak_voltage = 0;
    uint8_t  was_charging = 0;
    uint8_t  charge_exit_count = 0;
    uint8_t  game_restored = 0;
    uint8_t  charging_game_saved = 0;
    uint8_t  wake_touch_confirmed = 0;
    uint16_t standby_wake_magic = 0;
    // ====== 调试模式：绕过Standby，直接等待PA8触摸 ======
//#define DEBUG_NO_STANDBY
#ifdef DEBUG_NO_STANDBY
    // 调试模式：PA8触摸直接触发主循环，绕过Standby
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
        RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO, ENABLE);

    /* PA8: 触摸INT输入(上拉), CST816S有触摸时会拉低此引脚 */
    GPIOA->CFGHR = (GPIOA->CFGHR & ~0xF) | 0x8;  // PA8=上拉/浮空输入
    GPIOA->BSHR = GPIO_Pin_8;  // PA8 ODR=1, 使能上拉
    while (1) {
        if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_8) == Bit_RESET) {
            // 检测到PA8触摸唤醒
            current_state = 1;
            end_flag = 0;
            // 初始化系统时钟和外设
            NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
            SystemCoreClockUpdate();
            Delay_Init();

            // PVD低电压检测: VDD < 2.5V 强制 → 系统复位进入Standby
            Battery_Init();
            RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
            PWR_PVDLevelConfig(PWR_PVDLevel_2V5);
            PWR_PVDCmd(ENABLE);
            Delay_Ms(1);
            if (PWR_GetFlagStatus(PWR_FLAG_PVDO) == SET
                && !Battery_IsCharging() && Battery_GetStatus() != BAT_FULL) {
                RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
                RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
                SPI_Flash_PowerDown();
                Touch_WaitWakeInputsReleased();
                NVIC_SystemReset();  // 强制系统复位 -> main()判断SFTRST -> Standby
            }

            LCD_PWR_Init();
            LCD_PWR_ON;
            LCD_LED_SET;
            SPI_FLASH_Init();
            SPI_Flash_WAKEUP();
            LCD_Init();
            //TFT_init();
            LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
            LCD_LED_SET;
            CheckAndShowLowBattery();  // 2.5V~2.6V低电显示

            LCD_Backlight_PWM_Stop();  // 停PWM使PB9回GPIO模式，确保LCD_LED_SET/CLR有效
            LCD_LED_CLR;              // 开LCD背光(GPIO LOW=ON)
            TIMER2_Init();
            LCD_DMA_Interrupt_Init();
            Touch_Init();           // 初始化触摸控制器(CST816S I2C通信)
            LoadVideoAddress();
            goto MAIN_LOOP;  // 跳转到主循环
        }
    }
#else
    // ====== 正式版本：通过BKP备份寄存器判断复位类型和Standby唤醒源 ======
    RCC_APB1PeriphClockCmd(
        RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    standby_wake_magic = BKP_ReadBackupRegister(BKP_DR9);
    if (standby_wake_magic == STANDBY_WAKE_ARMED_MAGIC) {
        s_standby_unknown_wake = 1;
    }
    else if (standby_wake_magic == STANDBY_TOUCH_WAKE_MAGIC) {
        s_standby_touch_wake = 1;
    }
    else if (standby_wake_magic == STANDBY_CHARGE_WAKE_MAGIC) {
        s_standby_charge_wake = 1;
    }
    if (standby_wake_magic != 0) {
        BKP_WriteBackupRegister(BKP_DR9, 0);
    }

    /* 恢复设置 */
    if (BKP_ReadBackupRegister(BKP_DR6) == 0xB071) {
        uint16_t idx = BKP_ReadBackupRegister(BKP_DR7);
        if (idx <= 5) g_sleep_time_index = (uint8_t)idx;
        uint16_t br = BKP_ReadBackupRegister(BKP_DR8);
        if (br >= 1 && br <= 100) g_brightness = (uint8_t)br;  /* br=0表示未保存, 保持默认100% */
        uint16_t orient = BKP_ReadBackupRegister(BKP_DR10);
        if (orient <= 3) g_orientation_index = (uint8_t)orient;
    }

    if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET
        && !s_standby_touch_wake && !s_standby_charge_wake
        && !s_standby_unknown_wake) {
        RCC_ClearFlag();  // 清除复位标志
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
            RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO, ENABLE);

        // GPIOC/D时钟已使能，GPIOA/B由Standby_PinConfig设置
        GPIOC->CFGLR = 0x00000000;
        GPIOC->CFGHR = 0x00000000;
        GPIOD->CFGLR = 0x00000000;
        GPIOD->CFGHR = 0x00000000;

        Standby_PinConfig();//Standby模式引脚配置

        // 配置PB6(CHRG) EXTI中断唤醒
        Battery_StandbyWakeupConfig();
#ifdef KEEP_LCD_POWER_IN_STANDBY
        // 保电时配置LCD供电和PA8 EXTI唤醒
        Touch_WaitWakeInputsReleased();
        Touch_StandbyWakeupConfig();
#endif

        /* PA0作为KEY未启用，PA8仅作触摸唤醒，PB5/PB6充电唤醒 */
        PWR_WakeUpPinCmd(DISABLE);
        BKP_WriteBackupRegister(BKP_DR9, STANDBY_WAKE_ARMED_MAGIC);

        /*
         * PA8 EXTI Event触发WFI后会引发Power Reset丢失SRAM数据，
         * 因此需要重新初始化所有外设复位
         */
        PWR_EnterSTANDBYMode_RAM();
        Standby_RecordWakeSource();
        PWR->CTLR &= ~PWR_CTLR_PDDS;
        NVIC->SCTLR &= ~(1U << 2);
        NVIC_SystemReset();
        while (1);
    }
    RCC_ClearFlag();
#endif

    // ====== 系统初始化 ======
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);//设置中断优先级分组1
    SystemCoreClockUpdate();//更新系统时钟
    Delay_Init();//初始化延时函数

    // USART_Printf_Init(115200);//初始化串口,USART1复用语音IC
    // printf("SystemClk:%d\r\n", SystemCoreClock);

    LCD_PWR_Init();          // LCD电源+背光引脚初始化
    LCD_PWR_ON;  // LCD 从OFF切ON
    Battery_Init();          // 配置PB5/PB6为充电检测上拉输入
    if (s_standby_unknown_wake) {
        /*
         * PB5/PB6电平可能变化，退化为检查充电状态回退到触摸/充电
         * 根据充电状态再判断唤醒源
         */
        s_standby_unknown_wake = 0;
        if (Battery_GetStatus() != BAT_NORMAL) {
            s_standby_charge_wake = 1;
        }
        else {
            s_standby_touch_wake = 1;
        }
        CheckWakeupSource(0);
    }
    if (s_standby_touch_wake) {
        /*
         * 先LCD/Flash初始化完毕再确认触摸，降低确认延迟提高响应
         * 通过拉低PB8复位CST816S
         */
        Touch_WakeResumeInit();
        wake_touch_confirmed = Main_ConfirmTouchWake();
    }

    Delay_Ms(20);            // 等待LCD复位完成和SPI稳定
    SPI_FLASH_Init();//初始化Flash SPI 
    SPI_Flash_WAKEUP();      // 从PowerDown或Standby中唤醒Flash
    FlashStore_ResInit();    // 读取外部Flash资源目录(字库/图片数据已烧录)

    PVD_CheckLowVoltage(); // 低电压检测<2.7V复位,>= 2.7V正常

    TIMER2_Init();           // 初始化定时器2
    LCD_DMA_Interrupt_Init();//LCD DMA中断配置
    BAT_ADC_Init();          // 初始化ADC电池检测
    VOICE_SW_Init();         // 语音IC电源控制(PB3, 低电平有效)
//#if 0
//    /* 语音IC UART1: PA9 AF_PP, 9600 8N1, 直接写BRR避免__udivdi3(省~1.2K Flash) */
//    {
//        RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1 | RCC_APB2Periph_GPIOA, ENABLE);
//        GPIO_InitTypeDef g = {0};
//        g.GPIO_Pin = GPIO_Pin_9;
//        g.GPIO_Speed = GPIO_Speed_50MHz;
//        g.GPIO_Mode = GPIO_Mode_AF_PP;
//        GPIO_Init(GPIOA, &g);
//        /* 144MHz/(16*9600)=937.5, BRR=(937<<4)|8=0x3A98 */
//        USART1->BRR = 0x3A98;
//        USART1->CTLR1 = 0x00002008;  /* UE | TE (TX only, 8bit) */
//        USART1->CTLR2 = 0;
//        USART1->CTLR3 = 0;
//    }
//#endif
    LCD_LED_SET;             // 在AF_OD模式下ODR无法控制，此处无效但保留
    LCD_Backlight_PWM_Init();// 初始化背光PWM(TIM4_CH4, 默认0%)
    if (!s_standby_touch_wake) {
        Touch_Init();
    }
    if (s_standby_charge_wake) {
        /* 充电唤醒时不响应触摸，直接禁用以防干扰 */
        Main_DisableTouchInput();
    }
    else if (s_standby_touch_wake) {
        if (!wake_touch_confirmed) {
            /*
             * PA8中断但未确认有效触摸，可能是干扰误触发
             * 重新配置INT引脚并系统复位进入Standby
             */
            Main_DisableTouchInput();
            Touch_WaitWakeInputsReleased();
            NVIC_SystemReset();
            while (1);
        }
        CheckWakeupSource(1);
        /* 确认PA8有效触摸后，重新初始化触摸IC确保通信 */
        Touch_Init();
    }
    // if (current_state == 1) {//菜单模式
    //     VOICE_SW_ON;//打开语音IC
    //     Delay_Ms(150);//等待语音IC启动
    //     while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    //     VOICE_PlayTrack(current_video_index+1);  // 播放对应视频曲目
    // }

    Settings_ApplyOrientation(g_orientation_index);
    game_restored = GameRestore_ApplyPending();//尝试恢复待处理的游戏存档

#ifndef DEBUG_NO_STANDBY
    if (s_standby_touch_wake) {
        current_state = 1;
        end_flag = 0;
        s_wake_wait_release = 1;
        s_wake_release_count = 0;
    }
    IWDG_Init();//初始化独立看门狗
#endif

    // Standby唤醒后加载视频地址
    if (!game_restored && (!first_boot || current_state == 1)) {
        LoadVideoAddress();
    }

#ifdef DEBUG_NO_STANDBY
    MAIN_LOOP :
#endif
    // 主循环
    while (1) {
        
#ifndef DEBUG_NO_STANDBY
        IWDG_ReloadCounter();//喂狗
#endif
        /* 定时器TIM2中断置位标志，while中CheckAndShowLowBattery刷新LCD */
        if (TIMER2_TakeLowBatteryPending()) {
            CheckAndShowLowBattery();
        }
        /* 有触摸时先处理触摸输入再判断(可能改变模式)
         * 充电时仅菜单/设置可交互, 防止误触视频/游戏 */
        if (current_state == 1 && end_flag == 0
            && (Battery_GetStatus() == BAT_NORMAL
                || s_video_menu_visible
                || s_video_menu_dragging
                || s_main_mode == MAIN_MODE_MENU
                || s_settings_mode != SETTINGS_NONE
                || g_settings_from_game)
            && Main_HandleTouchInput()) {
            continue;
        }

        /* 活动状态: 处理游戏/菜单/视频更新(主逻辑) */
        if (current_state == 1 && end_flag == 0) {
            Main_UpdateActiveMode(&charging_game_saved);//活动模式更新: 菜单/游戏/视频主循环处理
            Main_FlashLowBattery();  /* 电量0级时低频闪烁低电图标 */
        }
        else {
            // 待机/充电模式
            // STDBY和CHRG电平检测
            BatteryStatus bat = Battery_GetStatus();
            VOICE_SW_OFF;//关闭语音IC
            if (bat == BAT_CHARGING || bat == BAT_FULL) {//充电/满电模式
                uint16_t voltage = BAT_ADC_ReadVoltage();
                Main_DisableTouchInput();
                charge_exit_count = 0;
                // 确认充电中，开始充电流程
                if (!was_charging) {
                    LCD_Backlight_PWM_SetDuty(1);  // 5%亮度(省电)，PWM占空比极低实现微亮
                    peak_voltage = voltage;       // 充电初始峰值为当前电压
                    Battery_GameLevelAllowIncrease();
                    was_charging = 1;
                    Battery_ResetDisplay();       // 复位充电显示
                }
                else if (voltage > peak_voltage) {
                    peak_voltage = voltage;       // 更新峰值
                }
                BatteryLevel level = Battery_ChargeVoltageToLevel(peak_voltage);
                Battery_DrawIcon(bat, level);   // 绘制充电图标和电量
                Delay_Ms(100);           // PWM刷新周期约为100ms
                continue;//继续充电循环
            }
            /*
             * PB5/PB6电平可能瞬时变化，需确认
             * 充电唤醒后检测PA8约500ms确认是否真正拔出
             */
            if (s_standby_charge_wake && !was_charging) {
                Main_DisableTouchInput();
                if (charge_exit_count < CHARGE_EXIT_CONFIRM_COUNT) {
                    charge_exit_count++;
                    Delay_Ms(CHARGE_EXIT_CHECK_MS);
                    continue;
                }
                s_standby_charge_wake = 0;
                charge_exit_count = 0;
            }
            /*
             * 刚拔电时也可能波动
             * 需500ms连续检测确认真正拔出
             */
            if (was_charging
                && charge_exit_count < CHARGE_EXIT_CONFIRM_COUNT) {
                charge_exit_count++;
                Delay_Ms(CHARGE_EXIT_CHECK_MS);
                continue;
            }
            // 确认拔出
            charge_exit_count = 0;
            was_charging = 0;
            // 进入待机流程
            IWDG_Disable();
            end_flag = 0;
            current_state = 0;
            LCD_Backlight_PWM_Stop();//停PWM模式(省电)
            LCD_LED_SET;  // 切换GPIO模式用ODR确保持续关闭
            LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);// 全屏填充黑色，确保待机前无残留画面
            next_bank = 0x00;
            next_higvh = 0x00;
            next_high = 0x00;
            next_low = 0x00;

            if (!charging_game_saved
                && GameSave_IsSupportedMode(s_main_mode)
                && GameSave_Write(s_main_mode)) {
                charging_game_saved = 1;
            }
            System_PrepareForStandby();
            if (charging_game_saved) {
                GameSave_SaveGameWakeState(s_main_mode);
            }
            else if (BKP_ReadBackupRegister(BKP_DR1) != 0x5A5A) {
                /*
                 * BKP_DR1无有效存档(0x5A5A)时默认恢复视频模式
                 * 存储视频唤醒状态以备下次唤醒切换视频
                 */
                GameSave_SaveVideoWakeState(current_video_index);
            }
            NVIC_SystemReset();//系统复位(经IWDG复位)进入Standby
        }
    }
#endif
}
