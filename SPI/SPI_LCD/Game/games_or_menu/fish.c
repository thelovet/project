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
#include "timer.h"         /* GetTimerCount: 基于真实时间的移动节拍(与帧率无关) */

/* ================================================================
 *  游戏参数
 * ================================================================ */
#define FISH_PLAYER_START_LEVEL  0   /* 玩家初始等级(第1条沙丁鱼) */
#define FISH_PLAYER_LEVEL_MAX    8   /* 玩家最大等级(第9张玩家形象, PLAYER_COUNT-1) */
#define FISH_PLAYER_X   (LCD_W / 2)
#define FISH_PLAYER_Y   (LCD_H / 2)

#define FISH_NUM_FOOD   6      /* 场上小鱼数量 */
#define FISH_SPEED_BASE 2      /* 小鱼基础移动速度(降慢, 便于捕捉) */
#define FISH_MOVE_US    40000 /* 小鱼每40ms移动一次(基于真实时间, 更连续) */
#define FISH_FOOD_RANGE 3      /* 自适应难度: 小鱼等级在玩家左右 ±3 内 */
#define FISH_INVINCIBLE 0      /* 无敌模式(1=碰到大鱼也不死, 直接吃掉; 0=正常玩法, 碰大鱼即失败) */

/* 顶部得分条区域(16x32数字从y=4到y=35), 实体不得越过, 避免游动时擦掉计分 */
#define SCORE_TOP      4
#define SCORE_H        32
#define PLAY_TOP      (SCORE_TOP + SCORE_H)     /* 36: 游戏区顶部边界 */
#define PLAY_Y_MIN    (PLAY_TOP + 20)           /* 最高实体半高(player=20), 防顶图越界 */
#define PLAY_Y_MAX    (LCD_H - 21)              /* 高度最大41(半高20)时底边仍完整 */
#define PLAY_X_MIN    26                        /* 宽度最大52(半宽26)时左右仍完整 */
#define PLAY_X_MAX    (LCD_W - 26)

/* ================================================================
 *  游戏状态
 * ================================================================ */
typedef struct {
    uint8_t active;      /* 是否在场 */
    uint8_t level;       /* 等级 0-9 */
    uint8_t dir;         /* 游动方向(0-7) */
    uint16_t x, y;       /* 当前位置 */
    int16_t vx, vy;      /* 速度分量 */
    int16_t prev_x, prev_y;  /* 上一帧位置(增量擦除用) */
    uint8_t prev_level;  /* 上一帧等级(尺寸变化后擦除旧图用) */
    uint8_t drawn;       /* 该实体已绘制过(用于只画变化) */
} FishEntity;

static uint8_t   g_state = FISH_STATE_PLAYING;
static FishEntity g_player;          /* 玩家大鱼 */
static FishEntity g_food[FISH_NUM_FOOD];  /* 小鱼 */
static uint16_t  g_score;            /* 得分 */
static uint16_t  g_last_score;       /* 上一次已绘制的得分(变化才重绘计分) */
static uint16_t  g_frame;            /* 帧计数器(用于动画/移动节拍) */
static uint32_t  g_food_us;          /* 上次小鱼移动的时间戳(微秒, 真实时间节拍) */
static uint8_t   g_full_redraw;      /* 全屏重绘脏标记 */
static uint8_t   g_prev_pressed;
static int16_t   s_last_touch_x;     /* 上一次有效触摸位置(增量拖动用) */
static int16_t   s_last_touch_y;

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

/* 前向声明: clear_entity_rect 内部需重绘相交实体 */
static void draw_image_sprite(FlashStoreId id_base, const uint16_t dims[][2],
                              uint8_t count, uint8_t level, uint16_t x, uint16_t y);

/* ================================================================
 *  擦除一个实体的矩形区域, 并立即重绘所有与该区域相交的其他实体。
 *  说明: 透明图增量绘制用背景色填满某实体旧矩形时, 会误伤与其重叠的
 *  其他实体像素; 因此填背景后立即按固定顺序重绘矩形内的所有实体, 防止残留。
 *  @param dims 尺寸表  @param level 等级  @param cx,cy 中心
 * ================================================================ */
static void clear_entity_rect(const uint16_t dims[][2], uint8_t level,
                              int16_t cx, int16_t cy)
{
    uint16_t w = dims[level][0];
    uint16_t h = dims[level][1];
    int16_t x0 = cx - (int16_t)(w / 2);
    int16_t y0 = cy - (int16_t)(h / 2);
    int16_t x1 = x0 + (int16_t)w - 1;
    int16_t y1 = y0 + (int16_t)h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < PLAY_TOP) y0 = PLAY_TOP;
    if (x1 > 239) x1 = 239;
    if (y1 > 239) y1 = 239;
    if (x1 < x0 || y1 < y0) return;
    /* 填背景 (LCD_Fill 的 end 不含坐标) */
    LCD_Fill((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 + 1), (uint16_t)(y1 + 1), COLOR_BG);
    /* 重绘所有与该矩形相交的实体(按当前坐标), 避免被背景误伤 */
    {
        uint16_t pw = player_imgs_dim[g_player.level][0];
        uint16_t ph = player_imgs_dim[g_player.level][1];
        int16_t px0 = (int16_t)g_player.x - (int16_t)(pw / 2);
        int16_t py0 = (int16_t)g_player.y - (int16_t)(ph / 2);
        int16_t px1 = px0 + (int16_t)pw - 1;
        int16_t py1 = py0 + (int16_t)ph - 1;
        if (px1 >= x0 && px0 <= x1 && py1 >= y0 && py0 <= y1)
            draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                              PLAYER_COUNT, g_player.level, g_player.x, g_player.y);
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            FishEntity *f = &g_food[i];
            if (!f->active) continue;
            uint16_t fw = fish_imgs_dim[f->level][0];
            uint16_t fh = fish_imgs_dim[f->level][1];
            int16_t fx0 = (int16_t)f->x - (int16_t)(fw / 2);
            int16_t fy0 = (int16_t)f->y - (int16_t)(fh / 2);
            int16_t fx1 = fx0 + (int16_t)fw - 1;
            int16_t fy1 = fy0 + (int16_t)fh - 1;
            if (fx1 >= x0 && fx0 <= x1 && fy1 >= y0 && fy0 <= y1)
                draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                                  FISH_COUNT, f->level, f->x, f->y);
        }
    }
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
    if (g_score == g_last_score) return;   /* 得分未变则不重绘(避免计分闪烁) */
    /* 清空顶部计分条区域 */
    LCD_Fill(0, SCORE_TOP, LCD_W, PLAY_TOP, COLOR_BG);
    uint8_t digits[8];
    uint8_t dc = uint_to_digits(g_score, digits);
    uint16_t total_w = dc * 16;
    uint16_t start_x = (240 - total_w) / 2;
    uint16_t y = SCORE_TOP;
    LCD_CS_CLR;
    uint8_t i;
    for (i = 0; i < dc; i++) {
        uint8_t d = digits[dc - 1 - i];
        sok_draw_digit_16x32(start_x + i * 16, y, d, LCD_WHITE);
    }
    LCD_CS_SET;
    g_last_score = g_score;
}

/* ================================================================
 *  绘制游戏结束画面: 中央"游戏结束"(32x32红字) + 下排"重新开始"提示(24x24)
 * ================================================================ */
static void draw_game_over(void)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, COLOR_BG);
    /* "游戏结束" 32x32 四字, 居中 */
    {
        uint16_t x = (240 - 4 * 32) / 2;
        uint16_t y = (240 - 32) / 2 - 14;
        LCD_CS_CLR;
        sok_draw_char_32(x,      y, FlashStore_ResLoad(FLASH_STORE_ID_32_YOU, 0, 128), LCD_RED);
        sok_draw_char_32(x + 32, y, FlashStore_ResLoad(FLASH_STORE_ID_32_XI3, 0, 128), LCD_RED);
        sok_draw_char_32(x + 64, y, FlashStore_ResLoad(FLASH_STORE_ID_32_JIE, 0, 128), LCD_RED);
        sok_draw_char_32(x + 96, y, FlashStore_ResLoad(FLASH_STORE_ID_32_SHU, 0, 128), LCD_RED);
        LCD_CS_SET;
    }
    /* "重新开始" 24x24 四字提示, 居左下排 */
    {
        uint16_t x = (240 - 4 * 24) / 2;
        uint16_t y = (240 - 32) / 2 + 22;
        LCD_CS_CLR;
        sok_draw_char_24(x,      y, FlashStore_ResLoad(FLASH_STORE_ID_24_CHONG, 0, 72), LCD_WHITE);
        sok_draw_char_24(x + 24, y, FlashStore_ResLoad(FLASH_STORE_ID_24_XIN,   0, 72), LCD_WHITE);
        sok_draw_char_24(x + 48, y, FlashStore_ResLoad(FLASH_STORE_ID_24_KAI,   0, 72), LCD_WHITE);
        sok_draw_char_24(x + 72, y, FlashStore_ResLoad(FLASH_STORE_ID_24_SHI3,  0, 72), LCD_WHITE);
        LCD_CS_SET;
    }
    draw_score();
}

/* ================================================================
 *  初始化一条小鱼 (随机位置/等级/方向)
 * ================================================================ */
static void food_respawn(FishEntity *e, uint8_t avoid_player)
{
    uint8_t was_drawn = e->drawn;
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
    e->x = (uint16_t)(PLAY_X_MIN + fish_rand(PLAY_X_MAX - PLAY_X_MIN + 1));
    e->y = (uint16_t)(PLAY_Y_MIN + fish_rand(PLAY_Y_MAX - PLAY_Y_MIN + 1));
    e->dir = (uint8_t)fish_rand(8);
    e->vx = (int16_t)fish_rand(FISH_SPEED_BASE * 2 + 1) - FISH_SPEED_BASE;
    e->vy = (int16_t)fish_rand(FISH_SPEED_BASE * 2 + 1) - FISH_SPEED_BASE;
    if (e->vx == 0 && e->vy == 0) e->vx = 1;   /* 保证至少一个方向有速度, 避免停死 */
    /* 先换好新位置再擦旧图: clear 用 prev(旧) 算旧矩形, 重绘相交实体用当前(已更新)坐标, 不会把旧位置又画回 */
    if (was_drawn)
        clear_entity_rect(fish_imgs_dim, e->prev_level, e->prev_x, e->prev_y);
    e->prev_x = (int16_t)e->x;
    e->prev_y = (int16_t)e->y;
    e->prev_level = e->level;
    e->drawn = 0;
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
    g_last_score = 0xFFFF;   /* 强制首帧重绘计分 */
    g_frame = 0;
    g_food_us = GetTimerCount();   /* 小鱼时间节拍基准 */
    g_full_redraw = 1;
    g_prev_pressed = 0;

    g_player.active = 1;
    g_player.level = FISH_PLAYER_START_LEVEL;
    g_player.x = FISH_PLAYER_X;
    g_player.y = FISH_PLAYER_Y;
    g_player.vx = 0;
    g_player.vy = 0;
    g_player.prev_x = FISH_PLAYER_X;
    g_player.prev_y = FISH_PLAYER_Y;
    g_player.prev_level = FISH_PLAYER_START_LEVEL;
    g_player.drawn = 0;

    for (i = 0; i < FISH_NUM_FOOD; i++)
        food_respawn(&g_food[i], 1);
}

/* ================================================================
 *  更新 (每帧调用)
 *  @return GAME_RET_* 返回值
 * ================================================================ */
uint8_t fish_update(void)
{
    /* 已结束: 显示结束画面, 点击任意位置重新开始 */
    if (g_state == FISH_STATE_OVER) {
        TouchState ts_over;
        Touch_Scan(&ts_over);
        if (ts_over.pressed) {
            fish_init();   /* 重新开始一局 */
        }
        return GAME_RET_INTERACT;
    }

    TouchState ts;
    Touch_Scan(&ts);
    g_frame++;

    uint8_t upress = ts.pressed;
    uint8_t just_pressed = (uint8_t)(upress && !g_prev_pressed);
    g_prev_pressed = upress;

    /* 增量拖动: 按住滑动时, 鱼按手指的位移增量平移(不吸附到手指位置, 松开即停) */
    if (upress) {
        if (just_pressed) {
            /* 刚按下: 记录起点, 本帧鱼不动 */
            s_last_touch_x = (int16_t)ts.x;
            s_last_touch_y = (int16_t)ts.y;
        } else {
            int16_t dx = (int16_t)ts.x - s_last_touch_x;
            int16_t dy = (int16_t)ts.y - s_last_touch_y;
            g_player.x = (uint16_t)((int16_t)g_player.x + dx);
            g_player.y = (uint16_t)((int16_t)g_player.y + dy);
            s_last_touch_x = (int16_t)ts.x;
            s_last_touch_y = (int16_t)ts.y;
        }
        /* 限制在游戏区内(不越入顶部计分条) */
        if (g_player.x < PLAY_X_MIN) g_player.x = PLAY_X_MIN;
        if (g_player.x > PLAY_X_MAX) g_player.x = PLAY_X_MAX;
        if (g_player.y < PLAY_Y_MIN) g_player.y = PLAY_Y_MIN;
        if (g_player.y > PLAY_Y_MAX) g_player.y = PLAY_Y_MAX;
    }

    /* 小鱼随机游动 (基于真实时间节拍, 与帧率无关, 防未按下时主循环空转导致狂飙) */
    {
        uint32_t now_us = GetTimerCount();
        if ((uint32_t)(now_us - g_food_us) >= FISH_MOVE_US) {
            g_food_us = now_us;
            uint8_t i;
            for (i = 0; i < FISH_NUM_FOOD; i++) {
                FishEntity *f = &g_food[i];
                if (!f->active) continue;
                f->x = (uint16_t)((int16_t)f->x + f->vx);
                f->y = (uint16_t)((int16_t)f->y + f->vy);
                if (f->x < PLAY_X_MIN) { f->x = PLAY_X_MIN; f->vx = (int16_t)-f->vx; }
                if (f->x > PLAY_X_MAX) { f->x = PLAY_X_MAX; f->vx = (int16_t)-f->vx; }
                if (f->y < PLAY_Y_MIN) { f->y = PLAY_Y_MIN; f->vy = (int16_t)-f->vy; }
                if (f->y > PLAY_Y_MAX) { f->y = PLAY_Y_MAX; f->vy = (int16_t)-f->vy; }
                /* 偶尔改变方向 */
                if (fish_rand(64) == 0) {
                    f->vx = (int16_t)fish_rand(FISH_SPEED_BASE*2+1) - FISH_SPEED_BASE;
                    f->vy = (int16_t)fish_rand(FISH_SPEED_BASE*2+1) - FISH_SPEED_BASE;
                    if (f->vx == 0 && f->vy == 0) f->vx = 1;   /* 避免停死 */
                }
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
                if (FISH_INVINCIBLE || f->level <= g_player.level) {
                    /* 吃掉(无敌模式下碰到任何大小都吃): 长大+得分, 小鱼重生 */
                    g_score += (uint16_t)(f->level + 1) * 10;
                    if (g_player.level < FISH_PLAYER_LEVEL_MAX)
                        g_player.level++;
                    food_respawn(f, 1);
                } else {
                    /* 碰到更大的鱼: 游戏结束 */
                    g_state = FISH_STATE_OVER;
                    draw_game_over();
                    return GAME_RET_INTERACT;
                }
            }
        }
    }

    /* ===== 增量绘制 (不整屏重绘, 只刷新有变化的鱼/玩家, 防闪烁) ===== */
    if (g_full_redraw) {
        /* 首次/恢复: 全屏填背景, 所有实体重画 */
        draw_background();
        g_full_redraw = 0;
        g_player.drawn = 0;
        uint8_t i0;
        for (i0 = 0; i0 < FISH_NUM_FOOD; i0++) g_food[i0].drawn = 0;
    }

    /* 玩家: 位置/等级变化 或 被重叠误伤标记时才重画 */
    {
        FishEntity *p = &g_player;
        uint8_t moved = (p->x != (uint16_t)p->prev_x ||
                         p->y != (uint16_t)p->prev_y ||
                         p->level != p->prev_level);
        if (!p->drawn || moved) {
            if (p->drawn && moved)
                clear_entity_rect(player_imgs_dim, p->prev_level,
                                  p->prev_x, p->prev_y);
            draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                              PLAYER_COUNT, p->level, p->x, p->y);
            p->prev_x = (int16_t)p->x;
            p->prev_y = (int16_t)p->y;
            p->prev_level = p->level;
            p->drawn = 1;
        }
    }

    /* 小鱼: 逻辑同玩家 */
    {
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            FishEntity *f = &g_food[i];
            if (!f->active) continue;
            uint8_t moved = (f->x != (uint16_t)f->prev_x ||
                             f->y != (uint16_t)f->prev_y ||
                             f->level != f->prev_level);
            if (!f->drawn || moved) {
                if (f->drawn && moved)
                    clear_entity_rect(fish_imgs_dim, f->prev_level,
                                      f->prev_x, f->prev_y);
                draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                                  FISH_COUNT, f->level, f->x, f->y);
                f->prev_x = (int16_t)f->x;
                f->prev_y = (int16_t)f->y;
                f->prev_level = f->level;
                f->drawn = 1;
            }
        }
    }

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