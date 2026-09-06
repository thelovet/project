#include "display.h"
#include "battery.h"
#include "touch.h"
#include "menu.h"
#include "game_save.h"

#define BKP_LOWBATT_SAVE_FLAG  BKP_DR5
#define BKP_LOWBATT_SAVE_MAGIC 0xA5A5  /* 低电已保存标志 */
static uint8_t g_lowbatt_game_dirty = 0;  /* 游戏状态已变化标记，仅RAM，关机时同步到BKP */

extern MainMode s_main_mode;
#define LCD_WIDTH   240
#define LCD_HEIGHT  240
#define SEG_WIDTH   12
#define SEG_HEIGHT  24
#define SEG_THICK   4
#define START_X     ((LCD_WIDTH - SEG_WIDTH * 3 * 3) / 2)
static const uint8_t seg_digits[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

/** 低电图标几何参数，由 LowBattery_ComputeGeo 统一计算，消除两处绘制函数的重复 */
typedef struct {
    uint16_t bw, bh, bx, by;               /* 电池主体: 宽/高/左上角坐标 */
    uint16_t nub_w, nub_h, nub_x, nub_y;   /* 电池凸起: 宽/高/左上角坐标 */
    uint16_t ex_w, ex_h, dot_h, gap;       /* 感叹号: 竖条宽/高、点高、间距 */
    uint16_t ex_x, ex_y;                   /* 感叹号: 左上角坐标 */
} LowBatteryGeo;

/**
 * @brief  根据屏幕尺寸计算低电图标所有几何参数
 * @param  geo  输出几何参数
 */
static void LowBattery_ComputeGeo(LowBatteryGeo *geo)
{
    geo->bw = LCD_WIDTH * 5 / 8;                  /* 电池宽 = 5/8 屏宽 */
    geo->bh = geo->bw * 11 / 20;                  /* 电池高 ≈ 0.55 * 宽 */
    geo->bx = (LCD_WIDTH - geo->bw) / 2;           /* 居中 */
    geo->by = (LCD_HEIGHT - geo->bh) / 2;

    geo->nub_w = geo->bw / 10;                     /* 凸起宽 */
    geo->nub_h = geo->bh * 2 / 5;                  /* 凸起高 */
    geo->nub_x = geo->bx + geo->bw;
    geo->nub_y = geo->by + (geo->bh - geo->nub_h) / 2;

    geo->ex_w = geo->bw / 8;                       /* 叹号竖条宽 */
    geo->ex_h = geo->bh * 4 / 9;                   /* 叹号竖条高 */
    geo->dot_h = geo->ex_w;                        /* 叹号点高 = 竖条宽 */
    geo->gap = geo->ex_w / 2;                      /* 竖条与点间距 */
    geo->ex_x = geo->bx + (geo->bw - geo->ex_w) / 2;
    geo->ex_y = geo->by + geo->bh / 6;
}

/*
 * @fn      LCD_ShowLowBattery
 *
 * @brief   程序化绘制低电量警告图标（电池框+感叹号），同步清屏后绘制
 *
 * @return  none
 */
void LCD_ShowLowBattery(void)
{
    LowBatteryGeo g;
    LowBattery_ComputeGeo(&g);

    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);       /* 同步清屏，防止残留上一画面 */

    /* 电池外框（红色，1像素边框） */
    LCD_Fill(g.bx, g.by, g.bx + g.bw, g.by + 1, LCD_RED);                       /* 上边 */
    LCD_Fill(g.bx, g.by + g.bh - 1, g.bx + g.bw, g.by + g.bh, LCD_RED);          /* 下边 */
    LCD_Fill(g.bx, g.by, g.bx + 1, g.by + g.bh, LCD_RED);                        /* 左边 */
    LCD_Fill(g.bx + g.bw - 1, g.by, g.bx + g.bw, g.by + g.bh, LCD_RED);          /* 右边 */

    /* 电池凸起（红色外框+黑色填充） */
    LCD_Fill(g.nub_x, g.nub_y, g.nub_x + g.nub_w, g.nub_y + g.nub_h, LCD_RED);
    LCD_Fill(g.nub_x + 1, g.nub_y + 1, g.nub_x + g.nub_w - 1, g.nub_y + g.nub_h - 1, LCD_BLACK);

    /* 感叹号 */
    LCD_Fill(g.ex_x, g.ex_y, g.ex_x + g.ex_w, g.ex_y + g.ex_h, LCD_RED);
    LCD_Fill(g.ex_x, g.ex_y + g.ex_h + g.gap, g.ex_x + g.ex_w, g.ex_y + g.ex_h + g.gap + g.dot_h, LCD_RED);
}

/**
 * @brief  绘制低电图标（电池框+感叹号），叠加在游戏画面上不刷黑屏
 * @param  color  显示颜色（LCD_RED=绘制, LCD_BLACK=清除）
 */
static void LCD_DrawLowBatteryOverlay(uint16_t color)
{
    LowBatteryGeo g;
    LowBattery_ComputeGeo(&g);

    /* 电池外框，边框加粗到2像素 */
    LCD_Fill(g.bx, g.by, g.bx + g.bw, g.by + 2, color);                          /* 上边 */
    LCD_Fill(g.bx, g.by + g.bh - 2, g.bx + g.bw, g.by + g.bh, color);             /* 下边 */
    LCD_Fill(g.bx, g.by, g.bx + 2, g.by + g.bh, color);                          /* 左边 */
    LCD_Fill(g.bx + g.bw - 2, g.by, g.bx + g.bw, g.by + g.bh, color);             /* 右边 */

    /* 电池凸起填充 */
    LCD_Fill(g.nub_x, g.nub_y, g.nub_x + g.nub_w, g.nub_y + g.nub_h, color);

    /* 感叹号 */
    LCD_Fill(g.ex_x, g.ex_y, g.ex_x + g.ex_w, g.ex_y + g.ex_h, color);
    LCD_Fill(g.ex_x, g.ex_y + g.ex_h + g.gap, g.ex_x + g.ex_w,
             g.ex_y + g.ex_h + g.gap + g.dot_h, color);
}

/**
 * @brief  显示低电图标，红色电池框+感叹号覆盖在游戏画面上
 * @note   每3秒闪烁1秒，游戏启用时不刷黑屏直接覆盖绘制
 */
void LCD_ShowLowBatteryOverlay(void)
{
    LCD_DrawLowBatteryOverlay(LCD_RED);
}

/**
 * @brief  清除低电图标，仅刷黑图标线条，不刷整个矩形区域
 * @note   与显示使用同一绘制函数传入LCD_BLACK，仅覆盖红色像素
 */
void LCD_ClearLowBatteryOverlay(void)
{
    LCD_DrawLowBatteryOverlay(LCD_BLACK);
}

/**
 * @brief 显示3位数字（最大999）
 * @param num 要显示的数字（最大999）
 * @param y 数字显示的y坐标
 */
void LCD_ShowNum(uint16_t num, uint16_t y)
{
    if(num > 999) num = 999;
    uint8_t hundreds = num / 100;
    uint8_t tens = (num % 100) / 10;
    uint8_t ones = num % 10;
    uint8_t dgts[3] = {hundreds, tens, ones};
    uint16_t bg = LCD_BLACK, fg = LCD_GBLUE;
    for(int d = 0; d < 3; d++) {
        uint16_t xb = START_X + d * SEG_WIDTH * 3;
        uint8_t sd = seg_digits[dgts[d]];
        LCD_Fill(xb - 2, y - 2, xb + SEG_WIDTH + 2, y + SEG_HEIGHT + 2, bg);
        if(sd & 0x01) LCD_Fill(xb + SEG_THICK, y, xb + SEG_WIDTH - SEG_THICK, y + SEG_THICK, fg);
        if(sd & 0x02) LCD_Fill(xb + SEG_WIDTH - SEG_THICK, y + SEG_THICK, xb + SEG_WIDTH, y + SEG_HEIGHT/2, fg);
        if(sd & 0x04) LCD_Fill(xb + SEG_WIDTH - SEG_THICK, y + SEG_HEIGHT/2, xb + SEG_WIDTH, y + SEG_HEIGHT - SEG_THICK, fg);
        if(sd & 0x08) LCD_Fill(xb + SEG_THICK, y + SEG_HEIGHT - SEG_THICK, xb + SEG_WIDTH - SEG_THICK, y + SEG_HEIGHT, fg);
        if(sd & 0x10) LCD_Fill(xb, y + SEG_HEIGHT/2, xb + SEG_THICK, y + SEG_HEIGHT - SEG_THICK, fg);
        if(sd & 0x20) LCD_Fill(xb, y + SEG_THICK, xb + SEG_THICK, y + SEG_HEIGHT/2, fg);
        if(sd & 0x40) LCD_Fill(xb + SEG_THICK, y + SEG_HEIGHT/2 - SEG_THICK/2, xb + SEG_WIDTH - SEG_THICK, y + SEG_HEIGHT/2 + SEG_THICK/2, fg);
    }
}

/**
 * @brief 统一准备Flash、触摸和LCD进入待机
 */
void System_PrepareForStandby(void)
{
    SPI_Flash_PowerDown();// Flash进入PowerDown模式
    CST816S_SleepEnable();// CST816S进入待机模式
    LCD_Enter_Sleep();// LCD进入待机模式
    LCD_PWR_OFF;// 关闭LCD电源
    Touch_WaitWakeInputsReleased();// 等待触摸输入释放
}



/*
 * @brief 检查电池电压并显示低电量警告图标
 *          检测电池电压是否低于2.5V，若低于则显示低电量警告图标
 *          充电中或充满时不显示低电警告，让主循环显示充电图标
 */
void CheckAndShowLowBattery(void)
{
    // 第二道：2.5V <= VDD < 2.6V，显示低电警告
    // 充电中或充满时不显示低电警告，让主循环显示充电图标
    
    if (Battery_IsCharging() || Battery_GetStatus() == BAT_FULL) {
        return;  // 有外部供电，跳过低电检测
    }
    
    PWR_PVDLevelConfig(PWR_PVDLevel_2V6);  // 阈值2.6V
    PWR_PVDCmd(ENABLE);
    Delay_Ms(1);
    // 多次检测去抖，防止PVD误触发
    // uint8_t pvd_count = 0;
    // for (uint8_t i = 0; i < 50; i++) {
    //     Delay_Ms(2);
    //     if (PWR_GetFlagStatus(PWR_FLAG_PVDO) == SET) pvd_count++;
    // }
    if (PWR_GetFlagStatus(PWR_FLAG_PVDO) == SET)  // VDD < 2.6V（多数采集中）
    {
        /* PWM初始化后PB9是复用输出，必须先恢复为GPIO才能直接控制。 */
        LCD_Backlight_PWM_Stop();  // 恢复PB9为GPIO并关背光
        Delay_Ms(10);              // 消除花屏
        LCD_ShowLowBattery();       // 显示"电池电压低"
        LCD_LED_CLR;                // 开背光
        for (uint8_t i = 0; i < 5; i++) {
            IWDG_ReloadCounter();    // 看门狗约1秒，防止提示期间提前复位
            Delay_Ms(100);
        }
        LCD_LED_SET;                // 关背光

        /* 低电关机前保存游戏状态 */
        RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
        PWR_BackupAccessCmd(ENABLE);
        /* 若游戏状态有变化，清除BKP标记以触发重新保存 */
        if (g_lowbatt_game_dirty) {
            BKP_WriteBackupRegister(BKP_LOWBATT_SAVE_FLAG, 0x0000);
            g_lowbatt_game_dirty = 0;
        }
        /* 已保存(DR5=0xA5A5)则跳过，防止频繁擦写Flash */
        if (BKP_ReadBackupRegister(BKP_LOWBATT_SAVE_FLAG)
            != BKP_LOWBATT_SAVE_MAGIC
            && GameSave_IsSupportedMode(s_main_mode)) {
            if (GameSave_Write(s_main_mode)) {
                BKP_WriteBackupRegister(BKP_LOWBATT_SAVE_FLAG,
                                        BKP_LOWBATT_SAVE_MAGIC);
                GameSave_SaveGameWakeState(s_main_mode);
            }
        }

        System_PrepareForStandby();
        NVIC_SystemReset();    // 软复位 → main开头检测SFTRST → 直接Standby
    }
}

/**
 * @brief  清除低电保存标记，表示游戏状态已变化，下次低电需重新保存
 * @note   由各游戏在状态变化时调用，仅置RAM变量，关机时统一写BKP
 */
void Main_ClearLowBattSaveFlag(void)
{
    g_lowbatt_game_dirty = 1;  /* 仅置RAM标记，避免每帧写BKP */
}
