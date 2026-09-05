/**
 * @file    settings.c
 * @brief   设置模块: 亮度 / 息屏时间 / 屏幕方向 三个设置页
 * @note    视频上滑菜单与各游戏的设置二级菜单都复用本模块;
 *          参数经 BKP 后备寄存器持久化(Settings_SaveAll), 设置页由 main.c
 *          的 Settings_Update 统一调度, 返回键/下滑返回在 main.c 处理。
 */

#include "settings.h"
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "sok_img_win.h"
#include "ch32v20x_bkp.h"

#define BKP_SLEEP_MAGIC  0xB071
#define BKP_SLEEP_DR_MAGIC  BKP_DR6
#define BKP_SLEEP_DR_INDEX  BKP_DR7
#define BKP_BRIGHT_DR       BKP_DR8
#define BKP_ORIENT_DR       BKP_DR10

/* 统一保存: 确保DR6/DR7/DR8/DR10同时有效, 避免只保存一项时另一项为0导致唤醒后不亮 */
static void Settings_SaveAll(void)
{
    BKP_WriteBackupRegister(BKP_SLEEP_DR_MAGIC, BKP_SLEEP_MAGIC);
    BKP_WriteBackupRegister(BKP_SLEEP_DR_INDEX, g_sleep_time_index);
    BKP_WriteBackupRegister(BKP_BRIGHT_DR, g_brightness);
    BKP_WriteBackupRegister(BKP_ORIENT_DR, g_orientation_index);
}

/* ---- 全局设置变量 ---- */
uint8_t g_brightness = 100;
uint8_t g_sleep_time_index = 2;
uint8_t g_orientation_index = 0;
const uint16_t g_sleep_times[6] = {2, 5, 10, 20, 30, 60};

static const uint8_t madctl_rotations[4] = {0x08, 0x68, 0xC8, 0xA8};
static SettingsMode s_mode = SETTINGS_NONE;

/* 前置声明 */
static void Brightness_Draw(void);
/* 重绘当前设置页(只重绘、不处理触摸), 供低电图标灭段恢复时调用 */
static void Brightness_DrawSlider(void);
static void SleepTime_Draw(void);
static void Orientation_Draw(void);
static void Sleep_DrawBody(void);          /* 设置页内容局部重绘(低电区域刷新用) */
static void Orientation_DrawBody(void);
void Settings_Redraw(void)
{
    switch (s_mode) {
    case SETTINGS_BRIGHTNESS:  Brightness_Draw();  break;
    case SETTINGS_SLEEP_TIME:  SleepTime_Draw();   break;
    case SETTINGS_ORIENTATION: Orientation_Draw(); break;
    default: break;
    }
}

/* 低电灭段恢复: 只重绘电池图标覆盖区的当前设置页内容(不整屏清黑, 避免电池消失后整屏闪一下) */
void Settings_RedrawRegion(void)
{
    LCD_ClearLowBatteryOverlay();   /* 清红色图标(红→黑) */
    switch (s_mode) {
    case SETTINGS_BRIGHTNESS:  Brightness_DrawSlider();  break;
    case SETTINGS_SLEEP_TIME:  Sleep_DrawBody();         break;
    case SETTINGS_ORIENTATION: Orientation_DrawBody();   break;
    default: break;
    }
}



#define DIGIT_W 14
#define DIGIT_H 28

/* 亮度 */
#define BR_X    44   /* (240-152)/2 居中 */
#define BR_W    152
#define BR_Y    108
#define BR_H    12
#define BR_KNOB_W 10
#define BR_KNOB_H 20
#define BR_NUM_CX  120                       /* 数字居中X */
#define BR_NUM_Y   (BR_Y - 30)               /* 数字Y中心 (78) */

#define TAP_THRESHOLD 15

static uint8_t  s_br_drag = 0;
static int16_t  s_br_pos = BR_X + BR_W;

/* 息屏 */
#define ST_Y   100
#define ST_H   34
#define ST_W   38
#define ST_LX  46
#define ST_RX  156
#define ST_NUM_CX 120                       /* 数字居中X */
#define ST_NUM_Y  122                       /* 数字中心Y */

static uint8_t s_st_prev = 0;
static uint16_t s_st_start_x = 0;
static uint16_t s_st_start_y = 0;

/* 方向 */
#define OR_X  120
#define OR_Y  120
#define OR_SZ 60                            /* 箭头更大 */

static uint8_t s_or_prev = 0;
static uint16_t s_or_start_x = 0;
static uint16_t s_or_start_y = 0;

/* ---- 辅助: 用2048字库画居中数字 ---- */
static void DrawDigitsCenter(uint16_t cx, uint16_t cy,
                              uint16_t num, uint16_t color)
{
    uint8_t digits[3];
    uint8_t n = 0;
    if (num == 0) {
        digits[0] = 0;
        n = 1;
    } else {
        uint16_t tmp = num;
        while (tmp > 0) { tmp /= 10; n++; }
        tmp = num;
        for (uint8_t i = n; i > 0; i--) {
            digits[i - 1] = (uint8_t)(tmp % 10);
            tmp /= 10;
        }
    }

    uint16_t total_w = (uint16_t)n * DIGIT_W;
    uint16_t sx = cx - total_w / 2;
    uint16_t sy = cy - DIGIT_H / 2;

    LCD_CS_CLR;
    for (uint8_t i = 0; i < n; i++) {
        sok_draw_digit_scaled(sx + (uint16_t)i * DIGIT_W, sy,
                               digits[i], color, DIGIT_W, DIGIT_H);
    }
    LCD_CS_SET;
}

/* ---- 辅助: 清除数字区域 ---- */
static void ClearDigitArea(uint16_t cx, uint16_t cy)
{
    int16_t x0 = (int16_t)cx - 50;
    int16_t y0 = (int16_t)cy - DIGIT_H / 2 - 2;
    int16_t x1 = (int16_t)cx + 50;
    int16_t y1 = (int16_t)cy + DIGIT_H / 2 + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_W - 1) x1 = LCD_W - 1;
    if (y1 > LCD_H - 1) y1 = LCD_H - 1;
    LCD_Fill((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, LCD_BLACK);
}

/* ---- 辅助: 返回按钮绘制 ---- */
static void DrawBack(void)
{
    LCD_Fill(BACK_X, BACK_Y, BACK_X + BACK_W, BACK_Y + BACK_H, 0xFFFF);
    LCD_Fill(BACK_X + 3, BACK_Y + 3, BACK_X + BACK_W - 3, BACK_Y + BACK_H - 3, 0x0000);
}

/* ---- 屏幕方向应用 ---- */
void Settings_ApplyOrientation(uint8_t index)
{
    if (index > 3) index = 0;
    LCD_CS_CLR;
    LCD_WR_REG(0x36);
    LCD_WR_DATA(madctl_rotations[index]);
    LCD_CS_SET;
}

/* ---- 进入设置 ---- */
void Settings_Enter(SettingsMode mode)
{
    s_mode = mode;
    s_br_drag = 0;
    s_st_prev = 0;
    s_st_start_x = 0;
    s_st_start_y = 0;
    s_or_prev = 0;
    s_or_start_x = 0;
    s_or_start_y = 0;
    s_br_pos = BR_X + (int16_t)((uint32_t)g_brightness * BR_W / 100);
    switch (mode) {
    case SETTINGS_BRIGHTNESS:  Brightness_Draw();  break;
    case SETTINGS_SLEEP_TIME:  SleepTime_Draw();   break;
    case SETTINGS_ORIENTATION: Orientation_Draw(); break;
    default: break;
    }
}

/* ================================================================
 *  亮度设置
 * ================================================================ */
static void Brightness_DrawSlider(void)
{
    /* 轨道背景 */
    LCD_Fill(BR_X - BR_KNOB_W/2, BR_Y - 3,
             BR_X + BR_W + BR_KNOB_W/2, BR_Y + BR_H + 3, 0x4208);
    /* 激活填充 */
    LCD_Fill(BR_X, BR_Y, s_br_pos, BR_Y + BR_H, 0x07FF);
    /* 滑块 */
    LCD_Fill(s_br_pos - BR_KNOB_W/2, BR_Y - 3,
             s_br_pos + BR_KNOB_W/2, BR_Y + BR_H + 3, 0xFFE0);
    /* 上方数字 */
    ClearDigitArea(BR_NUM_CX, BR_NUM_Y);
    DrawDigitsCenter(BR_NUM_CX, BR_NUM_Y, g_brightness, LCD_GBLUE);
}

static void Brightness_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Brightness_DrawSlider();
}

static uint8_t Brightness_Update(void)
{
    static uint16_t s_br_start_y = 0;
    TouchState ts;
    Touch_Scan(&ts);
    if (ts.pressed) {
        /* 只响应起点在滑块区域的触摸, 从底部上划菜单扫过的忽略 */
        if (!s_br_drag) {
            if (!s_br_start_y) s_br_start_y = ts.y;  /* 记录起点Y */
            if (s_br_start_y >= BR_Y - 18 && s_br_start_y <= BR_Y + BR_H + 18
                && ts.x >= BR_X && ts.x <= BR_X + BR_W) {
                s_br_drag = 1;
                s_br_pos = (int16_t)ts.x;  /* 直接跳到点击位置 */
                g_brightness = (uint8_t)((uint32_t)(s_br_pos - BR_X) * 99 / BR_W + 1);
                LCD_Backlight_PWM_SetDuty(g_brightness);
                Brightness_DrawSlider();
            }
        }
        /* 拖拽中: 任意Y位置, 左右滑动均可调节 */
        if (s_br_drag) {
            GameAutoSleep_KeepAlive();  /* 拖动中持续重置休眠计时器 */
            int16_t nx = (int16_t)ts.x;
            if (nx < BR_X) nx = BR_X;
            if (nx > BR_X + BR_W) nx = BR_X + BR_W;
            if (nx != s_br_pos) {
                s_br_pos = nx;
                g_brightness = (uint8_t)((uint32_t)(nx - BR_X) * 99 / BR_W + 1);  /* 最低亮度1% */
                LCD_Backlight_PWM_SetDuty(g_brightness);
                Brightness_DrawSlider();
            }
        }
    } else {
        s_br_start_y = 0;                           /* 松手时重置起点 */
        if (s_br_drag) Settings_SaveAll();          /* 松手时保存 */
        s_br_drag = 0;
    }
    return 1;
}

/* ================================================================
 *  息屏时间设置
 * ================================================================ */
static void Sleep_DrawBody(void)
{
    /* "-" 按钮 */
    LCD_Fill(ST_LX, ST_Y, ST_LX + ST_W, ST_Y + ST_H, 0x07FF);
    LCD_Fill(ST_LX + 12, ST_Y + ST_H/2 - 2, ST_LX + ST_W - 12, ST_Y + ST_H/2 + 2, 0x0000);
    /* "+" 按钮 */
    LCD_Fill(ST_RX, ST_Y, ST_RX + ST_W, ST_Y + ST_H, 0x07FF);
    LCD_Fill(ST_RX + 12, ST_Y + ST_H/2 - 2, ST_RX + ST_W - 12, ST_Y + ST_H/2 + 2, 0x0000);
    LCD_Fill(ST_RX + ST_W/2 - 2, ST_Y + 11, ST_RX + ST_W/2 + 2, ST_Y + ST_H - 11, 0x0000);
    /* 数字(左移给S留空) */
    DrawDigitsCenter(ST_NUM_CX - DIGIT_W/2, ST_NUM_Y,
                     g_sleep_times[g_sleep_time_index], LCD_GBLUE);
    /* S 手画 (缩到20x14, 匹配数字) */
    {
        static const uint16_t s_src[30] = {
            0x0000,0x0000,0x0000,0x07E0,0x1FFC,0x3F7C,0x780E,0x700E,
            0x700E,0x700E,0x7000,0x7800,0x3E00,0x3F80,0x0FE0,0x07F0,
            0x01F8,0x007C,0x001E,0x000E,0xE00E,0xE00E,0xE00E,0xE01C,
            0xF87C,0x7FF8,0x1FE0,0x0000,0x0000,0x0000
        };
        uint16_t ds[22]; uint8_t dr;
        for (dr = 0; dr < 20; dr++) {
            uint8_t sr = 3 + dr * 23 / 19;             /* 24行源→20行输出 */
            ds[dr] = s_src[sr] >> 1;                    /* 裁掉第1列 */
        }
        ds[20] = 0; ds[21] = 0;
        uint16_t xs = ST_NUM_CX + DIGIT_W/2 + 1;
        uint16_t ys = ST_NUM_Y - 10;                    /* 20高, 居中 */
        for (dr = 0; dr < 20; dr++) {
            uint16_t b = ds[dr]; uint8_t c = 0, st;
            while (c < 14) {
                while (c < 14 && !(b & 0x8000)) { b <<= 1; c++; }
                if (c >= 14) break;
                st = c;
                while (c < 14 && (b & 0x8000)) { b <<= 1; c++; }
                LCD_Fill(xs + st, ys + dr, xs + c, ys + dr + 1, LCD_GBLUE);
            }
        }
    }
}

static void SleepTime_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Sleep_DrawBody();
}

static uint8_t SleepTime_Update(void)
{
    TouchState ts;
    Touch_Scan(&ts);
    if (ts.pressed) {
        if (!s_st_prev) {
            s_st_prev = 1;
            s_st_start_x = ts.x;
            s_st_start_y = ts.y;
        }
    } else if (s_st_prev) {
        s_st_prev = 0;
        int16_t dx = (int16_t)ts.x - (int16_t)s_st_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_st_start_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        /* 仅Y<200的区域响应点击, 屏蔽下方200~240(防止划菜单时误触) */
        if (dx <= TAP_THRESHOLD && dy <= TAP_THRESHOLD
            && s_st_start_y < 200) {
            if (s_st_start_x < 120) {
                if (g_sleep_time_index > 0) {
                    g_sleep_time_index--;
                    Settings_SaveAll();
                    SleepTime_Draw();
                }
            } else {
                if (g_sleep_time_index < 5) {
                    g_sleep_time_index++;
                    Settings_SaveAll();
                    SleepTime_Draw();
                }
            }
        }
    }
    return 1;
}

/* ================================================================
 *  屏幕方向设置
 * ================================================================ */
static void Orientation_DrawBody(void)
{
    int16_t cx = OR_X, cy = OR_Y, s = OR_SZ;
    /* 上箭头+右标记(屏幕方向由MADCTL寄存器旋转) */
    LCD_Fill(cx - 4,  cy,         cx + 4,  cy + s,    0xFFE0);   /* 杆 */
    LCD_Fill(cx + 20, cy - 34,    cx + 26, cy - 28,   0xFFE0);   /* 右标记 */
    for (uint8_t i = 0; i < 5; i++) {
        LCD_Fill(cx - 4 - i * 6, cy - 40 + i * 8, cx - i * 6,     cy - 32 + i * 8, 0xFFE0);
        LCD_Fill(cx + i * 6,     cy - 40 + i * 8, cx + 4 + i * 6, cy - 32 + i * 8, 0xFFE0);
    }
}

static void Orientation_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Orientation_DrawBody();
}

static uint8_t Orientation_Update(void)
{
    TouchState ts;
    Touch_Scan(&ts);
    if (ts.pressed) {
        if (!s_or_prev) {
            s_or_prev = 1;
            s_or_start_x = ts.x;
            s_or_start_y = ts.y;
        }
    } else if (s_or_prev) {
        s_or_prev = 0;
        int16_t dx = (int16_t)ts.x - (int16_t)s_or_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_or_start_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= TAP_THRESHOLD && dy <= TAP_THRESHOLD) {
            g_orientation_index = (g_orientation_index + 1) & 3;
            Settings_ApplyOrientation(g_orientation_index);
            Settings_SaveAll();
            Orientation_Draw();
        }
    }
    return 1;
}

/* ================================================================
 *  退出设置
 * ================================================================ */
void Settings_Exit(void)
{
    s_mode = SETTINGS_NONE;
    s_br_drag = 0;
    s_st_prev = 0;
    s_st_start_x = 0;
    s_st_start_y = 0;
    s_or_prev = 0;
    s_or_start_x = 0;
    s_or_start_y = 0;
}

/* ================================================================
 *  统一入口
 * ================================================================ */
uint8_t Settings_Update(void)
{
    if (s_mode == SETTINGS_NONE) return 0;
    switch (s_mode) {
    case SETTINGS_BRIGHTNESS:  return Brightness_Update();
    case SETTINGS_SLEEP_TIME:  return SleepTime_Update();
    case SETTINGS_ORIENTATION: return Orientation_Update();
    default: s_mode = SETTINGS_NONE; return 0;
    }
}
