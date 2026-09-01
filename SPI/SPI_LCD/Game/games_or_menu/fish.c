/**
 * @file    fish.c
 * @brief   大鱼吃小鱼游戏模块
 *          触摸跟手控制大鱼移动, 小鱼随机游动;
 *          大鱼能吃掉比自己小的鱼并成长, 碰到比自己大的鱼则失败。
 *          鱼图片存于外部Flash, 运行时按大小DMA流式读取。
 */
#include "fish.h"
#include "tetris.h"        /* GAME_RET_* */
#include "menu.h"          /* 菜单/退出/重开 */
#include "sok_img_win.h"   /* sok_draw_digit_16x32 */
#include "flash_store.h"   /* 鱼图片资源从外部Flash读取 */
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "string.h"

/* ================================================================
 *  游戏参数
 * ================================================================ */
#define FISH_LEVELS     10   /* 鱼等级 0(最小) ~ 9(最大) */
#define FISH_IMG_SIZE   32   /* 每条鱼位图边长(正方形) */

#define FISH_PLAYER_START_LEVEL  2   /* 玩家初始等级 */
#define FISH_PLAYER_LEVEL_MAX    9   /* 玩家最大等级 */
#define FISH_PLAYER_X   (LCD_W / 2)
#define FISH_PLAYER_Y   (LCD_H / 2)

#define FISH_NUM_FOOD   6      /* 场上小鱼数量 */
#define FISH_SPEED_BASE 3      /* 小鱼基础移动速度 */

/* ================================================================
 *  游戏状态
 * ================================================================ */
typedef struct {
    uint8_t active;      /* 是否在场 */
    uint8_t level;       /* 等级 0-9 */
    uint8_t dir;         /* 游动方向(0-7) */
    uint16_t x, y;       /* 当前位置 */
    int16_t vx, vy;      /* 速度分量 */
} FishEntity;

static uint8_t   g_state = FISH_STATE_PLAYING;
static FishEntity g_player;          /* 玩家大鱼 */
static FishEntity g_food[FISH_NUM_FOOD];  /* 小鱼 */
static uint16_t  g_score;            /* 得分 */
static uint16_t  g_frame;            /* 帧计数器(用于动画/移动节拍) */
static uint8_t   g_full_redraw;      /* 全屏重绘脏标记 */
static uint16_t  g_touch_x, g_touch_y;   /* 当前触摸目标 */
static uint8_t   g_prev_pressed;

/* ================================================================
 *  颜色定义 (RGB565)
 * ================================================================ */
#define COLOR_BG     0x14A6   /* 深海蓝背景 */
#define COLOR_PLAYER 0x07E0   /* 玩家大鱼-鲜绿 */
#define COLOR_FOOD   0xFD20   /* 小鱼-橙黄 */
#define COL_RED      0xF800   /* 结束提示-红 */

/* ================================================================
 *  内部工具: 随机数 (复用 game_rand)
 * ================================================================ */
static uint16_t fish_rand(uint16_t n)
{
    if (n == 0) return 0;
    return (uint16_t)(game_rand() % n);
}

/* ================================================================
 *  绘制一条鱼 (程序化几何: 椭圆身 + 三角尾)
 *  @param x,y  中心坐标  @param size  鱼尺寸(边长)  @param color
 * ================================================================ */
static void draw_fish(uint16_t x, uint16_t y, uint8_t size, uint16_t color)
{
    uint16_t hl = (uint16_t)size / 2;   /* 半径 */
    uint16_t body_w = (uint16_t)size * 2 / 3;
    uint16_t body_h = (uint16_t)size / 2;
    uint16_t x0 = (x >= hl) ? (x - hl) : 0;
    uint16_t y0 = (y >= body_h) ? (y - body_h) : 0;
    uint16_t x1 = x + hl < LCD_W ? x + hl : LCD_W - 1;
    uint16_t y1 = y + body_h < LCD_H ? y + body_h : LCD_H - 1;

    /* 鱼身(椭圆近似: 一个矩形) */
    if (x1 > x0 && y1 > y0)
        LCD_Fill(x0, y0, x1, y1, color);
    /* 尾鳍(左侧三角, 缩小) */
    {
        uint16_t tx0 = (x > body_w) ? (x - body_w - (uint16_t)size / 4) : 0;
        uint16_t ty0 = (y >= 2) ? (y - 2) : 0;
        uint16_t tx1 = (x > body_w) ? (x - body_w) : 0;
        uint16_t ty1 = (y + 2 < LCD_H) ? (y + 2) : LCD_H - 1;
        if (tx1 > tx0 && ty1 > ty0)
            LCD_Fill(tx0, ty0, tx1, ty1, color);
    }
    /* 眼睛(右上一, 背景色点睛) */
    {
        uint16_t ex = (x + body_w / 2 - 2 < LCD_W) ? (x + body_w / 2 - 2) : LCD_W - 1;
        uint16_t ey = (y >= body_h / 2) ? (y - body_h / 2) : 0;
        LCD_Fill(ex, ey, ex, ey, COLOR_BG);
    }
}

/* ================================================================
 *  清屏背景
 * ================================================================ */
static void draw_background(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, COLOR_BG);
}

/* ================================================================
 *  绘制得分 (右上角, 16x32数字)
 * ================================================================ */
static void draw_score(void)
{
    uint8_t digits[8];
    uint8_t dc = uint_to_digits(g_score, digits);
    uint16_t total_w = dc * 16;
    uint16_t start_x = (240 - total_w) / 2;
    uint16_t y = 4;
    LCD_CS_CLR;
    uint8_t i;
    for (i = 0; i < dc; i++) {
        uint8_t d = digits[dc - 1 - i];
        sok_draw_digit_16x32(start_x + i * 16, y, d, LCD_WHITE);
    }
    LCD_CS_SET;
}

/* ================================================================
 *  绘制游戏结束画面
 * ================================================================ */
static void draw_game_over(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, COLOR_BG);
    /* 中央画一个大红叉表示失败 */
    LCD_Fill(90, 100, 150, 140, COLOR_BG);
    LCD_Fill(96, 106, 98,  134, COL_RED);
    LCD_Fill(144, 106, 146, 134, COL_RED);
    LCD_Fill(112, 100, 128, 140, COL_RED);
    draw_score();
}

/* ================================================================
 *  初始化一条小鱼 (随机位置/等级/方向)
 * ================================================================ */
static void food_respawn(FishEntity *e, uint8_t avoid_player)
{
    e->active = 1;
    e->level = (uint8_t)fish_rand(FISH_PLAYER_LEVEL_MAX - 1);  /* 0..8 小鱼 */
    e->x = (uint16_t)fish_rand(LCD_W);
    e->y = (uint16_t)fish_rand(LCD_H);
    e->dir = (uint8_t)fish_rand(8);
    e->vx = (int16_t)fish_rand(FISH_SPEED_BASE * 2 + 1) - FISH_SPEED_BASE;
    e->vy = (int16_t)fish_rand(FISH_SPEED_BASE * 2 + 1) - FISH_SPEED_BASE;
    (void)avoid_player;
}

/* ================================================================
 *  初始化
 * ================================================================ */
void fish_init(void)
{
    uint8_t i;
    g_state = FISH_STATE_PLAYING;
    g_score = 0;
    g_frame = 0;
    g_full_redraw = 1;
    g_prev_pressed = 0;
    g_touch_x = FISH_PLAYER_X;
    g_touch_y = FISH_PLAYER_Y;

    g_player.active = 1;
    g_player.level = FISH_PLAYER_START_LEVEL;
    g_player.x = FISH_PLAYER_X;
    g_player.y = FISH_PLAYER_Y;
    g_player.vx = 0;
    g_player.vy = 0;

    for (i = 0; i < FISH_NUM_FOOD; i++)
        food_respawn(&g_food[i], 1);
}

/* ================================================================
 *  更新 (每帧调用)
 *  @return GAME_RET_* 返回值
 * ================================================================ */
uint8_t fish_update(void)
{
    TouchState ts;
    Touch_Scan(&ts);
    g_frame++;

    /* 触摸跟手目标 */
    if (ts.pressed) {
        g_touch_x = ts.x;
        g_touch_y = ts.y;
    }
    g_prev_pressed = ts.pressed;

    /* 玩家移动到触摸位置(平滑跟手, 每帧步进) */
    {
        int16_t dx = (int16_t)g_touch_x - (int16_t)g_player.x;
        int16_t dy = (int16_t)g_touch_y - (int16_t)g_player.y;
        int16_t step = 6;
        if (dx < -step) dx = -step;
        if (dx >  step) dx =  step;
        if (dy < -step) dy = -step;
        if (dy >  step) dy =  step;
        g_player.x = (uint16_t)((int16_t)g_player.x + dx);
        g_player.y = (uint16_t)((int16_t)g_player.y + dy);
    }

    /* 小鱼随机游动 */
    if ((g_frame & 3) == 0) {
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            FishEntity *f = &g_food[i];
            if (!f->active) continue;
            f->x = (uint16_t)((int16_t)f->x + f->vx);
            f->y = (uint16_t)((int16_t)f->y + f->vy);
            if (f->x < 4 || f->x > LCD_W - 4) f->vx = (int16_t)-f->vx;
            if (f->y < 4 || f->y > LCD_H - 4) f->vy = (int16_t)-f->vy;
            /* 偶尔改变方向 */
            if (fish_rand(64) == 0) {
                f->vx = (int16_t)fish_rand(FISH_SPEED_BASE*2+1) - FISH_SPEED_BASE;
                f->vy = (int16_t)fish_rand(FISH_SPEED_BASE*2+1) - FISH_SPEED_BASE;
            }
        }
    }

    /* 碰撞检测: 玩家 vs 小鱼 */
    {
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            FishEntity *f = &g_food[i];
            if (!f->active) continue;
            int16_t ddx = (int16_t)f->x - (int16_t)g_player.x;
            int16_t ddy = (int16_t)f->y - (int16_t)g_player.y;
            if (ddx < 0) ddx = (int16_t)-ddx;
            if (ddy < 0) ddy = (int16_t)-ddy;
            /* 粗略判定: 玩家尺寸随等级增大 */
            uint16_t pr = (uint16_t)(8 + g_player.level * 4);
            if (ddx < (int16_t)pr && ddy < (int16_t)pr) {
                if (f->level < g_player.level) {
                    /* 吃掉小鱼: 长大+得分, 小鱼重生 */
                    g_score += (uint16_t)(f->level + 1) * 10;
                    if (g_player.level < FISH_PLAYER_LEVEL_MAX)
                        g_player.level++;
                    food_respawn(f, 1);
                } else if (f->level == g_player.level) {
                    /* 同级: 视作未能吃掉, 无反应 */
                } else {
                    /* 碰到更大的鱼: 游戏结束 */
                    g_state = FISH_STATE_OVER;
                    draw_game_over();
                    return GAME_RET_INTERACT;
                }
            }
        }
    }

    /* ===== 绘制 ===== */
    draw_background();
    {
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            if (!g_food[i].active) continue;
            draw_fish(g_food[i].x, g_food[i].y,
                      (uint8_t)(10 + g_food[i].level * 4), COLOR_FOOD);
        }
    }
    draw_fish(g_player.x, g_player.y,
              (uint8_t)(16 + g_player.level * 4), COLOR_PLAYER);

    /* 得分显示 */
    draw_score();

    return GAME_RET_NONE;
}

/* ================================================================
 *  触摸复位
 * ================================================================ */
void fish_reset_touch(void)
{
    g_prev_pressed = 0;
}

/* ================================================================
 *  全屏重绘
 * ================================================================ */
void fish_full_redraw(void)
{
    g_full_redraw = 1;
}

/* ================================================================
 *  菜单可见
 * ================================================================ */
uint8_t fish_is_menu_visible(void)
{
    return (g_state == FISH_STATE_EXIT_MENU || g_state == FISH_STATE_RESTART_MENU);
}

/* ================================================================
 *  存档 / 恢复 (最小实现: 存玩家等级/得分/小鱼)
 * ================================================================ */
uint8_t fish_save_state(uint8_t *buf, uint8_t max_len)
{
    if (!buf || max_len < 4) return 0;
    buf[0] = g_player.level;
    buf[1] = (uint8_t)(g_score >> 8);
    buf[2] = (uint8_t)(g_score & 0xFF);
    buf[3] = g_state;
    return 4;
}

uint8_t fish_restore_state(const uint8_t *buf, uint8_t len)
{
    if (!buf || len < 4) return 0;
    g_player.level = buf[0];
    g_score = (uint16_t)((uint16_t)buf[1] << 8) | buf[2];
    g_state = (buf[3] == FISH_STATE_OVER) ? FISH_STATE_OVER : FISH_STATE_PLAYING;
    return 1;
}