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
#include "flash_store.h"    /* 语言设置页的字模从外部Flash读 */
#include "ch32v20x_bkp.h"

/* ================= 常量与全局设置定义(统一置顶) ================= */

/* ---- 设置持久化(BKP 后备寄存器) ---- */
#define BKP_SLEEP_MAGIC  0xB071
#define BKP_SLEEP_DR_MAGIC  BKP_DR6
#define BKP_SLEEP_DR_INDEX  BKP_DR7
#define BKP_BRIGHT_DR       BKP_DR8
#define BKP_ORIENT_DR       BKP_DR10
#define BKP_LANG_DR         BKP_DR11   /* 语言索引 (DR1~DR10 已被其它模块占用) */

/* ---- 数字绘制尺寸 ---- */
#define DIGIT_W 14
#define DIGIT_H 28

/* ---- 亮度设置页 ---- */
#define BR_X    44   /* (240-152)/2 居中 */
#define BR_W    152
#define BR_Y    108
#define BR_H    12
#define BR_KNOB_W 10
#define BR_KNOB_H 20
#define BR_NUM_CX  120                       /* 数字居中X */
#define BR_NUM_Y   (BR_Y - 30)               /* 数字Y中心 (78) */
#define TAP_THRESHOLD 15
#define BR_SMOOTH_DIV 3                      /* 滑块位置平滑强度: 每帧向手指位置靠近 1/该值(防手抖带得数字乱跳) */
#define BR_MAX_LAG    8                      /* 滑块最大滞后(px): 快拖时最多落后手指这么多, 超出部分直接跟上 */
static uint8_t  s_br_drag = 0;              /* 亮度拖动中标志 */
static int16_t  s_br_pos = BR_X + BR_W;     /* 当前滑块位置 */
static int16_t  s_br_smooth_x = 0;          /* 平滑后的触摸X(防手抖) */

/* ---- 息屏时间设置页 ---- */
#define ST_Y   100
#define ST_H   34
#define ST_W   38
#define ST_LX  46
#define ST_RX  156
#define ST_NUM_CX 120                       /* 数字居中X */
#define ST_NUM_Y  122                       /* 数字中心Y */
static uint8_t s_st_prev = 0;               /* 上次按下状态 */
static uint16_t s_st_start_x = 0;           /* 按下起点X */
static uint16_t s_st_start_y = 0;           /* 按下起点Y */

/* ---- 屏幕方向设置页 ---- */
#define OR_X  120
#define OR_Y  120
#define OR_SZ 60                            /* 箭头更大 */
static uint8_t s_or_prev = 0;               /* 上次按下状态 */
static uint16_t s_or_start_x = 0;           /* 按下起点X */
static uint16_t s_or_start_y = 0;           /* 按下起点Y */

/* ---- 语言设置页 (中文 / English) ---- */
#define LG_CN_Y     66                      /* "中文" 行顶部Y (2字 x 24 = 48宽, 居中) */
#define LG_EN_Y    140                      /* "English" 行顶部Y (7字 x 16 = 112宽, 居中) */
#define LG_MARK_X   28                      /* 选中标记方块X (左侧指示列) */
#define LG_MARK_SZ  12
#define LG_LAT_DY    2                      /* 半宽字母基线与24x24汉字差2行, 下移对齐 */
#define LG_TAP_SPLIT 110                    /* 点击分界Y: 上半=中文, 下半=English */
static uint8_t s_lg_prev = 0;               /* 上次按下状态 */
static uint16_t s_lg_start_x = 0;           /* 按下起点X */
static uint16_t s_lg_start_y = 0;           /* 按下起点Y */

/* ---- 全局设置变量 ---- */
uint8_t g_brightness = 100;                 /* 亮度 1~100 */
uint8_t g_sleep_time_index = 2;             /* 息屏时间索引(0=2s ... 5=60s) */
uint8_t g_orientation_index = 0;            /* 屏幕方向索引 0~3 */
uint8_t g_language_index = 0;               /* 语言: 0=中文, 1=English */
const uint16_t g_sleep_times[6] = {2, 5, 10, 20, 30, 60};   /* 可选息屏秒数 */
static const uint8_t madctl_rotations[4] = {0x08, 0x68, 0xC8, 0xA8};  /* MADCTL 方向 */
static SettingsMode s_mode = SETTINGS_NONE;  /* 当前设置页 */

/* ---- 各设置页函数前置声明 ---- */
static void Brightness_Draw(void);
static void Brightness_DrawSlider(void);
static void SleepTime_Draw(void);
static void Orientation_Draw(void);
static void Language_Draw(void);
static void Sleep_DrawBody(void);
static void Orientation_DrawBody(void);
static void Language_DrawBody(void);

/**
* @brief  统一保存所有设置到 BKP 后备寄存器(魔数/息屏时间/亮度/方向/语言)。
* @note   确保 DR6/DR7/DR8/DR10 同时有效, 避免只存一项时另一项为 0 导致唤醒后不亮。
*/
static void Settings_SaveAll(void)
{
    BKP_WriteBackupRegister(BKP_SLEEP_DR_MAGIC, BKP_SLEEP_MAGIC);
    BKP_WriteBackupRegister(BKP_SLEEP_DR_INDEX, g_sleep_time_index);
    BKP_WriteBackupRegister(BKP_BRIGHT_DR, g_brightness);
    BKP_WriteBackupRegister(BKP_ORIENT_DR, g_orientation_index);
    BKP_WriteBackupRegister(BKP_LANG_DR, g_language_index);
}

/**
* @brief  重绘当前设置页(整页: 清黑+返回键+内容)。
* @note   只重绘、不处理触摸, 供设置页刷新/进入时调用。
*/
void Settings_Redraw(void)
{
    switch (s_mode) {
    case SETTINGS_BRIGHTNESS:  Brightness_Draw();  break;
    case SETTINGS_SLEEP_TIME:  SleepTime_Draw();   break;
    case SETTINGS_ORIENTATION: Orientation_Draw(); break;
    case SETTINGS_LANGUAGE:    Language_Draw();    break;
    default: break;
    }
}

/**
* @brief  低电灭段恢复: 只重绘电池图标覆盖区内的当前设置页内容。
* @note   先清红色低电图标(红→黑), 再按 s_mode 局部重绘, 不整屏清黑, 避免消失后整屏闪一下。
*/
void Settings_RedrawRegion(void)
{
    LCD_ClearLowBatteryOverlay();   /* 清红色图标(红→黑) */
    switch (s_mode) {
    case SETTINGS_BRIGHTNESS:  Brightness_DrawSlider();  break;
    case SETTINGS_SLEEP_TIME:  Sleep_DrawBody();         break;
    case SETTINGS_ORIENTATION: Orientation_DrawBody();   break;
    case SETTINGS_LANGUAGE:    Language_DrawBody();      break;
    default: break;
    }
}


/**
* @brief  用 2048 字库把数值居中绘制在 (cx, cy), 支持 0~999。
* @param  cx    数字中心 X
* @param  cy    数字中心 Y
* @param  num   要显示的数值(0~999)
* @param  color 数字颜色(RGB565)
* @note   内部 LCD_CS_CLR/SET, 调用前勿假设 CS 状态。
*/
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

/**
* @brief  清除数字显示区域(以 cx 为中心, 宽±50, 高=数字高+4)为黑。
* @param  cx 数字中心 X
* @param  cy 数字中心 Y
* @note   只清数字附近, 不影响整框/文字。
*/
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

/**
* @brief  绘制返回按钮: 白底黑色方框(3px 内缩)。
* @note   按钮宏为 BACK_X/BACK_Y/BACK_W/BACK_H。
*/
static void DrawBack(void)
{
    LCD_Fill(BACK_X, BACK_Y, BACK_X + BACK_W, BACK_Y + BACK_H, 0xFFFF);
    LCD_Fill(BACK_X + 3, BACK_Y + 3, BACK_X + BACK_W - 3, BACK_Y + BACK_H - 3, 0x0000);
}

/**
* @brief  应用屏幕方向(写 MADCTL 0x36 寄存器)。
* @param  index 方向索引 0~3(越界钳位为 0)
*/
void Settings_ApplyOrientation(uint8_t index)
{
    if (index > 3) index = 0;
    LCD_CS_CLR;
    LCD_WR_REG(0x36);
    LCD_WR_DATA(madctl_rotations[index]);
    LCD_CS_SET;
}

/**
* @brief  进入指定设置页并立即重绘。
* @param  mode 设置页枚举(SETTINGS_BRIGHTNESS/SLEEP_TIME/ORIENTATION/LANGUAGE)
* @note   重置各页触摸状态, 并把亮度滑块 s_br_pos 初始到当前亮度位置。
*/
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
    s_lg_prev = 0;
    s_lg_start_x = 0;
    s_lg_start_y = 0;
    s_br_pos = BR_X + (int16_t)((uint32_t)g_brightness * BR_W / 100);
    switch (mode) {
    case SETTINGS_BRIGHTNESS:  Brightness_Draw();  break;
    case SETTINGS_SLEEP_TIME:  SleepTime_Draw();   break;
    case SETTINGS_ORIENTATION: Orientation_Draw(); break;
    case SETTINGS_LANGUAGE:    Language_Draw();    break;
    default: break;
    }
}

/**
* @brief  绘制亮度页内容(轨道/激活填充/滑块/上方数字), 用于局部刷新。
* @note   不动整框/返回键, 避免整页重画闪烁; 供拖拽或参数变化时调用。
*/
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

/**
* @brief  绘制亮度设置整页: 清黑 + 返回键 + 滑块内容。
*/
static void Brightness_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Brightness_DrawSlider();
}

/**
* @brief  处理亮度页触摸: 点/拖滑块调节亮度, 松手保存。
* @return 恒返回 1(页面活跃)。
* @note   只响应起点在滑块区域的触摸; 拖动中任意 Y 可左右调; 松手时 Settings_SaveAll。
*/
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
                s_br_smooth_x = s_br_pos;  /* 平滑值从点击位置起算 */
                g_brightness = (uint8_t)((uint32_t)(s_br_pos - BR_X) * 99 / BR_W + 1);
                LCD_Backlight_PWM_SetDuty(g_brightness);
                Brightness_DrawSlider();
            }
        }
        /* 拖拽中: 任意Y位置, 左右滑动均可调节 */
        if (s_br_drag) {
            GameAutoSleep_KeepAlive();  /* 拖动中持续重置休眠计时器 */
            int16_t nx = (int16_t)ts.x;
            /* 指数平滑(与 fish 拖动同一手法) + 最大滞后上限: 触摸坐标有 ±1~2px 抖动, 直接把坐标
               映射成亮度会让滑块和数字一起抖, 平滑后静止时基本不动。但固定 1/3 增益在快速拖动
               (一两帧走完)时只能走到一半(与鱼"快滑只走一半"同因), 故与目标距离超过 BR_MAX_LAG
               时把超出部分直接走掉; 小幅抖动(<= BR_MAX_LAG)仍按 1/3 平滑, 去抖效果不变。 */
            {
                int16_t dx = (int16_t)(nx - s_br_smooth_x);
                int16_t step = (int16_t)(dx / BR_SMOOTH_DIV);
                if (dx > BR_MAX_LAG)       step = (int16_t)(dx - BR_MAX_LAG);
                else if (dx < -BR_MAX_LAG) step = (int16_t)(dx + BR_MAX_LAG);
                s_br_smooth_x = (int16_t)(s_br_smooth_x + step);
            }
            nx = s_br_smooth_x;
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

/**
* @brief  绘制息屏时间页内容("-" 按钮/数字/"S" 图标), 用于局部刷新。
* @note   不含整框清黑/返回键, 供低电灭段恢复或增量刷新调用。
*/
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

/**
* @brief  绘制息屏时间设置整页: 清黑 + 返回键 + 内容。
*/
static void SleepTime_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Sleep_DrawBody();
}

/**
* @brief  处理息屏时间页触摸: 点 "-"/"+" 增减, 松手保存。
* @return 恒返回 1(页面活跃)。
* @note   仅 Y<200 的点击响应, 屏蔽下方 200~240 防划菜单误触。
*/
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

/**
* @brief  绘制屏幕方向页内容(上箭头+右标记), 用于局部刷新。
* @note   屏幕方向由 MADCTL 寄存器旋转, 此处仅画示意箭头。
*/
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

/**
* @brief  绘制屏幕方向设置整页: 清黑 + 返回键 + 内容。
*/
static void Orientation_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Orientation_DrawBody();
}

/**
* @brief  处理屏幕方向页触摸: 点击即循环切换方向(0~3)并保存。
* @return 恒返回 1(页面活跃)。
* @note   单击切换方向并写 MADCTL 旋转, 松手时保存。
*/
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

/**
* @brief  从外部Flash读一个 24x24 全宽汉字(72B)并绘制。
* @note   字模在资源表末尾(115~118), 没重烧字库时读失败 -> 该字不显示(其它正常)。
*         调用者负责 LCD_CS_CLR/SET (sok_draw_char_24 内部不管CS)。
*/
static void Lang_DrawCnChar(uint16_t x, uint16_t y, FlashStoreId id, uint16_t color)
{
    uint8_t buf[72];
    if (FlashStore_ResRead(id, 0, buf, sizeof(buf)) != sizeof(buf)) return;
    sok_draw_char_24(x, y, buf, color);
}

/**
* @brief  从外部Flash读一个 16x24 半宽字母(48B = 24行 x 2字节, MSB在左)并绘制。
* @note   y 用 LG_LAT_DY 下移, 使字母基线与 24x24 汉字视觉对齐。
*/
static void Lang_DrawLatChar(uint16_t x, uint16_t y, FlashStoreId id, uint16_t color)
{
    uint8_t buf[48];
    uint8_t py, px, seg, in;
    if (FlashStore_ResRead(id, 0, buf, sizeof(buf)) != sizeof(buf)) return;
    for (py = 0; py < 24; py++) {
        uint16_t bits = (uint16_t)(((uint16_t)buf[py * 2] << 8) | buf[py * 2 + 1]);
        seg = 0; in = 0;
        for (px = 0; px < 16; px++) {
            uint8_t fg = (uint8_t)((bits >> (15 - px)) & 0x01);
            if (fg && !in) { seg = px; in = 1; }
            else if (!fg && in) {
                LCD_Address_Set(x + seg, y + py + LG_LAT_DY, x + px - 1, y + py + LG_LAT_DY);
                for (; seg < px; seg++) LCD_WR_DATA_16Bit(color);
                in = 0;
            }
        }
        if (in) {
            LCD_Address_Set(x + seg, y + py + LG_LAT_DY, x + 15, y + py + LG_LAT_DY);
            for (; seg < 16; seg++) LCD_WR_DATA_16Bit(color);
        }
    }
}

/**
* @brief  绘制语言设置页内容("中文" / "English" 两行 + 选中标记), 用于局部刷新。
* @note   选中项用绿色文字 + 左侧绿色方块; 未选中用白色文字(无方块)。
*/
static void Language_DrawBody(void)
{
    /* 中文 = 中 文 (2 x 24 = 48宽, 居中: x 96~144); English = 7 x 16 = 112宽, 居中: x 64~176 */
    static const uint8_t cn_id[2] = { FLASH_STORE_ID_24_ZHONG, FLASH_STORE_ID_24_WEN };
    static const uint8_t en_id[7] = { FLASH_STORE_ID_16_E, FLASH_STORE_ID_16_N,
                                      FLASH_STORE_ID_16_G, FLASH_STORE_ID_16_L,
                                      FLASH_STORE_ID_16_I, FLASH_STORE_ID_16_S,
                                      FLASH_STORE_ID_16_H };
    uint8_t i;
    uint16_t cn_color = (g_language_index == 0) ? LCD_GREEN : LCD_WHITE;
    uint16_t en_color = (g_language_index == 1) ? LCD_GREEN : LCD_WHITE;

    LCD_CS_CLR;
    for (i = 0; i < 2; i++) {
        Lang_DrawCnChar((uint16_t)(96 + i * 24), LG_CN_Y, (FlashStoreId)cn_id[i], cn_color);
    }
    for (i = 0; i < 7; i++) {
        Lang_DrawLatChar((uint16_t)(64 + i * 16), LG_EN_Y, (FlashStoreId)en_id[i], en_color);
    }
    LCD_CS_SET;

    /* 选中标记方块(只画选中项) */
    if (g_language_index == 0) {
        LCD_Fill(LG_MARK_X, LG_CN_Y + 6, LG_MARK_X + LG_MARK_SZ, LG_CN_Y + 6 + LG_MARK_SZ, LCD_GREEN);
    } else {
        LCD_Fill(LG_MARK_X, LG_EN_Y + 6, LG_MARK_X + LG_MARK_SZ, LG_EN_Y + 6 + LG_MARK_SZ, LCD_GREEN);
    }
}

/**
* @brief  绘制语言设置整页: 清黑 + 返回键 + 内容。
*/
static void Language_Draw(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    DrawBack();
    Language_DrawBody();
}

/**
* @brief  处理语言设置页触摸: 点上半=中文, 下半=English, 改动即保存并重绘。
* @return 恒返回 1(页面活跃)。
* @note   仅 Y<200 的点击响应, 屏蔽下方 200~240 防划菜单误触(同息屏时间页)。
*/
static uint8_t Language_Update(void)
{
    TouchState ts;
    Touch_Scan(&ts);
    if (ts.pressed) {
        if (!s_lg_prev) {
            s_lg_prev = 1;
            s_lg_start_x = ts.x;
            s_lg_start_y = ts.y;
        }
    } else if (s_lg_prev) {
        s_lg_prev = 0;
        int16_t dx = (int16_t)ts.x - (int16_t)s_lg_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_lg_start_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= TAP_THRESHOLD && dy <= TAP_THRESHOLD && s_lg_start_y < 200) {
            uint8_t idx = (s_lg_start_y < LG_TAP_SPLIT) ? 0 : 1;
            if (idx != g_language_index) {
                g_language_index = idx;
                Settings_SaveAll();
            }
            Language_Draw();
        }
    }
    return 1;
}

/**
* @brief  退出设置: 复位 s_mode 与各页触摸状态。
* @note   不重绘画面, 由调用方(main.c)恢复场景。
*/
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
    s_lg_prev = 0;
    s_lg_start_x = 0;
    s_lg_start_y = 0;
}

/**
* @brief  统一设置入口: 按 s_mode 分派到对应页 Update。
* @return 1=设置页活跃; 0=不在设置状态(s_mode==NONE)。
*/
uint8_t Settings_Update(void)
{
    if (s_mode == SETTINGS_NONE) return 0;
    switch (s_mode) {
    case SETTINGS_BRIGHTNESS:  return Brightness_Update();
    case SETTINGS_SLEEP_TIME:  return SleepTime_Update();
    case SETTINGS_ORIENTATION: return Orientation_Update();
    case SETTINGS_LANGUAGE:    return Language_Update();
    default: s_mode = SETTINGS_NONE; return 0;
    }
}
