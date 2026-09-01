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
#include "timer.h"
/* ================================================================
 *  菜单: 退出菜单映射 + 行类型 (复用 menu.c 统一菜单系统)
 * ================================================================ */
/* 下划菜单游戏映射 (视频/推箱子/2048/华容道, 减当前=大鱼吃小鱼) */
static const uint8_t fish_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_SOKOBAN, EXIT_RET_D2048, EXIT_RET_TETRIS, EXIT_RET_KLOTSKI
};
/* 退出菜单行类型 (供 menu_draw_nrow_frame_only / game_menu_drag_poll 使用) */
static const uint8_t fish_row_types[6] = {
    MENU_ROW_SHIPIN, MENU_ROW_TUI, MENU_ROW_2048, MENU_ROW_TETRIS, MENU_ROW_KLOTSKI, MENU_ROW_FANHUI
};         /* GetTimerCount: 基于真实时间的移动节拍(与帧率无关) */

/* ================================================================
 *  游戏参数
 * ================================================================ */
#define FISH_PLAYER_START_LEVEL  0   /* 玩家物理初始等级0(第1条沙丁鱼); HUD显示为 等级+1 */
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
/* 顶部信息栏数字大小为8x14, 信息栏占 y4~31; PLAY_TOP=32 让鱼顶贴信息栏下缘 */
#define PLAY_TOP       32   /* 游戏区顶部边界(信息栏底31, 留1px) */
#define PLAY_Y_MIN    (PLAY_TOP + 20)           /* 最高实体半高(player=20), 防顶图越界 */
#define PLAY_Y_MAX    (LCD_H - 21)              /* 高度最大41(半高20)时底边仍完整 */
#define PLAY_X_MIN    26                        /* 宽度最大52(半宽26)时左右仍完整 */
#define PLAY_X_MAX    (LCD_W - 26)
#define PLAY_CORNER   30   /* 8角形倒角边长: 切掉矩形四角, 让鱼不进入圆形屏被遮挡的四角 */

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
static uint32_t  g_score;            /* 累积分(吃鱼得分 = 5^等级: 0级=1,1级=5,2级=25...) */
static uint8_t   g_hud_dirty;        /* 顶部信息(分数/进度)需重绘 */
static uint16_t  g_frame;            /* 帧计数器(用于动画/移动节拍) */
static uint32_t  g_food_us;          /* 上次小鱼移动的时间戳(微秒, 真实时间节拍) */
static uint8_t   g_full_redraw;      /* 全屏重绘脏标记 */
static uint8_t   g_prev_pressed;
static int16_t   s_last_touch_x;     /* 上一次有效触摸位置(增量拖动用) */
static int16_t   s_last_touch_y;
static uint16_t g_touch_start_x;

/* ================================================================
 *  颜色定义 (RGB565)
 * ================================================================ */
#define COLOR_BG     0x14A6   /* 深海蓝背景 */
#define COL_RED      0xF800   /* 结束提示-红 */
/* 判断像素是否视为"透明": 接近白色(含纯白)也不显示, 避免边缘残留白边 */
#define IS_TRANSPARENT(p)  (((((p) >> 11) & 0x1F) >= 0x1C) && \
                            (((((p) >> 5) & 0x3F)) >= 0x38) && \
                            (((p) & 0x1F) >= 0x1C))

/* ================================================================
 *  内部工具: 随机数 (复用 game_rand)
 * ================================================================ */
static uint16_t fish_rand(uint16_t n)
{
    if (n == 0) return 0;
    return (uint16_t)(game_rand() % n);
}

/* ================================================================
 *  将鱼/玩家中心点钳制到"8角形"可游动区(矩形四角倒角, 贴合圆形屏).
 *  倒角后中心点不再进入圆形屏被遮挡/裁切的四角。
 *  @param px,py 指针: 入=当前位置, 出=钳制后位置
 * ================================================================ */
static void fish_clamp_pos(int16_t *px, int16_t *py)
{
    int16_t x = *px, y = *py;
    /* 矩形钳位 */
    if (x < PLAY_X_MIN) x = PLAY_X_MIN;
    if (x > PLAY_X_MAX) x = PLAY_X_MAX;
    if (y < PLAY_Y_MIN) y = PLAY_Y_MIN;
    if (y > PLAY_Y_MAX) y = PLAY_Y_MAX;
    /* 四角倒角: 角部(离两邻边都 < PLAY_CORNER)时, 把中心点推回45°斜边内 */
    if (x < PLAY_X_MIN + PLAY_CORNER && y < PLAY_Y_MIN + PLAY_CORNER) {
        if ((x - PLAY_X_MIN) + (y - PLAY_Y_MIN) < PLAY_CORNER)
            x = PLAY_X_MIN + (int16_t)(PLAY_CORNER - (y - PLAY_Y_MIN));
    }
    if (x > PLAY_X_MAX - PLAY_CORNER && y < PLAY_Y_MIN + PLAY_CORNER) {
        if ((PLAY_X_MAX - x) + (y - PLAY_Y_MIN) < PLAY_CORNER)
            x = PLAY_X_MAX - (int16_t)(PLAY_CORNER - (y - PLAY_Y_MIN));
    }
    if (x < PLAY_X_MIN + PLAY_CORNER && y > PLAY_Y_MAX - PLAY_CORNER) {
        if ((x - PLAY_X_MIN) + (PLAY_Y_MAX - y) < PLAY_CORNER)
            x = PLAY_X_MIN + (int16_t)(PLAY_CORNER - (PLAY_Y_MAX - y));
    }
    if (x > PLAY_X_MAX - PLAY_CORNER && y > PLAY_Y_MAX - PLAY_CORNER) {
        if ((PLAY_X_MAX - x) + (PLAY_Y_MAX - y) < PLAY_CORNER)
            x = PLAY_X_MAX - (int16_t)(PLAY_CORNER - (PLAY_Y_MAX - y));
    }
    *px = x;
    *py = y;
}

/* ================================================================
 *  小鱼角落反弹: 在8角形的斜边处, 若速度仍朝外, 把速度反转向内
 *  并贴回斜边, 避免鱼卡在角落要等随机变向才动。
 * ================================================================ */
static void fish_bounce_corners(FishEntity *f)
{
    int16_t X = (int16_t)f->x, Y = (int16_t)f->y;
    int16_t dxl = X - PLAY_X_MIN, dxr = PLAY_X_MAX - X;
    int16_t dyt = Y - PLAY_Y_MIN, dyb = PLAY_Y_MAX - Y;
    if (dxl < PLAY_CORNER && dyt < PLAY_CORNER && f->vx <= 0 && f->vy <= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MIN + (int16_t)(PLAY_CORNER - dyt);
    } else if (dxr < PLAY_CORNER && dyt < PLAY_CORNER && f->vx >= 0 && f->vy <= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MAX - (int16_t)(PLAY_CORNER - dyt);
    } else if (dxl < PLAY_CORNER && dyb < PLAY_CORNER && f->vx <= 0 && f->vy >= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MIN + (int16_t)(PLAY_CORNER - dyb);
    } else if (dxr < PLAY_CORNER && dyb < PLAY_CORNER && f->vx >= 0 && f->vy >= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MAX - (int16_t)(PLAY_CORNER - dyb);
    }
    f->x = (uint16_t)X;
    f->y = (uint16_t)Y;
}

/* ================================================================
 *  升级/得分阈值: 返回 5^lv (0级=1, 1级=5, 2级=25...). lv<=9 安全(uint32)
 * ================================================================ */
static uint32_t fish_thresh(uint8_t lv)
{
    uint32_t v = 1;
    uint8_t i;
    for (i = 0; i < lv; i++) v *= 5;
    return v;
}

/* ================================================================
 *  把 uint32 分数转成各位数字; 返回位数, digits[0]=个位 ... digits[n-1]=最高位
 * ================================================================ */
static uint8_t fish_score_digits(uint32_t v, uint8_t *digits)
{
    uint8_t n = 0;
    do {
        if (n >= 8) break;
        digits[n++] = (uint8_t)(v % 10);
        v /= 10;
    } while (v > 0);
    return n;
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
            /* 跳过接近白色背景(透明) */
            while (c < w && IS_TRANSPARENT(px[r * w + c])) c++;
            if (c >= w) break;
            uint16_t start = c;
            /* 收集连续非透明(非白)段 */
            while (c < w && !IS_TRANSPARENT(px[r * w + c])) c++;
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
 *  小鱼移动的行级原子重绘: 对"旧∪新"区域逐行 "先写该行背景、再写该行新鱼像素",
 *  避免整块背景填充造成的瞬间闪烁; 之后重绘区域内其它相交实体(防误伤)。
 * ================================================================ */
static void fish_move_redraw(const FishEntity *f)
{
    uint16_t w = fish_imgs_dim[f->level][0];
    uint16_t h = fish_imgs_dim[f->level][1];
    uint16_t ow = fish_imgs_dim[f->prev_level][0];
    uint16_t oh = fish_imgs_dim[f->prev_level][1];
    int16_t nx0 = (int16_t)f->x - (int16_t)(w / 2);
    int16_t ny0 = (int16_t)f->y - (int16_t)(h / 2);
    int16_t nx1 = nx0 + (int16_t)w - 1;
    int16_t ny1 = ny0 + (int16_t)h - 1;
    int16_t ox0 = (int16_t)f->prev_x - (int16_t)(ow / 2);
    int16_t oy0 = (int16_t)f->prev_y - (int16_t)(oh / 2);
    int16_t ox1 = ox0 + (int16_t)ow - 1;
    int16_t oy1 = oy0 + (int16_t)oh - 1;
    int16_t rx0 = (nx0 < ox0) ? nx0 : ox0;
    int16_t ry0 = (ny0 < oy0) ? ny0 : oy0;
    int16_t rx1 = (nx1 > ox1) ? nx1 : ox1;
    int16_t ry1 = (ny1 > oy1) ? ny1 : oy1;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < PLAY_TOP) ry0 = PLAY_TOP;
    if (rx1 > 239) rx1 = 239;
    if (ry1 > 239) ry1 = 239;
    if (rx1 < rx0 || ry1 < ry0) return;

    const uint8_t *img = FlashStore_ResLoad((FlashStoreId)(FLASH_STORE_ID_FISH_0 + f->level),
                                            0, (uint16_t)(w * h * 2));
    if (!img) return;
    const uint16_t *px = (const uint16_t *)img;

    LCD_CS_CLR;
    for (int16_t y = ry0; y <= ry1; y++) {
        /* 该行先写背景(不跨越行, 撕裂窗口收缩到单行, 减闪) */
        LCD_Address_Set((uint16_t)rx0, (uint16_t)y, (uint16_t)rx1, (uint16_t)y);
        for (int16_t x = rx0; x <= rx1; x++) LCD_WR_DATA_16Bit(COLOR_BG);
        /* 该行新鱼的非白段(覆盖背景) */
        int16_t fy = y - ny0;
        if (fy >= 0 && fy < (int16_t)h) {
            int16_t c = 0;
            while (c < (int16_t)w) {
                    while (c < (int16_t)w && IS_TRANSPARENT(px[fy * w + c])) c++;
                    if (c >= (int16_t)w) break;
                    int16_t start = c;
                    while (c < (int16_t)w && !IS_TRANSPARENT(px[fy * w + c])) c++;
                int16_t sx0 = nx0 + start;
                int16_t sx1 = nx0 + (int16_t)c - 1;
                if (sx1 < rx0 || sx0 > rx1) continue;
                if (sx0 < rx0) sx0 = rx0;
                if (sx1 > rx1) sx1 = rx1;
                if (sx1 < sx0) continue;
                LCD_Address_Set((uint16_t)sx0, (uint16_t)y, (uint16_t)sx1, (uint16_t)y);
                for (uint16_t i = (uint16_t)(sx0 - nx0); i <= (uint16_t)(sx1 - nx0); i++)
                    LCD_WR_DATA_16Bit(px[fy * w + i]);
            }
        }
    }
    LCD_CS_SET;

    /* 重绘与区域相交的其它实体(当前坐标), 避免行背景误伤它们 */
    {
        uint16_t pw = player_imgs_dim[g_player.level][0];
        uint16_t ph = player_imgs_dim[g_player.level][1];
        int16_t px0 = (int16_t)g_player.x - (int16_t)(pw / 2);
        int16_t py0 = (int16_t)g_player.y - (int16_t)(ph / 2);
        int16_t px1 = px0 + (int16_t)pw - 1;
        int16_t py1 = py0 + (int16_t)ph - 1;
        if (px1 >= rx0 && px0 <= rx1 && py1 >= ry0 && py0 <= ry1)
            draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                              PLAYER_COUNT, g_player.level, g_player.x, g_player.y);
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            const FishEntity *of = &g_food[i];
            if (of == f || !of->active) continue;
            uint16_t fw = fish_imgs_dim[of->level][0];
            uint16_t fh = fish_imgs_dim[of->level][1];
            int16_t fx0 = (int16_t)of->x - (int16_t)(fw / 2);
            int16_t fy0 = (int16_t)of->y - (int16_t)(fh / 2);
            int16_t fx1 = fx0 + (int16_t)fw - 1;
            int16_t fy1 = fy0 + (int16_t)fh - 1;
            if (fx1 >= rx0 && fx0 <= rx1 && fy1 >= ry0 && fy0 <= ry1)
                draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                                  FISH_COUNT, of->level, of->x, of->y);
        }
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
 *  绘制顶部信息栏: 左=当前分数, 右=升级进度条(10格, 按分数在当前级的占比)
 * ================================================================ */
static void draw_hud(void)
{
    if (!g_hud_dirty) return;
    g_hud_dirty = 0;
    /* 清空顶部信息区 */
    LCD_Fill(0, SCORE_TOP, LCD_W, PLAY_TOP, COLOR_BG);
    {
        uint8_t digits[8];
        uint8_t dc = fish_score_digits(g_score, digits);   /* 低位在前 */
        uint16_t score_w = (uint16_t)dc * 8;
        uint16_t prog_w = 10 * 4 + 9 * 1;         /* 进度条总宽49 */
        uint16_t pl_total = prog_w + 2 + 7;       /* 进度+间隔+等级 */
        LCD_CS_CLR;
        /* 行1: 当前分数(居中, 黄色 7x11) */
        {
            uint16_t sx = (240 - score_w) / 2;
            uint8_t i;
            for (i = 0; i < dc; i++) {
                uint8_t d = digits[dc - 1 - i];
                sok_draw_digit_scaled(sx + i * 8, 4, d, LCD_YELLOW, 8, 14);
            }
        }
        /* 行2: 升级进度条 + 当前等级(居中, 等级紧贴进度条右侧) */
        {
            uint16_t px = (240 - pl_total) / 2;
            uint16_t py = 18;
            uint8_t j;
            uint32_t lo = (g_player.level > 0) ? fish_thresh(g_player.level) : 0;
            uint32_t hi = fish_thresh((uint8_t)(g_player.level + 1));
            uint32_t seg = hi - lo;
            uint8_t fill = 0;
            if (seg > 0) {
                uint32_t cur = (g_score >= lo) ? (g_score - lo) : 0;
                fill = (uint8_t)((cur * 10) / seg);
                if (fill > 10) fill = 10;
            }
            for (j = 0; j < 10; j++) {
                uint16_t col = (j < fill) ? 0xFFE0 : 0x1E69;
                LCD_Fill(px, py, (uint16_t)(px + 4), (uint16_t)(py + 13), col);
                px += 4 + 1;
            }
            px += 2;
            LCD_CS_CLR;   /* 进度条 LCD_Fill 内部会切 CS, 画等级前须重新拉低片选, 否则写不进 */
            sok_draw_digit_scaled(px, py, (uint8_t)(g_player.level + 1), LCD_WHITE, 8, 14);
        }
        LCD_CS_SET;
    }
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
    g_hud_dirty = 1;
    draw_hud();
}

/* ================================================================
 *  初始化一条小鱼 (随机位置/等级/方向)
 * ================================================================ */
static void food_respawn(FishEntity *e, uint8_t avoid_player)
{
    uint8_t was_drawn = e->drawn;
    e->active = 1;
    /* 大多小鱼 <=玩家级别(可吃, 供成长), 少数更大(挑战); 避免开局无鱼可吃 */
    if (fish_rand(4) != 0) {
        /* 75%: 等级 0..玩家级别 (含同级, 可吃) */
        e->level = (uint8_t)fish_rand((uint16_t)(g_player.level + 1));
    } else {
        /* 25%: 玩家级别+1 .. +2 (更大, 碰撞失败) */
        int16_t lo = (int16_t)g_player.level + 1;
        int16_t hi = (int16_t)g_player.level + 2;
        if (hi > 9) hi = 9;
        if (lo > 9) lo = 9;
        if (hi < lo) hi = lo;
        e->level = (uint8_t)(lo + (int16_t)fish_rand((uint16_t)(hi - lo + 1)));
    }
    if (e->level > 9) e->level = 9;
    e->x = (uint16_t)(PLAY_X_MIN + fish_rand(PLAY_X_MAX - PLAY_X_MIN + 1));
    e->y = (uint16_t)(PLAY_Y_MIN + fish_rand(PLAY_Y_MAX - PLAY_Y_MIN + 1));
    /* 生成点也限制在8角形内(不进圆形屏四角) */
    {
        int16_t cx = (int16_t)e->x;
        int16_t cy = (int16_t)e->y;
        fish_clamp_pos(&cx, &cy);
        e->x = (uint16_t)cx;
        e->y = (uint16_t)cy;
    }
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
/* Full redraw of the current fish scene (background + player + all food + HUD).
   Used to restore the scene behind a menu after a menu-switch cleared the area to
   black (mirrors tetris redrawing its board). Game-over screen uses draw_game_over. */
static void fish_draw_scene(void)
{
    uint8_t i;
    draw_background();
    g_full_redraw = 0;
    draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                      PLAYER_COUNT, g_player.level, g_player.x, g_player.y);
    g_player.prev_x = (int16_t)g_player.x;
    g_player.prev_y = (int16_t)g_player.y;
    g_player.prev_level = g_player.level;
    g_player.drawn = 1;
    for (i = 0; i < FISH_NUM_FOOD; i++) {
        FishEntity *f = &g_food[i];
        if (!f->active) continue;
        draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                          FISH_COUNT, f->level, f->x, f->y);
        f->prev_x = (int16_t)f->x;
        f->prev_y = (int16_t)f->y;
        f->prev_level = f->level;
        f->drawn = 1;
    }
    g_hud_dirty = 1;
    draw_hud();
}

/* Restore the background right after a menu clear (same frame) to avoid a
   one-frame black flash on menu switch/cancel (others redraw the board fast).*/
static void fish_restore_scene_behind(void)
{
    if (g_state_before_menu == FISH_STATE_OVER) draw_game_over();
    else fish_draw_scene();
    g_full_redraw = 0;
}

void fish_init(void)
{
    uint8_t i;
    g_state = FISH_STATE_PLAYING;
    g_score = 0;
    g_menu_clear_color = COLOR_BG;   /* 清菜单位置用背景色替代黑色, 避免切菜单黑闪 */
    g_hud_dirty = 1;   /* 首帧重绘顶部信息 */
    g_frame = 0;
    g_food_us = GetTimerCount();   /* 小鱼时间节拍基准 */
    g_full_redraw = 1;
    g_prev_pressed = 0;
    g_touch_down = 0;
    g_menu_cooldown = 0;
    g_exit_menu_visible = 0;
    g_restart_menu_visible = 0;
    g_restart_sliding = 0;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
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
    uint8_t interacted = 0;

    /* 菜单冷却 */
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 设置页面下滑→退出菜单 */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        g_state = FISH_STATE_EXIT_MENU;
        g_full_redraw = 1;
    }

    /* ---- 退出菜单 ---- */
    if (g_state == FISH_STATE_EXIT_MENU) {
        /* 从结束画面进入菜单时, 需重绘结束画面作背景 */
        if (g_full_redraw) {
            if (g_state_before_menu == FISH_STATE_OVER) draw_game_over();
            else fish_draw_scene();
            g_full_redraw = 0;
        }
        if (!g_exit_dragging && !g_exit_sliding) {
            menu_draw_nrow_frame_only(g_exit_drag_y, fish_row_types, 6);
        }
        /* 上划打断: 切换到重新开始菜单 */
        {
            uint8_t r = menu_swipe_to_restart_handler(220, 10);
            if (r) {
                fish_restore_scene_behind();
                g_state = FISH_STATE_RESTART_MENU;
                return r - 1;
            }
        }
        /* 拖拽 */
        if (g_exit_dragging) {
            TouchState ts_d;
            Touch_Scan(&ts_d);
            uint8_t poll_ret = game_menu_drag_poll(&g_exit_drag_y, &g_exit_drag_start_y,
                                                    &g_exit_dragging, &ts_d, fish_row_types);
            if (poll_ret == 1) {
                g_exit_sliding = 1;
            } else if (poll_ret == 2) {
                menu_clear_exit_area_to(g_exit_drag_y);
                g_exit_menu_visible = 0;
                g_state = g_state_before_menu;
                fish_restore_scene_behind();
                g_touch_down = 0;
                g_menu_cooldown = 15;
            }
            return 0;
        }
        /* 滑入动画 */
        if (g_exit_sliding) {
            g_exit_drag_y += MENU_SLIDE_SPEED;
            if (g_exit_drag_y >= MENU_Y) {
                g_exit_drag_y = MENU_Y;
                g_exit_sliding = 0;
                g_touch_down = 0;
            }
            menu_draw_nrow_frame_only(g_exit_drag_y, fish_row_types, 6);
            return 0;
        }
        /* 点选退出菜单 */
        {
            TouchState ts;
            uint8_t ret = game_exit_menu_poll(fish_exit_map, &g_prev_pressed, &ts);
            if (ret == EXIT_RET_BACK) {
                menu_exit_back_cleanup();
                g_state = g_state_before_menu;
                fish_restore_scene_behind();
                return 0;
            }
            if (ret != EXIT_RET_NONE) {
                g_exit_menu_visible = 0;
                g_state = FISH_STATE_PLAYING;
                return ret;
            }
            return 0;
        }
    }

    /* ---- 重开菜单 ---- */
    if (g_state == FISH_STATE_RESTART_MENU) {
        if (g_full_redraw) {
            if (g_state_before_menu == FISH_STATE_OVER) draw_game_over();
            else fish_draw_scene();
            g_full_redraw = 0;
        }
        if (!g_restart_dragging && !g_restart_sliding) {
            menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
        }
        TouchState ts;
        if (g_restart_dragging) {
            Touch_Scan(&ts);
            uint8_t poll_ret = game_restart_menu_drag_poll(&g_restart_slide_y, g_touch_start_y,
                                                            &g_restart_dragging, &ts);
            if (poll_ret == 1) {
                g_restart_sliding = 1;
                g_prev_pressed = 0;
            } else if (poll_ret == 2) {
                goto restart_menu_close;
            } else if (poll_ret == 3) {
                goto restart_menu_to_exit;
            }
            return 0;
        }
        if (g_restart_sliding) {
            g_restart_slide_y -= RESTART_MENU_SLIDE_SPEED;
            if (g_restart_slide_y <= RESTART_MENU_Y) {
                g_restart_slide_y = RESTART_MENU_Y;
                g_restart_sliding = 0;
                g_prev_pressed = 0;
            }
            menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
            {
                TouchState ts_d;
                Touch_Scan(&ts_d);
                if (game_restart_menu_down_poll(&ts_d, &g_touch_down,
                                                &g_touch_start_x, &g_touch_start_y))
                    goto restart_menu_to_exit;
            }
            return 0;
        }
        Touch_Scan(&ts);
        if (game_restart_menu_down_poll(&ts, &g_touch_down,
                                        &g_touch_start_x, &g_touch_start_y))
            goto restart_menu_to_exit;
        {
            static int8_t ph = -1;
            int8_t ch = -1;
            if (ts.x >= RESTART_MENU_X && ts.x < RESTART_MENU_X + RESTART_MENU_W) {
                uint16_t yo = ts.y - (RESTART_MENU_Y + RESTART_MENU_BDR);
                uint16_t ih = (RESTART_MENU_H - 2 * RESTART_MENU_BDR) / 3;
                if (yo < ih * 3) ch = (int8_t)(yo / ih);
            }
            if (!g_prev_pressed && ts.pressed) {
                ph = ch;
            } else if (g_prev_pressed && !ts.pressed && ch == ph) {
                if (ph < 0) {
                    /* 点击菜单外 → 关闭菜单 */
                    goto restart_menu_close;
                }
                if (ph == 0) {
                    /* 重新开始 */
                    menu_restart_cleanup();
                    g_state = FISH_STATE_PLAYING;
                    fish_init();
                    ph = -1;
                    g_prev_pressed = 0;
                    return GAME_RET_INTERACT;
                }
                /* 设置 或 返回 */
                if (ph == 1) {
                    menu_enter_game_settings();
                    interacted = EXIT_RET_SETTINGS;
                } else {
                    interacted = GAME_RET_INTERACT;
                }
                ph = -1;
                g_prev_pressed = 0;
                goto restart_menu_close;
            }
            if (!ts.pressed) ph = -1;
            g_prev_pressed = ts.pressed;
            return 0;
        }
        restart_menu_to_exit:
        game_menu_restart_to_exit((uint8_t *)&g_state, FISH_STATE_EXIT_MENU, ts.y);
        fish_restore_scene_behind();
        return 0;
        restart_menu_close:
        menu_restart_cleanup();
        g_state = g_state_before_menu;
        fish_restore_scene_behind();
        g_prev_pressed = ts.pressed;
        return interacted;
    }

    /* ---- 游戏结束: 上滑/下滑呼出菜单 (不再点击任意处重开) ---- */
    if (g_state == FISH_STATE_OVER) {
        if (g_full_redraw) { draw_game_over(); g_full_redraw = 0; }
        TouchState ts;
        Touch_Scan(&ts);
        if (ts.pressed && !g_touch_down) {
            g_touch_down = 1;
            g_touch_start_x = ts.x;
            g_touch_start_y = ts.y;
            if (ts.y <= 220) g_menu_cooldown = 0;  /* 非上划区清菜单冷却 */
        }
        else if (ts.pressed && g_touch_down && g_touch_start_y < MENU_SWIPE_START_Y) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (dy_move > MENU_SWIPE_TRIGGER_DIST) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_EXIT_MENU;
                g_exit_menu_visible = 1;
                g_exit_dragging = 1;
                g_exit_drag_start_y = ts.y;
                if (dy_move > MENU_DRAG_MAX_STEP) dy_move = MENU_DRAG_MAX_STEP;
                g_exit_drag_y = -MENU_H + dy_move;
                return 0;
            }
        }
        else if (ts.pressed && g_touch_down && g_touch_start_y > 220) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (dy_move < -5) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_RESTART_MENU;
                g_restart_menu_visible = 1;
                g_restart_dragging = 1;
                g_restart_sliding = 0;
                g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
                game_restart_menu_drag_poll(&g_restart_slide_y, g_touch_start_y,
                                            &g_restart_dragging, &ts);
                return 0;
            }
        }
        else if (!ts.pressed && g_touch_down) {
            g_touch_down = 0;
            int16_t dy = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (g_touch_start_y > 220 && dy < -10) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_RESTART_MENU;
                g_restart_menu_visible = 1;
                g_restart_sliding = 1;
                g_restart_dragging = 0;
                g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H + dy;
                if (g_restart_slide_y < RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP)
                    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP;
                return GAME_RET_INTERACT;
            }
            if (g_touch_start_y < MENU_SWIPE_START_Y && dy >= MENU_SWIPE_COMMIT_DIST) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_EXIT_MENU;
                g_exit_menu_visible = 1;
                g_exit_sliding = 1;
                g_exit_drag_y = -MENU_H;
                return GAME_RET_INTERACT;
            }
        }
        g_prev_pressed = ts.pressed;
        return GAME_RET_INTERACT;
    }

    TouchState ts;
    Touch_Scan(&ts);
    g_frame++;

    uint8_t upress = ts.pressed;
    uint8_t just_pressed = (uint8_t)(upress && !g_prev_pressed);
    g_prev_pressed = upress;

    /* 触摸起始记录 (供菜单手势检测) */
    if (upress && !g_touch_down) {
        g_touch_down = 1;
        g_touch_start_x = ts.x;
        g_touch_start_y = ts.y;
        if (ts.y <= 220) g_menu_cooldown = 0;  /* 非上划区清菜单冷却 */
    }
    /* 下滑触发退出菜单拖拽 */
    if (game_menu_enter_exit_drag(&ts, (uint8_t *)&g_state,
                                   FISH_STATE_PLAYING, FISH_STATE_EXIT_MENU))
        return 0;
    /* 上划触发重新开始菜单 */
    if (game_menu_enter_restart_drag(&ts, (uint8_t *)&g_state,
                                      FISH_STATE_PLAYING, FISH_STATE_RESTART_MENU, 220))
        return 0;
    /* 松手提交菜单 */
    if (!upress && g_touch_down) {
        g_touch_down = 0;
        int16_t dy = (int16_t)ts.y - (int16_t)g_touch_start_y;
        if (game_menu_commit_exit(dy, (uint8_t *)&g_state, FISH_STATE_EXIT_MENU))
            return GAME_RET_INTERACT;
        if (game_menu_commit_restart(dy, (uint8_t *)&g_state,
                                      FISH_STATE_RESTART_MENU, 220, 10))
            return GAME_RET_INTERACT;
    }
    /* 增量拖动: 按住滑动时, 鱼按手指的位移增量平移(不吸附到手指位置, 松开即停) */
    if (g_state == FISH_STATE_PLAYING && upress) {
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
        /* 限制在8角形游动区(矩形+四角倒角, 不进圆形屏被遮挡的四角) */
        {
            int16_t px = (int16_t)g_player.x;
            int16_t py = (int16_t)g_player.y;
            fish_clamp_pos(&px, &py);
            g_player.x = (uint16_t)px;
            g_player.y = (uint16_t)py;
        }
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
                /* 8角形钳制: 不进圆形屏被遮挡的四角 */
                {
                    int16_t fx = (int16_t)f->x;
                    int16_t fy = (int16_t)f->y;
                    fish_clamp_pos(&fx, &fy);
                    f->x = (uint16_t)fx;
                    f->y = (uint16_t)fy;
                }
                /* 角落反弹: 速度朝外时反转向内, 防卡角 */
                fish_bounce_corners(f);
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
                    /* 吃掉: 按等级得分(5^等级:0级=1,1级=5...), 累计分达阈值即升一级 */
                    g_score += fish_thresh(f->level);
                    g_hud_dirty = 1;   /* 顶部分数/进度需刷新 */
                    while (g_player.level < FISH_PLAYER_LEVEL_MAX &&
                           g_score >= fish_thresh((uint8_t)(g_player.level + 1))) {
                        g_player.level++;   /* 累计分达到下一级阈值, 升一级 */
                    }
                    food_respawn(f, 1);
                } else {
                    /* 碰到更大的鱼: 游戏结束 */
                    g_state = FISH_STATE_OVER;
                    draw_game_over();
                    g_touch_down = 0;
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
                    fish_move_redraw(f);   /* 行级原子重绘(鱼移动), 减闪 */
                else
                    draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                                      FISH_COUNT, f->level, f->x, f->y);
                f->prev_x = (int16_t)f->x;
                f->prev_y = (int16_t)f->y;
                f->prev_level = f->level;
                f->drawn = 1;
            }
        }
    }

    /* 顶部信息 */
    draw_hud();

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