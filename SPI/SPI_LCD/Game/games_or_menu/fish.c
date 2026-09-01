/**
 * @file    fish.c
 * @brief   大鱼吃小鱼游戏模块
 *          触摸跟手控制大鱼移动, 小鱼随机游动;
 *          大鱼能吃掉比自己小的鱼并成长, 碰到比自己大的鱼则失败。
 *          鱼图片存于外部Flash, 运行时按大小DMA流式读取。
 */
#include "fish.h"
#include "fish_imgs.h"     /* 鱼图片尺寸/数据(fish_imgs_dim) */
#include "player_imgs.h"   /* 玩家形象尺寸/数据(player_imgs_dim) */
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
#define FISH_PLAYER_START_LEVEL  0   /* 玩家初始等级(第1条沙丁鱼) */
#define FISH_PLAYER_LEVEL_MAX    8   /* 玩家最大等级(第9张玩家形象, PLAYER_COUNT-1) */
#define FISH_PLAYER_X   (LCD_W / 2)
#define FISH_PLAYER_Y   (LCD_H / 2)

#define FISH_NUM_FOOD   6      /* 场上小鱼数量 */
#define FISH_SPEED_BASE 3      /* 小鱼基础移动速度 */
#define FISH_FOOD_RANGE 3      /* 自适应难度: 小鱼等级在玩家左右 ±3 内 */

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
 *  共用图片绘制: 按等级从外部Flash流式读取对应图, 跳过白色背景
 *  @param id_base  资源ID基址 (FLASH_STORE_ID_*_0)
 *  @param dims     尺寸表 (每等级 {w,h})
 *  @param count    等级数
 *  @param level    等级 0-(count-1)
 *  @param x,y      中心坐标
 * ================================================================ */
static void draw_image_sprite(FlashStoreId id_base, const uint16_t dims[][2],
                              uint8_t count, uint8_t level,
                              uint16_t x, uint16_t y)
{
    if (level >= count) return;
    uint16_t w = dims[level][0];
    uint16_t h = dims[level][1];
    FlashStoreId id = (FlashStoreId)(id_base + level);
    const uint8_t *img = FlashStore_ResLoad(id, 0, (uint16_t)(w * h * 2));
    if (!img) return;

    const uint16_t *px = (const uint16_t *)img;   /* 小端序强转 */
    /* 左上角(中心对齐) */
    int16_t x0 = (int16_t)(x) - (int16_t)(w / 2);
    int16_t y0 = (int16_t)(y) - (int16_t)(h / 2);

    uint16_t r, c;
    LCD_CS_CLR;
    for (r = 0; r < h; r++) {
        int16_t sy = y0 + (int16_t)r;
        if (sy < 0 || sy >= 240) continue;
        c = 0;
        while (c < w) {
            /* 跳过白色背景(透明) */
            while (c < w && px[r * w + c] == 0xFFFF) c++;
            if (c >= w) break;
            uint16_t start = c;
            /* 收集连续非白色段 */
            while (c < w && px[r * w + c] != 0xFFFF) c++;
            /* 段 [start, c), 屏幕X裁剪 */
            int16_t sx0 = x0 + (int16_t)start;
            int16_t sx1 = x0 + (int16_t)c - 1;
            if (sx1 < 0 || sx0 >= 240) continue;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > 239) sx1 = 239;
            if (sx1 < sx0) continue;
            LCD_Address_Set((uint16_t)sx0, (uint16_t)sy,
                            (uint16_t)sx1, (uint16_t)sy);
            for (uint16_t i = 0; i < (uint16_t)(c - start); i++) {
                LCD_WR_DATA_16Bit(px[r * w + start + i]);
            }
        }
    }
    LCD_CS_SET;
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
    /* 自适应难度: 小鱼等级在玩家 ±FISH_FOOD_RANGE 内随机, 并钳位到 [0,9] */
    {
        int16_t pl = (int16_t)g_player.level;
        int16_t lo = pl - FISH_FOOD_RANGE;
        int16_t hi = pl + FISH_FOOD_RANGE;
        if (lo < 0) lo = 0;
        if (hi > 9) hi = 9;
        uint8_t span = (uint8_t)(hi - lo + 1);
        e->level = (uint8_t)(lo + (int16_t)fish_rand(span));
    }
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
            draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                              FISH_COUNT, g_food[i].level,
                              g_food[i].x, g_food[i].y);
        }
    }
    draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                      PLAYER_COUNT, g_player.level,
                      g_player.x, g_player.y);

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