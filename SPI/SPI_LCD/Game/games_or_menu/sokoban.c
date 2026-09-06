/**
 * @file    sokoban.c
 * @brief   推箱子游戏模块
 * @note    地图元素: 0=空地 1=墙 2=目的地 3=箱子 4=玩家 5=箱子在目的地上
 *          地图滚动, 触摸手势控制, 拖拽窗口
 */

#include "sokoban.h"
#include "timer.h"
#include "lcd.h"
#include "string.h"
#include "touch.h"
#include "menu.h"
#include "display.h"
#include "sok_img_win.h"
#include "sok_img_prev.h"
#include "sok_img_restart.h"
#include "sok_img_next.h"
#include "flash_store.h"   /* 图像资源从外部Flash读取 */

#define SOK_BATTERY_X  108
#define SOK_BATTERY_Y    2

/* ================================================================
 *  地图数据结构
 * ================================================================ */
static uint8_t  g_runtime_map[SOK_MAX_CELLS];
static uint8_t  g_level_rows, g_level_cols;
static int16_t  g_ox, g_oy;
static uint16_t g_move_count;
static uint16_t g_tile_size;              /* 每格尺寸 (动态缩放计算) */
static const SokobanLevel *g_cur_level;   /* 当前关卡 (只读, 指向关卡数组) */

/* 下方3个操作按钮: 上一关/重新开始/下一关 */
#define SOK_BTN1_X      15     // 上一关 X
#define SOK_BTN2_X      90     // 重新开始 X
#define SOK_BTN3_X     165     // 下一关 X
#define SOK_BTN_W       60     // 按钮宽度
#define SOK_BTN_H       60     // 按钮高度
#define SOK_BTN_Y      160     // 按钮起始Y
#define SOK_BTN_HIT_Y0 130     // 按钮命中区Y起点
#define SOK_BTN_HIT_Y1 241     // 按钮命中区Y终点(含底部)
#define SOK_SWIPE_Y    210     // 滑动检测: 下方区域Y阈值 (当Y>此值显示菜单)
#define SOK_SLIDE_MAX_STEP   2  // 跟手拖拽每帧最大步进(防跳变)
#define SOK_SLIDE_SPEED      2  // 松手后自动滑动速度(px/帧)

/* 长按与滑动阈值 */
#define SOK_HOLD_THRESHOLD    50 // 长按移动阈值(像素)
#define SOK_RELEASE_THRESHOLD 10 // 松手滑动阈值(像素)

/*关卡切换阈值*/
#define HOLD_START_US      200000UL   /* 长按启动阈值 (~200ms, 微秒) */
#define HOLD_REPEAT_US     130000UL    /* 长按重复间隔 (~130ms, 微秒) */
#define HOLD_COOLDOWN_FRAMES 100       /* 长按自动切关后菜单冷却帧数(防重复触发) */

/* 退出菜单状态 (MENU_* 定义在 sokoban.h) */
static uint8_t  s_exit_menu_visible = 0;
static uint8_t  s_exit_sliding;       /* 滑动动画进行中 */
static int16_t  s_exit_drag_y;        /* 退出菜单Y坐标 */
static uint8_t  s_exit_dragging;      /* 拖拽中标志 */
static uint16_t s_exit_drag_start_y;  /* 拖拽起点Y */

/* 退出菜单 (视频/2048/俄罗斯方块/华容道/返回, 减当前=推箱子) */
static const uint8_t sokoban_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_D2048, EXIT_RET_TETRIS, EXIT_RET_KLOTSKI, EXIT_RET_FISH
};

/* ================================================================
 *  游戏状态与菜单状态
 * ================================================================ */
static uint8_t  s_won           = 0;
static uint8_t  s_menu_visible  = 0;
static uint8_t  s_menu_sliding     = 0;    /* 跟手滑动中 */
static int16_t  s_menu_slide_y     = 240;  /* 跟手滑动: 按钮面板顶部Y */
static uint8_t  s_menu_slide_anim  = 0;    /* 松手后自动滑动动画中 */
static uint8_t  s_menu_from_swipe  = 0;    /* 菜单是否由上划手势进入(区分level_init) */
static uint8_t  s_current_level = 0;
static uint8_t  s_level_num     = 0;  /* 当前关卡号 (1-based) */
static uint8_t  g_sok_dirty     = 0;  /* 0=干净, 1=全屏重绘, 2=电池闪灭仅局部 */

/* 退出菜单行类型 (视频/2048/俄罗斯方块/华容道/返回) */
static const uint8_t sok_row_types[5] = {
    MENU_ROW_SHIPIN, MENU_ROW_2048, MENU_ROW_TETRIS, MENU_ROW_KLOTSKI, MENU_ROW_FISH
};

/* 触摸输入状态 */
static uint8_t  s_prev_pressed   = 0;
static uint16_t s_touch_start_x, s_touch_start_y;
static uint16_t s_swipe_origin_y;   // 滑动起始Y坐标(用于退出菜单手势检测)
static uint8_t  s_menu_cooldown  = 0; // 菜单冷却计数, 防止误触重复打开
static uint8_t  s_hold_button    = 0;  // 长按按钮: 0=无, 1=上一关, 3=下一关
static uint32_t s_hold_us        = 0;  // 长按累计时间(us)
static uint32_t s_hold_last_us   = 0;  // 长按上次计时基准(us)
static uint8_t  s_drag_btn       = 0;  // 拖拽追踪: 0=无, 1=上一关, 2=重新开始, 3=下一关
static uint8_t  s_won_transition = 0;  // WON切关后进入menu_visible的标记, 防松手再切
static uint8_t  s_btn_max_y     = 0;  // 按钮栏拖拽最远Y, 松手回退>5px取消
static int16_t  s_slide_max_dy  = 0;  // 上划拖拽最远位移, 松手回退>2px取消

static void sokoban_draw_cell_at(uint8_t row, uint8_t col);

/** @brief  重绘 skip_y 以下的游戏格子 (0~239), 含CS控制 */
static void sok_redraw_grid_below(int16_t y_skip)
{
    uint8_t br, bc;
    LCD_CS_CLR;
    for (br = 0; br < g_level_rows; br++) {
        int16_t cy = g_oy + (int16_t)br * (int16_t)g_tile_size;
        if (cy + (int16_t)g_tile_size <= y_skip) continue;
        if (cy >= 240) break;
        for (bc = 0; bc < g_level_cols; bc++)
            sokoban_draw_cell_at(br, bc);
    }
    LCD_CS_SET;
}

/**
 * @brief  判断某格是否原始目标点
 */
static uint8_t is_orig_target(uint8_t row, uint8_t col)
{
    uint16_t idx = row * g_level_cols + col;
    uint8_t b = g_cur_level->map[idx / 2];
    uint8_t v = (idx & 1) ? (b % 10) : (b / 10);
    return (v == SOK_TARGET || v == SOK_BOX_ON_TARGET || v == SOK_PLAYER_ON_TARGET);
}

/**
 * @brief  缩放绘制30x30图片到 g_tile_size x g_tile_size (含调色板)
 */
static void sok_draw_scaled_tile(uint16_t dx, uint16_t dy, const void *img,
                                 const uint16_t *palette)
{
    uint16_t r, c;
    for (r = 0; r < g_tile_size; r++) {
        uint16_t sr = (uint16_t)(((uint32_t)r * SOK_TILE_SIZE) / g_tile_size);
        LCD_Address_Set(dx, dy + r, dx + g_tile_size - 1, dy + r);
        for (c = 0; c < g_tile_size; c++) {
            uint16_t sc = (uint16_t)(((uint32_t)c * SOK_TILE_SIZE) / g_tile_size);
            uint16_t index = sr * SOK_TILE_SIZE + sc;
            uint16_t color = palette
                ? palette[((const uint8_t *)img)[index]]
                : ((const uint16_t *)img)[index];
            LCD_WR_DATA_16Bit(color);
        }
    }
}

/**
 * @brief  缩放绘制精灵图片 (src_size x src_size 缩放后居中于30x30格内)
 */
static void sok_draw_scaled_sprite(uint16_t dx, uint16_t dy,
                                    const void *img, uint16_t src_size, uint16_t transp,
                                    const uint16_t *palette)
{
    uint16_t ss  = (uint16_t)(((uint32_t)g_tile_size * src_size) / SOK_TILE_SIZE);
    uint16_t off = (g_tile_size - ss) / 2;
    uint16_t r, c, s;

    for (r = 0; r < ss; r++) {
        uint16_t sr = (uint16_t)(((uint32_t)r * src_size) / ss);
        c = 0;
        while (c < ss) {
            uint16_t sc = (uint16_t)(((uint32_t)c * src_size) / ss);
            uint16_t idx = sr * src_size + sc;
            uint16_t color = palette
                ? palette[((const uint8_t *)img)[idx]]
                : ((const uint16_t *)img)[idx];
            if (color == transp) { c++; continue; }
            s = c;
            c++;
            while (c < ss) {
                sc = (uint16_t)(((uint32_t)c * src_size) / ss);
                idx = sr * src_size + sc;
                color = palette
                    ? palette[((const uint8_t *)img)[idx]]
                    : ((const uint16_t *)img)[idx];
                if (color == transp) break;
                c++;
            }
            LCD_Address_Set(dx + off + s, dy + off + r, dx + off + c - 1, dy + off + r);
            for (; s < c; s++) {
                uint16_t sc2 = (uint16_t)(((uint32_t)s * src_size) / ss);
                idx = sr * src_size + sc2;
                LCD_WR_DATA_16Bit(palette
                    ? palette[((const uint8_t *)img)[idx]]
                    : ((const uint16_t *)img)[idx]);
            }
        }
    }
}

/* ================================================================
 * 外部Flash图像加载辅助 (图像+调色板已烧录到外部Flash, 读入共享scratch)
 * ================================================================ */

/**
 * @brief  加载"调色板+图像"到共享scratch
 * @param  pal_id   调色板资源ID
 * @param  pal_bytes 调色板字节数
 * @param  img_id   图像资源ID
 * @param  img_bytes 图像字节数
 * @param  pal      输出: 调色板指针(位于scratch起始)
 * @return 图像数据指针(位于调色板之后); 失败返回0
 */
static const void *sok_load_pal_img(FlashStoreId pal_id, uint16_t pal_bytes,
                                    FlashStoreId img_id, uint16_t img_bytes,
                                    const uint16_t **pal)
{
    uint8_t *s = FlashStore_Scratch();
    if (FlashStore_ResRead(pal_id, 0, s, pal_bytes) != pal_bytes) return 0;
    if (FlashStore_ResRead(img_id, 0, s + pal_bytes, img_bytes) != img_bytes) return 0;
    *pal = (const uint16_t *)s;
    return s + pal_bytes;
}

/**
 * @brief  加载纯RGB565图像(无调色板)到共享scratch
 * @return 图像数据指针; 失败返回0
 */
static const void *sok_load_img(FlashStoreId img_id, uint16_t img_bytes)
{
    uint8_t *s = FlashStore_Scratch();
    return (FlashStore_ResRead(img_id, 0, s, img_bytes) == img_bytes) ? s : 0;
}

/**
 * @brief  按需加载纯RGB565图像的前vis_rows行到共享scratch (裁剪绘制用)
 * @note   源图每行src_w个像素(2字节/像素)连续存放, 因此只读可见行即可,
 *         减少拖拽阶段每帧的外部Flash读取量(由整图降为可见行)
 * @return 图像数据指针(前vis_rows行); 失败返回0
 */
static const void *sok_load_img_rows(FlashStoreId img_id, uint16_t src_w, int16_t vis_rows)
{
    uint8_t *s = FlashStore_Scratch();
    uint16_t bytes = (uint16_t)vis_rows * src_w * 2;
    return (FlashStore_ResRead(img_id, 0, s, bytes) == bytes) ? s : 0;
}


/**
 * @brief  全屏绘制关卡到LCD, 自动计算缩放和居中
 * @param  level_index  关卡索引 (0 ~ SOKOBAN_LEVEL_COUNT-1)
 */
void sokoban_draw_level(uint8_t level_index)
{
    if (level_index >= SOKOBAN_LEVEL_COUNT) return;
    const SokobanLevel *lv = &sokoban_levels[level_index];

    /* 清屏并绘制 */
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);

    uint16_t row, col;
    LCD_CS_CLR;
    for (row = 0; row < lv->rows; row++) {
        for (col = 0; col < lv->cols; col++) {
            sokoban_draw_cell_at((uint8_t)row, (uint8_t)col);
        }
    }
    LCD_CS_SET;
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
}

/**
 * @brief  绘制地图中单个单元格 (与 draw_level 一致, 但仅一格的版本)
 */
static void sokoban_draw_cell_at(uint8_t row, uint8_t col)
{
    uint16_t ux = (uint16_t)(g_ox + (int16_t)col * (int16_t)g_tile_size);
    uint16_t uy = (uint16_t)(g_oy + (int16_t)row * (int16_t)g_tile_size);
    uint8_t cell = g_runtime_map[row * g_level_cols + col];

    switch (cell) {
    case SOK_OUTER: break;
    case SOK_FLOOR: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
                                           FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_WALL: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_WALL_PALETTE, 108,
                                           FLASH_STORE_ID_WALL_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_BOX: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_BOX_PALETTE, 350,
                                           FLASH_STORE_ID_BOX_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_BOX_ON_TARGET: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_BOX_ON_TARGET_PALETTE, 50,
                                           FLASH_STORE_ID_BOX_ON_TARGET_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_TARGET: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
                                           FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        img = sok_load_pal_img(FLASH_STORE_ID_TARGET_PALETTE, 72,
                               FLASH_STORE_ID_TARGET_IMG, 625, &pal);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_TARGET_SIZE, 0x0000, pal);
        break;
    }
    case SOK_PLAYER: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
                                           FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        img = sok_load_img(FLASH_STORE_ID_PLAYER_IMG, 1250);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_PLAYER_SIZE, 0xffff, 0);
        break;
    }
    case SOK_PLAYER_ON_TARGET: {
        const uint16_t *pal;
        const void *img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
                                           FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        img = sok_load_pal_img(FLASH_STORE_ID_TARGET_PALETTE, 72,
                               FLASH_STORE_ID_TARGET_IMG, 625, &pal);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_TARGET_SIZE, 0x0000, pal);
        img = sok_load_img(FLASH_STORE_ID_PLAYER_IMG, 1250);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_PLAYER_SIZE, 0xffff, 0);
        break;
    }
    }
}

/**
 * @brief  在地图中查找玩家当前位置
 * @param  p_row  输出: 玩家所在行
 * @param  p_col  输出: 玩家所在列
 */
static void sokoban_find_player(uint8_t *p_row, uint8_t *p_col)
{
    uint8_t r, c;
    for (r = 0; r < g_level_rows; r++) {
        for (c = 0; c < g_level_cols; c++) {
            uint8_t v = g_runtime_map[r * g_level_cols + c];
            if (v == SOK_PLAYER || v == SOK_PLAYER_ON_TARGET) {
                *p_row = r;
                *p_col = c;
                return;
            }
        }
    }
}

/**
 * @brief  判断是否通关 (所有目标点均有箱子)
 * @retval 1=通关, 0=未通关
 */
uint8_t sokoban_is_won(void)
{
    uint8_t r, c;
    for (r = 0; r < g_level_rows; r++) {
        for (c = 0; c < g_level_cols; c++) {
            if (is_orig_target(r, c)) {
                if (g_runtime_map[r * g_level_cols + c] != SOK_BOX_ON_TARGET)
                    return 0;
            }
        }
    }
    return 1;
}

/**
 * @brief  绘制导航按钮 (上一关/重新开始/下一关)
 */
static void sok_draw_nav_buttons(void)
{
    if (s_current_level > 0) {
        sok_prev_draw(SOK_BTN1_X, SOK_BTN_Y);
    }
    sok_restart_draw(SOK_BTN2_X, SOK_BTN_Y);
    if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
        sok_next_draw(SOK_BTN3_X, SOK_BTN_Y);
    }
}

/**
 * @brief  清除关卡文字"关卡N"的背景 (只清文字本身居中宽度, 不全宽, 避免左右黑块)
 * @note   y=(240-32)/2-20=84, 字高32; 文字宽 total_w=64+4+digit_w (关32+卡32+数字)
 *         与 sok_level_draw 的布局公式一致
 */
static void sok_level_clear(void)
{
    uint8_t lv = s_level_num;
    uint8_t dc = (lv / 10 > 0) ? 2 : 1;
    uint16_t dw = dc * 16 + (dc - 1) * 2;
    uint16_t total_w = 64 + 4 + dw;
    uint16_t x = (240 - total_w) / 2;
    LCD_Fill(x, 84, x + total_w, 116, LCD_BLACK);
}
/**
 * @brief  准备关卡数据结构, 不涉及LCD刷新
 * @param  level_index  关卡索引
 * @retval 1=成功, 0=关卡索引无效
 */
static uint8_t sokoban_prepare_level(uint8_t level_index)
{
    if (level_index >= SOKOBAN_LEVEL_COUNT) return 0;

    g_cur_level = &sokoban_levels[level_index];
    g_level_rows = g_cur_level->rows;
    g_level_cols = g_cur_level->cols;
    g_move_count = 0;

    {
        uint16_t max_cols = g_level_cols > g_level_rows ? g_level_cols : g_level_rows;
        g_tile_size = 200 / max_cols;
        if (g_tile_size < 10) g_tile_size = 10;
        if (g_tile_size > SOK_TILE_SIZE) g_tile_size = SOK_TILE_SIZE;
    }

    /* BCD解压: 每字节=十位*10+个位, 存2个tile */
    {
        const uint8_t *src = g_cur_level->map;
        uint16_t i;
        uint16_t total = g_level_rows * g_level_cols;
        for (i = 0; i < total; i += 2) {
            uint8_t b = *src++;
            g_runtime_map[i] = b / 10;
            if (i + 1 < total) {
                g_runtime_map[i + 1] = b % 10;
            }
        }
    }

    g_ox = 20 + (200 - (int16_t)g_level_cols * (int16_t)g_tile_size) / 2;
    g_oy = 20 + (200 - (int16_t)g_level_rows * (int16_t)g_tile_size) / 2;

    s_current_level = level_index;
    s_won = 0;
    s_exit_menu_visible = 0;
    s_menu_sliding = 0;
    s_menu_slide_y = 240;
    s_menu_slide_anim = 0;
    s_menu_from_swipe = 0;
    s_level_num = level_index + 1;
    s_drag_btn         = 0;   /* 防旧值导致松手误触发切关 */
    s_hold_button      = 0;   /* 防旧值导致menu_visible误入长按 */
    s_hold_us      = 0;
    return 1;
}

/**
 * @brief  初始化并绘制指定关卡, 同时显示导航菜单
 * @param  level_index  关卡索引
 */
void sokoban_init_level(uint8_t level_index)
{
    if (!sokoban_prepare_level(level_index)) return;

    /* 清屏并全部重绘 */
    sokoban_draw_level(level_index);

    /* 导航按钮(3个) + 关卡编号显示 */
    s_menu_visible = 1;
    s_prev_pressed = 0;  /* 重置触摸, 防止从其他游戏进入时菜单被误关 */
    sok_draw_nav_buttons();
    sok_level_draw(s_level_num, LCD_RED);
}

/**
 * @brief  电池闪灭恢复，设置脏标记由 update() 局部重绘
 * @note   避免全屏 LCD_Fill 造成闪烁，仅重绘电池覆盖的瓦片
 */
void sokoban_full_redraw(void)
{
    g_sok_dirty = 2;  /* 电池闪灭重绘，跳过全屏大填充 */
}

/**
 * @brief  检查退出菜单或导航菜单是否可见
 * @retval 1=菜单可见, 0=不可见
 */
uint8_t sokoban_is_menu_visible(void)
{
    return s_exit_menu_visible || s_menu_visible;
}

/**
 * @brief  从设置菜单返回到按钮面板 (main.c GameSettings_Update 返回分支调用)
 * @note   置 s_menu_visible=1 让下一帧 sokoban_update 重绘导航按钮面板 + 关卡编号
 */
void sokoban_menu_settings_restore(void)
{
    s_menu_visible = 1;
    /* 局部恢复(参照按钮面板退出/下划打断做法): 只清设置条带覆盖区(y>=VIDEO_MENU_Y)重绘图格,
       再画按钮面板, 不全屏重绘(会闪)。设置条带覆盖 y=52~240, 故从 VIDEO_MENU_Y 清起。 */
    LCD_Fill(0, VIDEO_MENU_Y, 239, 239, LCD_BLACK);
    sok_redraw_grid_below(VIDEO_MENU_Y);
    sok_draw_nav_buttons();
    sok_level_draw(s_level_num, LCD_RED);
}


/**
 * @brief  移动玩家 (含推箱子逻辑)
 * @param  dir  移动方向
 * @retval 1=移动成功, 0=无法移动
 */
uint8_t sokoban_move(SokobanDir dir)
{
    if (dir == SOK_DIR_NONE) return 0;

    uint8_t pr = 0, pc = 0;
    sokoban_find_player(&pr, &pc);

    static const int8_t dr[] = { -1, 1,  0, 0 };
    static const int8_t dc[] = {  0, 0, -1, 1 };
    int8_t nr = (int8_t)pr + dr[dir];
    int8_t nc = (int8_t)pc + dc[dir];

    /* 边界检查 */
    if (nr < 0 || nr >= (int8_t)g_level_rows || nc < 0 || nc >= (int8_t)g_level_cols)
        return 0;

    uint8_t target_cell = g_runtime_map[nr * g_level_cols + nc];

    /* 撞墙或边界 */
    if (target_cell == SOK_WALL || target_cell == SOK_OUTER)
        return 0;

    /* 推箱子逻辑 */
    if (target_cell == SOK_BOX || target_cell == SOK_BOX_ON_TARGET) {
        int8_t br = nr + dr[dir];
        int8_t bc = nc + dc[dir];
        if (br < 0 || br >= (int8_t)g_level_rows || bc < 0 || bc >= (int8_t)g_level_cols)
            return 0;
        uint8_t beyond = g_runtime_map[br * g_level_cols + bc];
        if (beyond == SOK_WALL || beyond == SOK_OUTER ||
            beyond == SOK_BOX   || beyond == SOK_BOX_ON_TARGET)
            return 0;

        /* 移动箱子 */
        g_runtime_map[br * g_level_cols + bc] =
            is_orig_target((uint8_t)br, (uint8_t)bc) ? SOK_BOX_ON_TARGET : SOK_BOX;
        /* 原箱子位置恢复为地板/目标 */
        g_runtime_map[nr * g_level_cols + nc] =
            is_orig_target((uint8_t)nr, (uint8_t)nc) ? SOK_TARGET : SOK_FLOOR;
    }

    /* 移动玩家 */
    g_runtime_map[pr * g_level_cols + pc] =
        is_orig_target(pr, pc) ? SOK_TARGET : SOK_FLOOR;
    g_runtime_map[nr * g_level_cols + nc] =
        is_orig_target((uint8_t)nr, (uint8_t)nc) ? SOK_PLAYER_ON_TARGET : SOK_PLAYER;

    g_move_count++;
    Main_ClearLowBattSaveFlag();  /* 移动/菜单操作清除低电量保存标志 */

    /* 局部刷新 (三个变化的格子) */
    LCD_CS_CLR;
    sokoban_draw_cell_at(pr, pc);   /* 玩家原位置 */
    sokoban_draw_cell_at((uint8_t)nr, (uint8_t)nc);   /* 玩家新位置 */
    if (target_cell == SOK_BOX || target_cell == SOK_BOX_ON_TARGET) {
        int8_t br = nr + dr[dir];
        int8_t bc = nc + dc[dir];
        sokoban_draw_cell_at((uint8_t)br, (uint8_t)bc); /* 箱子新位置 */
    }
    LCD_CS_SET;
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);

    return 1;
}

/* ================================================================
 *  触摸输入 (长按移动和菜单手势)
 * ================================================================ */

/**
 * @brief  按钮命中检测: 返回 0=未命中, 1=上一关, 2=重新开始, 3=下一关
 */
static uint8_t sok_btn_hit_test(uint16_t x, uint16_t y)
{
    if (y < SOK_BTN_HIT_Y0 || y >= SOK_BTN_HIT_Y1) return 0;
    if (x < SOK_BTN1_X + SOK_BTN_W)                return 1;
    if (x >= SOK_BTN2_X && x < SOK_BTN2_X + SOK_BTN_W) return 2;
    if (x >= SOK_BTN3_X)                          return 3;
    return 0;
}

/**
 * @brief  裁剪绘制单个按钮的可见行
 */
static void sok_draw_btn_clipped(uint16_t x, int16_t y, const uint16_t *img, int16_t vis_rows)
{
    int16_t r, c, start, sr;
    for (r = 0; r < vis_rows; r++) {
        uint8_t is_last = (r == 30);  /* 最后一行(r=30)为纯黑, 不跳过0x0000 */
        for (sr = 0; sr < 2; sr++) {
            int16_t sy = y + r * 2 + sr;
            if (sy > 239) continue;
            c = 0;
            while (c < 30) {
                if (!is_last) {
                    while (c < 30 && img[r * 30 + c] == 0x0000) c++;
                }
                if (c >= 30) break;
                start = c;
                if (is_last) {
                    c = 30;
                } else {
                    while (c < 30 && img[r * 30 + c] != 0x0000) c++;
                }
                LCD_Address_Set(x + start * 2, (uint16_t)sy,
                                x + c * 2 - 1, (uint16_t)sy);
                for (; start < c; start++) {
                    uint8_t sc;
                    for (sc = 0; sc < 2; sc++)
                        LCD_WR_DATA_16Bit(img[r * 30 + start]);
                }
            }
        }
    }
}

/**
 * @brief  上划打断退出菜单, 关闭退出菜单并显示按钮菜单
 * @retval 1=已打断, 0=无操作
 */
static uint8_t sok_exit_swipe_cancel(void)
{
    int16_t cancel_dy = 0;
    uint8_t poll_ret = game_menu_swipe_up_poll(SOK_SWIPE_Y, MENU_SWIPE_TRIGGER_DIST, &cancel_dy);
    if (!poll_ret) return 0;

    g_menu_cooldown    = 15;  /* 全局冷却, 防立即重触发 */

    s_exit_menu_visible = 0;
    s_exit_sliding     = 0;
    s_exit_dragging    = 0;
    s_prev_pressed     = 0;
    s_menu_cooldown    = 15;
    g_touch_down       = 0;   /* 清除全局触摸状态(松手由按钮面板处理, 不设吸收标志防跨游戏残留) */
    LCD_CLEAR_MENU_AREA();           /* 清退出菜单区(含边框), 防黑色/残留 */
    sok_redraw_grid_below(MENU_Y);
    if (poll_ret == 2) {
        /* 松手打断: 手指已抬起, 自动动画滑入 */
        s_menu_slide_anim = 1;
        s_menu_slide_y    = 240;
    } else {
        /* 拖拽打断: 手指还在屏幕上, 跟手滑入 */
        TouchState ts_now;
        Touch_UpdateCoords();
        Touch_Scan(&ts_now);
        s_swipe_origin_y = g_touch_start_y; /* 上划起点, 用于计算总位移 */
        s_menu_slide_y   = 240;
        s_menu_sliding   = 1;
        s_slide_max_dy   = 0;   /* 复位最远位移, 防旧值导致回退>2误取消 */
        s_drag_btn       = sok_btn_hit_test(ts_now.x, ts_now.y);
    }
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
    return 1;
}

/**
 * @brief  中间区域上划关闭推箱子退出菜单 (与四游戏退出菜单上划关闭一致)
 *         起点在 [10, SOK_SWIPE_Y) 上划即关闭, 与底部上划(sok_exit_swipe_cancel)互补。
 * @retval 1=已关闭(按钮面板跟手滑入), 0=无操作
 */
static uint8_t sok_exit_mid_up_close(void)
{
    if (!game_menu_swipe_mid_up_close(SOK_SWIPE_Y, MENU_SWIPE_TRIGGER_DIST))
        return 0;

    g_touch_down    = 0;   /* 闭合手势松手由按钮面板处理, 不设吸收标志(防跨游戏残留) */
    s_exit_menu_visible = 0;
    s_exit_sliding  = 0;
    s_exit_dragging = 0;
    s_prev_pressed  = 0;
    s_menu_cooldown = 15;
    LCD_CLEAR_MENU_AREA();           /* 清退出菜单区(含边框), 防黑色/残留 */
    sok_redraw_grid_below(MENU_Y);
    /* 按钮面板跟手滑入 (与 sok_exit_swipe_cancel 拖拽打断一致) */
    {
        TouchState ts_now;
        Touch_UpdateCoords();
        Touch_Scan(&ts_now);
        s_swipe_origin_y = ts_now.y;
        s_menu_slide_y   = 240;
        s_menu_sliding   = 1;
        s_slide_max_dy   = 0;
        s_drag_btn       = sok_btn_hit_test(ts_now.x, ts_now.y);
    }
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
    return 1;
}

/**
 * @brief  在指定Y绘制导航按钮(屏幕外部分裁剪, 从底部逐渐出现)
 */
static void sok_draw_nav_buttons_at(int16_t y)
{
    if (y > 239) return;

    LCD_CS_CLR;

    /* 按钮完全在屏幕内(y<=180)复用已有绘制函数, 节省flash */
    if (y <= 180) {
        if (s_current_level > 0) sok_prev_draw(SOK_BTN1_X, (uint16_t)y);
        sok_restart_draw(SOK_BTN2_X, (uint16_t)y);
        if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) sok_next_draw(SOK_BTN3_X, (uint16_t)y);
        LCD_CS_SET;
        return;
    }

    /* 部分在屏幕外: 逐行裁剪, 避免LCD地址回绕 */
    int16_t max_vis_row = (239 - y) / 2;
    if (max_vis_row < 0) { LCD_CS_SET; return; }
    int16_t vis_rows = max_vis_row + 1;
    if (vis_rows > 31) vis_rows = 31;

    if (s_current_level > 0) {
        const void *img = sok_load_img_rows(FLASH_STORE_ID_IMG_PREV, PREV_IMG_W, vis_rows);
        if (img) sok_draw_btn_clipped(SOK_BTN1_X, y, img, vis_rows);
    }
    {
        const void *img = sok_load_img_rows(FLASH_STORE_ID_IMG_RESTART, RESTART_IMG_W, vis_rows);
        if (img) sok_draw_btn_clipped(SOK_BTN2_X, y, img, vis_rows);
    }
    if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
        const void *img = sok_load_img_rows(FLASH_STORE_ID_IMG_NEXT, NEXT_IMG_W, vis_rows);
        if (img) sok_draw_btn_clipped(SOK_BTN3_X, y, img, vis_rows);
    }
    LCD_CS_SET;
}

/**
 * @brief  重新开始当前关卡(保留菜单状态)
 */
static void sokoban_restart_current_level(void)
{
    if (!sokoban_prepare_level(s_current_level)) return;

    s_menu_visible = 0;
    s_exit_sliding = 0;
    s_exit_dragging = 0;
    s_hold_button = 0;
    s_hold_us = 0;
    s_drag_btn = 0;
    s_menu_sliding = 0;
    s_menu_slide_y = 240;
    s_menu_slide_anim = 0;
    s_menu_from_swipe = 0;
    s_menu_cooldown = 15;
    sokoban_draw_level(s_current_level);
}

/**
 * @brief  重置触摸状态 (切换关卡时调用, 防止误触发滑动)
 */
void sokoban_reset_touch(void)
{
    s_prev_pressed = 1;      /* 设为1防止首次触控被忽略, 避免立即触发菜单 */
    s_touch_start_x = 0;
    s_touch_start_y = 0;
    s_swipe_origin_y = MENU_SWIPE_START_Y;  /* 退出菜单手势起始Y坐标设为安全值 */
    s_menu_cooldown = 0;
}
/**
 * @brief  序列化推箱子当前状态到存档缓冲。
 * @param  buf     输出缓冲
 * @param  max_len 缓冲上限
 * @return 写入字节数
 */

uint8_t sokoban_save_state(uint8_t *buf, uint8_t max_len)
{
    uint16_t cells;
    uint16_t i;
    uint16_t player = 0xFFFF;
    uint8_t box_count = 0;

    if (max_len < 3 || s_current_level >= SOKOBAN_LEVEL_COUNT) return 0;
    cells = (uint16_t)g_level_rows * g_level_cols;
    for (i = 0; i < cells; i++) {
        uint8_t value = g_runtime_map[i];
        if (value == SOK_PLAYER || value == SOK_PLAYER_ON_TARGET) {
            if (player != 0xFFFF) return 0;
            player = i;
        } else if (value == SOK_BOX || value == SOK_BOX_ON_TARGET) {
            if ((uint16_t)3 + box_count >= max_len) return 0;
            buf[3 + box_count++] = (uint8_t)i;
        }
    }
    if (player == 0xFFFF) return 0;

    buf[0] = s_current_level;
    buf[1] = (uint8_t)player;
    buf[2] = box_count;
    return (uint8_t)(3 + box_count);
}
/**
 * @brief  从存档缓冲恢复推箱子状态并重绘。
 * @param  buf 存档缓冲
 * @param  len 长度
 * @return 1=成功, 0=失败
 */

uint8_t sokoban_restore_state(const uint8_t *buf, uint8_t len)
{
    const SokobanLevel *level;
    uint16_t cells;
    uint16_t i;
    uint8_t box_count;

    if (len < 3 || buf[0] >= SOKOBAN_LEVEL_COUNT) return 0;
    level = &sokoban_levels[buf[0]];
    cells = (uint16_t)level->rows * level->cols;
    box_count = buf[2];
    if (len != (uint8_t)(3 + box_count) || buf[1] >= cells) return 0;

    for (i = 0; i < box_count; i++) {
        uint16_t j;

        if (buf[3 + i] >= cells || buf[3 + i] == buf[1]) return 0;
        for (j = 0; j < i; j++) {
            if (buf[3 + i] == buf[3 + j]) return 0;
        }
    }

    if (!sokoban_prepare_level(buf[0])) return 0;
    for (i = 0; i < cells; i++) {
        uint8_t value = g_runtime_map[i];
        if (value == SOK_BOX || value == SOK_PLAYER) {
            g_runtime_map[i] = SOK_FLOOR;
        } else if (value == SOK_BOX_ON_TARGET || value == SOK_PLAYER_ON_TARGET) {
            g_runtime_map[i] = SOK_TARGET;
        }
    }
    for (i = 0; i < box_count; i++) {
        uint8_t pos = buf[3 + i];
        g_runtime_map[pos] = (g_runtime_map[pos] == SOK_TARGET)
                           ? SOK_BOX_ON_TARGET : SOK_BOX;
    }
    g_runtime_map[buf[1]] = (g_runtime_map[buf[1]] == SOK_TARGET)
                          ? SOK_PLAYER_ON_TARGET : SOK_PLAYER;

    s_won = sokoban_is_won() ? 2 : 0;
    s_menu_visible = s_won ? 1 : 0;
    s_exit_sliding = 0;
    s_exit_dragging = 0;
    s_hold_button = 0;
    s_hold_us = 0;
    s_menu_cooldown = 0;
    sokoban_draw_level(s_current_level);
    if (s_won) {
        sok_win_draw(65, 120);
        sok_draw_nav_buttons();
        sok_level_draw(s_level_num, LCD_RED);
    }
    return 1;
}

/**
 * @brief  主更新函数, 每帧调用以处理触摸输入和游戏状态
 * @retval  1=有交互发生, 0=否
 */
/**
 * @brief  菜单可见且按住按钮时的长按切关处理 (真实时间节拍)
 * @note   从 sokoban_update 抽出, 减小超大函数避免 R_RISCV_RVC_JUMP 超范围
 */
static void sokoban_hold_update(void)
{
    TouchState ts;
    /* 长按计数中不刷新坐标, 省I2C时间 */
    if (!s_hold_button) {
        Touch_UpdateCoords();
        Touch_Scan(&ts);
        s_drag_btn = sok_btn_hit_test(ts.x, ts.y);
        if (ts.y > s_btn_max_y) s_btn_max_y = ts.y;
    }
    /* 锁定按住目标, 防手指微移导致计时归零 */
    if (s_hold_us == 0) {
        s_hold_button = (s_drag_btn == 1 || s_drag_btn == 3)
                        ? s_drag_btn : 0;
        s_hold_last_us = GetTimerCount();   /* 开始计时基准 */
    }
    if (s_hold_button == 1 || s_hold_button == 3) {
        uint32_t now_us = GetTimerCount();  /* 真实时间节拍, 与帧率无关 */
        s_hold_us += (uint32_t)(now_us - s_hold_last_us);
        s_hold_last_us = now_us;
        if (s_hold_us >= HOLD_START_US) {
            s_won_transition = 0;   /* 自动切关已发生, 清除WON标记 */
            if (s_hold_button == 1 && s_current_level > 0) {
                sokoban_init_level(s_current_level - 1);
                s_menu_visible  = 1;
                s_hold_button   = 1;    /* 保留, 防prepare_level清零后长按跟踪中断 */
                s_drag_btn      = 0;    /* 松手不触发重复切关 */
                s_prev_pressed  = 1;    /* 保持按住态, 防下一帧重新初始化 */
                s_menu_cooldown = HOLD_COOLDOWN_FRAMES;  /* 连跳期间永不过期, 松手安全 */
            } else if (s_hold_button == 3
                       && s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                sokoban_init_level(s_current_level + 1);
                s_menu_visible  = 1;
                s_hold_button   = 3;    /* 保留, 防prepare_level清零后长按跟踪中断 */
                s_drag_btn      = 0;    /* 松手不触发重复切关 */
                s_prev_pressed  = 1;    /* 保持按住态, 防下一帧重新初始化 */
                s_menu_cooldown = HOLD_COOLDOWN_FRAMES;  /* 连跳期间永不过期, 松手安全 */
            }
            s_hold_us = HOLD_START_US - HOLD_REPEAT_US;
        }
    } else {
        s_hold_us = 0;
    }
}

/**
 * @brief  重绘 [x0,x1]x[y0,y1] 范围内覆盖的地图瓦片
 * @note   电池灭段恢复: 先用 LCD_ClearLowBatteryOverlay 把电池图标整块刷黑(红色像素→黑),
 *         地图外黑区/凸起变黑即无红色残留; 此处仅重绘电池区地图瓦片盖回被刷黑的形状。
 * @param  x0,y0,x1,y1  像素矩形范围(不含右/下边界, 与 LCD_Fill 一致)
 */
static void sok_redraw_battery_tiles(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int16_t br_start = ((int16_t)y0 - g_oy) / (int16_t)g_tile_size;
    int16_t br_end   = ((int16_t)y1 - g_oy) / (int16_t)g_tile_size;
    int16_t bc_start = ((int16_t)x0 - g_ox) / (int16_t)g_tile_size;
    int16_t bc_end   = ((int16_t)x1 - g_ox) / (int16_t)g_tile_size;

    if (br_start < 0) br_start = 0;
    if (br_end >= (int16_t)g_level_rows) br_end = (int16_t)g_level_rows - 1;
    if (bc_start < 0) bc_start = 0;
    if (bc_end >= (int16_t)g_level_cols) bc_end = (int16_t)g_level_cols - 1;

    if (br_start <= br_end && bc_start <= bc_end) {
        int16_t r, c;
        LCD_CS_CLR;
        for (r = br_start; r <= br_end; r++) {
            for (c = bc_start; c <= bc_end; c++) {
                sokoban_draw_cell_at((uint8_t)r, (uint8_t)c);
            }
        }
        LCD_CS_SET;
    }
}
/* 低电图标灭段恢复(设置模式用): 设置模式下 sokoban_update() 不跑, g_sok_dirty 无人消费,
   故需立即恢复. 二级菜单条带(x=45..195)盖住电池主体, 只重绘条带右缘外凸点 + 条带未盖住时
   框外露出的电池主体; 非设置模式保持置脏, 由下一帧 sokoban_update() 恢复(覆盖重绘UI). */
/**
 * @brief  低电图标灭段恢复: 按菜单覆盖范围重绘电池区瓦片。
 * @note   设置模式重绘电池区; 纯游戏重绘整屏。
 */
void sokoban_lowbatt_restore(void)
{
    if (g_settings_from_game) {
        LCD_ClearLowBatteryOverlay();
        /* 二级设置菜单条带(x=45~195)盖住电池主体, 不重绘框内(否则先瓦片后条带闪一帧);
           只重绘条带右缘外的凸点(x=195~210) + 条带未盖住时框外露出的电池主体(滑出中) */
        sok_redraw_battery_tiles(195, 104, 210, 136);
        if (s_video_menu_slide_y > 79) {
            int16_t ye = (s_video_menu_slide_y < 161) ? s_video_menu_slide_y : 161;
            sok_redraw_battery_tiles(45, 79, 195, ye);
        }
    } else {
        sokoban_full_redraw();
    }
}
/**
 * @brief  推箱子每帧更新: 处理触摸、菜单、移动与渲染。
 * @return GAME_RET_* 返回命令
 */

uint8_t sokoban_update(void)
{
    uint8_t interacted = 0;

    /* 菜单冷却递减 */
    if (s_menu_cooldown > 0) s_menu_cooldown--;
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 设置菜单中顶部下滑→退出菜单 (main.c 设标志, 四游戏各自消费; 推箱子需自行处理) */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        s_menu_visible = 0;
        s_exit_menu_visible = 1;
        s_exit_dragging = 1;
        s_exit_sliding = 0;
        s_exit_drag_start_y = 0;   /* game_menu_drag_poll 内部用 gdp_start_ty 记录, 此值仅作输出 */
        s_exit_drag_y = -MENU_H + MENU_DRAG_MAX_STEP;
        s_hold_button = 0;
        s_hold_us = 0;
        s_prev_pressed = 1;   /* 吸收下一帧触摸抖动 */
        sokoban_draw_level(s_current_level);   /* 设置下滑已把屏幕刷黑, 需重绘整图 */
    }

    /* ---- 电池闪灭恢复: 整块刷黑图标(红→黑)+重绘该处瓦片 ---- */
    if (g_sok_dirty) {
        if (!s_exit_menu_visible) {
            /* 非菜单: 先把电池图标整块刷黑(红→黑), 再重绘电池区地图瓦片 */
            LCD_ClearLowBatteryOverlay();
            sok_redraw_battery_tiles(45, 79, 210, 161);
            /* 重绘UI覆盖层(导航按钮/通关画面) */
            if (s_menu_visible) {
                if (s_won) {
                    sok_win_draw(65, 120);
                    sok_draw_nav_buttons();
                    sok_level_draw(s_level_num, LCD_RED);
                } else {
                    sok_draw_nav_buttons();
                    sok_level_draw(s_level_num, LCD_RED);
                }
            }
        } else {
            LCD_ClearLowBatteryOverlay();
            /* 退出菜单框(x=45~195)盖住电池主体, 不重绘框内(否则先瓦片后菜单闪一帧);
               只重绘框右缘外的凸点(x=195~210) + 菜单拖拽中(未盖住电池)框外露出的主体 */
            sok_redraw_battery_tiles(195, 104, 210, 136);
            {
                int16_t mb = s_exit_drag_y + MENU_H;   /* 菜单框底部y */
                if (mb < 79) mb = 79;
                if (mb <= 161) sok_redraw_battery_tiles(45, mb, 195, 161);
            }
            menu_draw_nrow_frame_only(s_exit_drag_y, sok_row_types, 5);
        }
        g_sok_dirty = 0;
    }

    if (s_exit_menu_visible) goto process_exit_menu;
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 0);

    /* ---- 菜单可见时持续绘制(关卡编号UI) ---- */
    if (s_menu_visible) {
        sok_level_draw(s_level_num, LCD_RED);
    }

    /* ---- 通关状态 ---- */
    if (s_won) {
        /* 通关后延时0.5s再显示菜单(胜利动画+按钮) */
        if (s_won == 1) {
            Delay_Ms(500);
            s_menu_visible = 1;
            sok_draw_nav_buttons();
            sok_level_draw(s_level_num, LCD_RED);
            s_won = 2;
        }

        /* 已通关时处理按钮触摸 —— 按下即切第一关, 长按在menu_visible无缝继续 */
        TouchState ts;
        Touch_Scan(&ts);

        if (!s_prev_pressed && ts.pressed) {
            interacted = 1;
            s_hold_us = 0;
            if (ts.x < SOK_BTN1_X + SOK_BTN_W && ts.y >= SOK_BTN_HIT_Y0 && ts.y < SOK_BTN_HIT_Y1) {
                if (s_current_level > 0) {
                    sokoban_init_level(s_current_level - 1);
                    s_hold_button   = 1;
                    s_drag_btn      = 1;   /* 让menu_visible保持长按跟踪 */
                    s_won_transition = 1;  /* 标记来自WON, menu_visible首次松手不切关 */
                }
            }
            else if (ts.x >= SOK_BTN2_X && ts.x < SOK_BTN2_X + SOK_BTN_W && ts.y >= SOK_BTN_HIT_Y0 && ts.y < SOK_BTN_HIT_Y1) {
                sokoban_restart_current_level();
            }
            else if (ts.x >= SOK_BTN3_X && ts.y >= SOK_BTN_HIT_Y0 && ts.y < SOK_BTN_HIT_Y1) {
                if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                    sokoban_init_level(s_current_level + 1);
                    s_hold_button   = 3;
                    s_drag_btn      = 3;   /* 让menu_visible保持长按跟踪 */
                    s_won_transition = 1;  /* 标记来自WON, menu_visible首次松手不切关 */
                }
            }
            /* 记录本次触摸起点用于后续滑动检测 */
            s_touch_start_x = ts.x;
            s_touch_start_y = ts.y;
            s_swipe_origin_y = ts.y;
            s_btn_max_y      = ts.y;
            s_prev_pressed   = 1;
        }
        /* 下滑进入退出菜单 (未命中非restart按钮时生效) */
        if (ts.pressed && s_won
            && s_swipe_origin_y < MENU_SWIPE_START_Y
            && s_menu_cooldown == 0) {
            int16_t dy_move;
            Touch_UpdateCoords();
            Touch_Scan(&ts);
            dy_move = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
            if (dy_move > MENU_SWIPE_TRIGGER_DIST) {
                s_exit_menu_visible = 1;
                s_exit_dragging = 1;
                s_exit_sliding = 0;
                s_exit_drag_start_y = ts.y;
                if (dy_move > MENU_DRAG_MAX_STEP)
                    dy_move = MENU_DRAG_MAX_STEP;
                s_exit_drag_y = -MENU_H + dy_move;
                s_hold_button = 0;
                return interacted;
            }
        }
        /* 松手下滑提交退出菜单 */
        if (!s_prev_pressed) {
            int16_t exit_dy =
                (int16_t)ts.y - (int16_t)s_swipe_origin_y;
            if (exit_dy >= MENU_SWIPE_COMMIT_DIST
                && s_swipe_origin_y < MENU_SWIPE_START_Y
                && s_menu_cooldown == 0) {
                s_exit_menu_visible = 1;
                s_exit_dragging = 0;
                s_exit_sliding = 1;
                s_exit_drag_y = -MENU_H;
                s_prev_pressed = 0;
                return interacted;
            }
        }
        if (!ts.pressed) {
            s_hold_button = 0;
            s_hold_us = 0;
        }

        s_prev_pressed = ts.pressed;
        return interacted;
    }

    /* ---- 菜单可见状态 (拖拽追踪: 按下记录→拖拽更新→松手执行) ---- */
    if (s_menu_visible) {
        TouchState ts;
        Touch_Scan(&ts);

        /* 由上划进入菜单: 屏蔽按钮交互, 等手指完全松开再恢复 */
        if (s_menu_from_swipe) {
            if (ts.pressed) {
                s_prev_pressed = 1;
                return interacted;
            }
            /* 手指松开, 清除保护, 下一帧正常交互 */
            s_menu_from_swipe = 0;
            s_prev_pressed = 0;
            s_drag_btn = 0;
            s_hold_us = 0;
            return interacted;
        }

        /* 按下: 记录命中按钮 */
        if (!s_prev_pressed && ts.pressed) {
            interacted = 1;
            s_drag_btn = sok_btn_hit_test(ts.x, ts.y);
            s_hold_us = 0;
            s_hold_button = 0;
            s_prev_pressed = 1;
            s_touch_start_x = ts.x;
            s_touch_start_y = ts.y;
            s_swipe_origin_y = ts.y;
            s_btn_max_y = ts.y;
            return interacted;
        }

        /* 按住中: 拖拽追踪 + 长按连跳(btn1/btn3有效时) */
        if (ts.pressed) {
            /* 下划打断按钮菜单, 打开退出菜单 */
            if (s_swipe_origin_y < MENU_SWIPE_START_Y
                && s_menu_cooldown == 0) {
                int16_t dy_move;
                Touch_UpdateCoords();
                Touch_Scan(&ts);
                dy_move = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
                if (dy_move > MENU_SWIPE_TRIGGER_DIST) {
                    s_menu_visible      = 0;
                    s_exit_menu_visible = 1;
                    s_exit_dragging     = 1;
                    s_exit_sliding      = 0;
                    s_exit_drag_start_y = ts.y;
                    if (dy_move > MENU_DRAG_MAX_STEP)
                        dy_move = MENU_DRAG_MAX_STEP;
                    s_exit_drag_y       = -MENU_H + dy_move;
                    s_hold_button       = 0;
                    s_hold_us       = 0;
                    /* 清除按钮区域: 先黑底再重绘游戏格子, 防边缘残留 */
                    {
                        int16_t fy;
                        LCD_CS_CLR;
                        for (fy = SOK_BTN_Y; fy < SOK_BTN_Y + SOK_BTN_H + 2; fy++) {
                            LCD_Address_Set(SOK_BTN1_X, fy, SOK_BTN1_X + SOK_BTN_W - 1, fy);
                            for (uint8_t k = 0; k < SOK_BTN_W; k++) LCD_WR_DATA_16Bit(LCD_BLACK);
                            LCD_Address_Set(SOK_BTN2_X, fy, SOK_BTN2_X + SOK_BTN_W - 1, fy);
                            for (uint8_t k = 0; k < SOK_BTN_W; k++) LCD_WR_DATA_16Bit(LCD_BLACK);
                            LCD_Address_Set(SOK_BTN3_X, fy, SOK_BTN3_X + SOK_BTN_W - 1, fy);
                            for (uint8_t k = 0; k < SOK_BTN_W; k++) LCD_WR_DATA_16Bit(LCD_BLACK);
                        }
                        LCD_CS_SET;
                    }
                    sok_redraw_grid_below(SOK_BTN_Y);
                    return interacted;
                }
            }

            /* 二次上划: 按钮面板已显示时, 再次上划进设置菜单 (亮度/息屏/方向/返回)
             * 置于下划打断分支之外, 起点在按钮区(中下部)也能触发 */
            if (s_menu_cooldown == 0
                && (int16_t)s_swipe_origin_y - (int16_t)ts.y > MENU_SWIPE_COMMIT_DIST) {
                int16_t up_move;
                Touch_UpdateCoords();
                Touch_Scan(&ts);
                up_move = (int16_t)s_swipe_origin_y - (int16_t)ts.y;
                if (up_move > MENU_SWIPE_COMMIT_DIST) {
                    s_menu_visible      = 0;   /* 按钮面板直接消失 */
                    s_hold_button       = 0;
                    s_hold_us           = 0;
                    s_prev_pressed      = 0;
                    s_menu_cooldown     = 15;
                    /* 局部清除按钮面板区(与按钮面板退出一致): 只清文字窄带 + 按钮带两个小区域,
                       不全宽/不全屏重绘(会闪)。文字用 sok_level_clear(文字本身居中宽度),
                       按钮带全宽(3按钮分布广)。 */
                    sok_level_clear();
                    LCD_Fill(0, SOK_BTN_Y, 239, SOK_BTN_Y + SOK_BTN_H, LCD_BLACK);
                    sok_redraw_grid_below(84);
                    menu_enter_game_settings_swipe(s_swipe_origin_y);
                    return interacted;
                }
            }
            sokoban_hold_update();
        }

        /* 松手: 执行拖拽目标按钮 */
        if (s_prev_pressed && !ts.pressed) {
            if (s_won_transition) {  /* WON切关后首次松手, 拦截不切, 清除标记 */
                s_won_transition = 0;
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_prev_pressed = 0;
                return interacted;
            }
            if (s_menu_cooldown > 0) {  /* 长按自动切关后的冷却期内松手 */
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_prev_pressed = 0;
                return interacted;
            }
            if (s_hold_button != 0 && s_drag_btn == 0 && s_hold_us > 0) {  /* 长按自动切关已发生(drag_btn被清零), 松手不做额外操作 */
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_prev_pressed = 0;
                return interacted;
            }
            /*  按钮栏下滑>5px取消 */
            if (s_btn_max_y - s_touch_start_y > 5) {
                s_menu_visible = 0;
                /* 窄带清除: 文字带+按钮带, 避免整块黑闪 */
                sok_level_clear();
                LCD_Fill(0, SOK_BTN_Y, 239, SOK_BTN_Y + SOK_BTN_H, LCD_BLACK);
                sok_redraw_grid_below(84);
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_menu_cooldown = 15;
                s_prev_pressed = 0;
                return interacted;
            }
            if (s_drag_btn == 1 && s_current_level > 0) {
                sokoban_init_level(s_current_level - 1);
                s_menu_visible = 1;
            } else if (s_drag_btn == 2) {
                sokoban_restart_current_level();
            } else if (s_drag_btn == 3
                       && s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                sokoban_init_level(s_current_level + 1);
                s_menu_visible = 1;
            } else if (s_drag_btn == 0 && !s_menu_from_swipe) {
                /* 非上划的菜单: 点击空白区关闭菜单开始游戏 */
                s_menu_visible = 0;
                /* 分两次清窄条带: 关卡文字带(84~116) + 按钮带(160~220), 不清 y=84~239 整块,
                   避免大片黑闪。中间 y=116~160 地图格子保留不动。
                   文字/按钮是透明覆盖, 其下格子由 sok_redraw_grid_below 重绘恢复。 */
                sok_level_clear();
                LCD_Fill(0, SOK_BTN_Y, 239, SOK_BTN_Y + SOK_BTN_H, LCD_BLACK);
                sok_redraw_grid_below(84);
                s_menu_cooldown =
                    (s_swipe_origin_y < MENU_SWIPE_START_Y) ? 0 : 15;
            }
            s_drag_btn = 0;
            s_hold_button = 0;
            s_hold_us = 0;
        }

        s_prev_pressed = ts.pressed;
        return interacted;
    }

    /* ---- 跟手上划: 拖拽模式 (限速, 跟随手指) ---- */
    if (s_menu_sliding) {
        TouchState ts_s;
        Touch_UpdateCoords();  /* 实时读取触摸坐标, 保证跟手流畅 */
        Touch_Scan(&ts_s);

        if (ts_s.pressed) {
            int16_t total_dy = (int16_t)s_swipe_origin_y - (int16_t)ts_s.y;
            if (total_dy > s_slide_max_dy) s_slide_max_dy = total_dy;
            int16_t target = 240 - total_dy;
            if (target < SOK_BTN_Y) target = SOK_BTN_Y;
            if (target > 240) target = 240;
            /* 限速: 每帧最多移动 SOK_SLIDE_MAX_STEP, 防跳变 */
            if (target < s_menu_slide_y) {
                int16_t step = s_menu_slide_y - target;
                if (step > SOK_SLIDE_MAX_STEP) step = SOK_SLIDE_MAX_STEP;
                s_menu_slide_y -= step;
            }
        }

        if (!ts_s.pressed) {
            /* 松手: 从最远位移回退>2px则取消, 否则自动滑入 */
            int16_t total_dy = (int16_t)s_swipe_origin_y - (int16_t)ts_s.y;
            int16_t back_dist = s_slide_max_dy - total_dy;
            if (back_dist > 2) {
                s_menu_sliding = 0;
                int16_t clip_y = s_menu_slide_y;
                if (clip_y > 239) clip_y = 239;
                s_menu_slide_y = 240;
                s_prev_pressed  = 0;  /* 释放触摸态, 防止下一帧误触发 */
                s_menu_cooldown = 15; /* 冷却, 防立即重触发上划 */
                LCD_Fill(0, (uint16_t)clip_y, 240, 240, LCD_BLACK);
                sok_redraw_grid_below(clip_y);
            } else {
                s_menu_sliding = 0;
                s_menu_slide_anim = 1;
            }
            return 0;
        }

        /* 绘制 (仅按住时到达) */
        if (s_menu_slide_y <= SOK_BTN_Y) {
            /* 拖到底: 切换为菜单可见模式 */
            s_menu_sliding         = 0;
            s_menu_visible         = 1;
            s_menu_from_swipe      = 1;
            s_prev_pressed         = 1;  /* 按住态, 防止下一帧误触 */
            s_drag_btn             = sok_btn_hit_test(ts_s.x, ts_s.y);
            sok_draw_nav_buttons_at(SOK_BTN_Y);
            sok_level_draw(s_level_num, LCD_RED);
        } else {
            sok_draw_nav_buttons_at(s_menu_slide_y);
        }
        return 0;
    }

    /* ---- 跟手上划: 自动滑动动画 (松手后滑到最终位置) ---- */
    if (s_menu_slide_anim) {
        s_menu_slide_y -= SOK_SLIDE_SPEED;
        if (s_menu_slide_y <= SOK_BTN_Y) {
            s_menu_slide_y = SOK_BTN_Y;
            s_menu_slide_anim = 0;
            s_menu_visible = 1;
            s_menu_from_swipe = 1;  /* 防止首帧误触关闭菜单 */
            s_prev_pressed = 1;     /* 吸收动画后触摸抖动 */
            s_drag_btn = 0;         /* 清除旧拖拽状态 */
            sok_draw_nav_buttons_at(SOK_BTN_Y);
            sok_level_draw(s_level_num, LCD_RED);
        } else {
            sok_draw_nav_buttons_at(s_menu_slide_y);
        }
        return 0;
    }

process_exit_menu:
    /* ---- 退出菜单不刷新电池图标，其他菜单保持计时器运行 ---- */
    if (!s_exit_menu_visible) {
        game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 0);
    }
    /* ---- 退出菜单处理 (拖拽模式) ---- */
    if (s_exit_menu_visible) {
        /* 拖拽模式处理 */
        if (s_exit_dragging) {
            /* 上划打断退出菜单拖拽 */
            if (sok_exit_swipe_cancel()) return 0;

            TouchState ts_d;
            Touch_Scan(&ts_d);
            uint8_t poll_ret = game_menu_drag_poll(&s_exit_drag_y, &s_exit_drag_start_y,
                                    &s_exit_dragging, &ts_d, sok_row_types);
            if (poll_ret == 2) {
                /* 上划回退>10px松手取消: 清可见区域并补画格子 */
                menu_clear_exit_area_to(s_exit_drag_y);
                s_exit_menu_visible = 0;
                s_exit_sliding = 0;
                s_exit_dragging = 0;
                sok_redraw_grid_below((s_exit_drag_y < 0) ? 0 : s_exit_drag_y);
                s_prev_pressed = 1;
                s_menu_cooldown = 15;
            } else if (poll_ret == 1) {
                s_exit_sliding = 1;
            }
            return 0;
        }

        /* 滑动动画模式: 自动滑至目标, 逐帧刷新 */
        if (s_exit_sliding) {

            /* 上划打断退出菜单滑动动画 */
            if (sok_exit_swipe_cancel()) return 0;

            s_exit_drag_y += MENU_SLIDE_SPEED;
            if (s_exit_drag_y >= MENU_Y) {
                s_exit_drag_y = MENU_Y;
                s_exit_sliding = 0;
                s_prev_pressed = 0;  /* 滑入结束重置按下态, 防拖拽松手被误判为点菜单外关闭 */
            }
            menu_draw_nrow_frame_only(s_exit_drag_y, sok_row_types, 5);
            return 0;
        }

        TouchState ts;

        /* 上划打断退出菜单(静止态) */
        if (sok_exit_swipe_cancel()) return 0;
        /* 中间区域上划关闭退出菜单返回按钮面板 */
        if (sok_exit_mid_up_close()) return 0;

        uint8_t ret = game_exit_menu_poll(sokoban_exit_map, &s_prev_pressed, &ts);
        if (ret == EXIT_RET_BACK) {
            /* "返回" 键退出菜单回到游戏, 擦除菜单区域, 补画地图 */
            s_exit_menu_visible = 0;
            s_exit_sliding = 0;
            s_exit_dragging = 0;
            s_menu_visible = 0;
            LCD_CLEAR_MENU_AREA();
            sok_redraw_grid_below(MENU_Y);
            if (s_won) {
                s_menu_visible = 1;
                sok_win_draw(65, 120);
                sok_draw_nav_buttons();
                sok_level_draw(s_level_num, LCD_RED);
            }
            game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
            s_prev_pressed = 1;
            s_touch_start_x = ts.x;
            s_touch_start_y = ts.y;
            s_swipe_origin_y = ts.y;
            s_menu_cooldown = 15;
            return 0;
        }
        if (ret != EXIT_RET_NONE) {
            s_exit_menu_visible = 0;
            return ret;
        }
        return 0;
    }

    /* ---- 游戏进行中 ---- */
    TouchState ts;
    Touch_Scan(&ts);

    if (!s_prev_pressed && ts.pressed) {
        interacted = 1;
        s_touch_start_x = ts.x;
        s_touch_start_y = ts.y;
        s_swipe_origin_y = ts.y;   // 记录触摸起始Y(用于退出菜单手势)
    }

    if (ts.pressed) {
        Touch_UpdateCoords();
        Touch_Scan(&ts);

        /* 检测退出菜单手势: 上方区域 + 向下滑动 > 5px 则可拖出退出菜单 */
        if (s_swipe_origin_y < MENU_SWIPE_START_Y
            && s_menu_cooldown == 0) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
            if (dy_move > MENU_SWIPE_TRIGGER_DIST) {
                s_exit_menu_visible = 1;
                s_exit_dragging = 1;
                s_exit_drag_start_y = ts.y;
                if (dy_move > MENU_DRAG_MAX_STEP)
                    dy_move = MENU_DRAG_MAX_STEP;
                s_exit_drag_y = -MENU_H + dy_move;
                return 0;
            }
        }

        /* 底部上划: 跟下滑菜单一样, 移动>5px即启动跟手滑动 */
        if (s_swipe_origin_y > SOK_SWIPE_Y
            && s_menu_cooldown == 0) {
            int16_t swipe_dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
            if (swipe_dy < 0 && -swipe_dy > MENU_SWIPE_TRIGGER_DIST) {
                s_menu_sliding = 1;
                s_slide_max_dy = -swipe_dy; /* 初始位移即为最远 */
                int16_t init_y = 240 - (-swipe_dy);
                if (init_y < SOK_BTN_Y) init_y = SOK_BTN_Y;
                s_menu_slide_y = init_y;
                s_drag_btn = sok_btn_hit_test(ts.x, ts.y);
                s_prev_pressed = 1;
                return 0;
            }
        }

        int16_t dx = (int16_t)ts.x - (int16_t)s_touch_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_touch_start_y;
        int16_t adx = (dx < 0) ? -dx : dx;
        int16_t ady = (dy < 0) ? -dy : dy;

        SokobanDir dir = SOK_DIR_NONE;
        if (adx > ady && adx > SOK_HOLD_THRESHOLD) {
            dir = (dx > 0) ? SOK_DIR_RIGHT : SOK_DIR_LEFT;
        } else if (ady > SOK_HOLD_THRESHOLD) {
            dir = (dy > 0) ? SOK_DIR_DOWN : SOK_DIR_UP;
        }

        if (dir != SOK_DIR_NONE) {
            /* 过滤误触: 顶部区域不触发上滑, 底部区域按下滑 */
            if ((dir == SOK_DIR_UP && s_swipe_origin_y > SOK_SWIPE_Y)
                || (dir == SOK_DIR_DOWN
                    && s_swipe_origin_y < MENU_SWIPE_START_Y)) {
                s_touch_start_x = ts.x;
                s_touch_start_y = ts.y;
            } else {
                sokoban_move(dir);
                if (sokoban_is_won()) {
                    sok_win_draw(65, 120);
                    s_won = 1;
                }
                s_touch_start_x = ts.x;
                s_touch_start_y = ts.y;
            }
        }
    }

    if (s_prev_pressed && !ts.pressed) {
        int16_t dx = (int16_t)ts.x - (int16_t)s_touch_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_touch_start_y;
        int16_t adx = (dx < 0) ? -dx : dx;
        int16_t ady = (dy < 0) ? -dy : dy;

        /* 释放检测: 退出菜单手势优先于移动方向解析 */
        int16_t exit_dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
        if (exit_dy >= MENU_SWIPE_COMMIT_DIST
            && s_swipe_origin_y < MENU_SWIPE_START_Y
            && s_menu_cooldown == 0) {
            s_exit_menu_visible = 1;
            s_exit_sliding = 1;
            s_exit_drag_y = -MENU_H;
            s_prev_pressed = 0;
            return interacted;
        }

        /* 显示菜单手势: 底部区域上滑(长距离滑动生效) */
        int16_t swipe_dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
        int16_t swipe_ady = (swipe_dy < 0) ? -swipe_dy : swipe_dy;

        /* 底部区域向上滑动超过阈值时, 显示导航菜单(上一关+重新开始+下一关) */
        if (swipe_ady > MENU_SWIPE_TRIGGER_DIST && swipe_dy < 0 && s_swipe_origin_y > SOK_SWIPE_Y && s_menu_cooldown == 0) {
            if (s_current_level > 0) {
                sok_prev_draw(SOK_BTN1_X, SOK_BTN_Y);
            }
            sok_restart_draw(SOK_BTN2_X, SOK_BTN_Y);
            if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                sok_next_draw(SOK_BTN3_X, SOK_BTN_Y);
            }
            s_menu_visible = 1;
            s_prev_pressed = 0;
            return interacted;
        }

        SokobanDir dir = SOK_DIR_NONE;
        if (adx > ady && adx > SOK_RELEASE_THRESHOLD) {
            dir = (dx > 0) ? SOK_DIR_RIGHT : SOK_DIR_LEFT;
        } else if (ady > SOK_RELEASE_THRESHOLD) {
            dir = (dy > 0) ? SOK_DIR_DOWN : SOK_DIR_UP;
        }

        if (dir != SOK_DIR_NONE
            && !(s_swipe_origin_y < MENU_SWIPE_START_Y
                 && dir == SOK_DIR_DOWN)) {
            sokoban_move(dir);
            if (sokoban_is_won()) {
                sok_win_draw(65, 120);
                s_won = 1;
            }
        }
    }

    s_prev_pressed = ts.pressed;
    return interacted;
}
