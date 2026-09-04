/**
 * @file    d2048.c
 * @brief   2048 游戏模块
 *          4x4 网格滑动, 数字合并, 触摸手势滑动控制
 */
#include "d2048.h"
#include "tetris.h"        /* GAME_RET_* */
#include "menu.h"          /* 菜单/退出/重开 */
#include "sok_img_win.h"   /* sok_draw_digit_16x32 */
#include "flash_store.h"   /* 字库资源从外部Flash读取 */
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "string.h"

/* ================================================================
 *  格子颜色表 (指数2对应索引1)
 * ================================================================ */
static const uint16_t tile_colors[] = {
    0,              /* 0 (空) */
    0xFF9A,         /* 2   - 暖黄色 (248,242,215) */
    0xF735,         /* 4   - 浅橙色 (245,228,175) */
    0xF64F,         /* 8   - 橙色   (240,202,122) */
    0xED4A,         /* 16  - 深橙   (232,168,84)  */
    0xDC09,         /* 32  - 棕橙色 (218,130,76)  */
    0xCB0A,         /* 64  - 红棕色 (200,98,82)   */
    0xAA8D,         /* 128 - 紫红色 (174,82,110)  */
    0x9250,         /* 256 - 紫色   (144,74,133)  */
    0x6A52,         /* 512 - 深紫色 (108,72,146)  */
    0x4AB3,         /* 1024- 蓝紫色   (72,86,156)   */
    0x2B74,         /* 2048- 蓝色   (46,110,162)  */
    0x2453,         /* 4096- 青蓝色 (34,138,154)  */
    0x2510,         /* 8192- 青绿色 (32,162,134)  */
    0x258D,         /* 16384-绿色   (36,178,104)  */
    0x2DC9,         /* 32768-黄绿色 (44,186,72)   */
};

#define D2048_DEBUG  0    /* 调试开关: 1=新块始终生成最高值, 0=正常 */

/* ================================================================
 *  游戏状态
 * ================================================================ */
static uint16_t g_grid[16];         /* 4x4 游戏格子: grid[r*4+c] */
static uint16_t g_prev_grid[16];    /* 上一帧格子快照, 用于增量绘制 */
static uint32_t g_score;
static D2048State g_state;
static uint8_t  g_grid_dirty;       /* 格子脏标记 */
static uint8_t  g_full_redraw;      /* 全屏重绘/电池闪灭后脏标记 */

/* 触摸状态: g_touch_start_x / g_prev_pressed 提升为 menu.c 共享全局 */

/* 下划菜单游戏映射 (顺序: 视频/推箱子/俄罗斯方块/华容道) */
static const uint8_t d2048_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_SOKOBAN, EXIT_RET_TETRIS, EXIT_RET_KLOTSKI, EXIT_RET_FISH
};

#define SWIPE_THRESH  10   /* 滑动最小阈值 */

/* 退出菜单行类型 (视频/推箱子/俄罗斯方块/华容道/返回) */
static const uint8_t d2048_row_types[5] = {
    MENU_ROW_SHIPIN, MENU_ROW_TUI, MENU_ROW_TETRIS, MENU_ROW_KLOTSKI, MENU_ROW_FISH
};

/* ================================================================
 *  根据数值获取颜色索引 (2→1, 4→2, 8→3, ...)
 * ================================================================ */
static uint8_t get_color_idx(uint16_t val)
{
    uint8_t i = 0;
    uint16_t v = 1;
    while (v < val && i < 15) { v <<= 1; i++; }
    return (i <= 15) ? i : 15;
}

/* ================================================================
 *  格子左上角坐标
 * ================================================================ */
static uint16_t tile_x(uint8_t col) { return D2048_GRID_X + col * (D2048_TILE_SIZE + D2048_GAP); }
static uint16_t tile_y(uint8_t row) { return D2048_GRID_Y + row * (D2048_TILE_SIZE + D2048_GAP); }

/* ================================================================
 *  绘制格子背景 (含圆角视觉效果)
 * ================================================================ */
static void draw_tile_bg(uint8_t row, uint8_t col, uint16_t color)
{
    uint16_t x = tile_x(col);
    uint16_t y = tile_y(row);
    LCD_Fill(x, y, x + D2048_TILE_SIZE - 1, y + D2048_TILE_SIZE - 1, color);
}



/* ================================================================
 *  绘制格子中的数字 (大于等于1024用8x16小字体)
 * ================================================================ */
static void draw_tile_number(uint8_t row, uint8_t col, uint16_t val)
{
    if (val == 0) return;

    uint16_t text_color = (val <= 32) ? LCD_BLACK : LCD_WHITE;

    /* 转数字数组并居中 */
    uint8_t digits[8];
    uint8_t dc = uint_to_digits(val, digits);

    LCD_CS_CLR;
    uint8_t i;

    if (dc >= 4) {
        /* 4+位数用缩放到适配小字体 */
        uint8_t tw, th, gap;
        if (dc == 4)      { tw = 9;  th = 18; gap = 1; }  /* 39px */
        else if (dc == 5) { tw = 7;  th = 14; gap = 1; }  /* 39px */
        else              { tw = 6;  th = 12; gap = 0; }  /* 6位 36px */
        uint16_t total_w = dc * tw + (dc - 1) * gap;
        uint16_t start_x = tile_x(col) + (D2048_TILE_SIZE - total_w) / 2;
        uint16_t y       = tile_y(row) + (D2048_TILE_SIZE - th) / 2;
        for (i = 0; i < dc; i++) {
            uint8_t d = digits[dc - 1 - i];
            sok_draw_digit_scaled(start_x + i * (tw + gap), y, d, text_color, tw, th);
        }
    } else {
        /* 1~3位用16x32大字体 */
        uint8_t  gap;
        if (dc == 1)      gap = 0;
        else if (dc == 2) gap = 16;
        else              gap = (D2048_TILE_SIZE - 16) / 2;   /* 12 */
        uint16_t total_w = 16 + (dc - 1) * gap;
        uint16_t start_x = tile_x(col) + (D2048_TILE_SIZE - total_w) / 2;
        uint16_t y = tile_y(row) + D2048_DIGIT_OFFY;
        for (i = 0; i < dc; i++) {
            uint8_t d = digits[dc - 1 - i];
            sok_draw_digit_16x32(start_x + i * gap, y, d, text_color);
        }
    }
    LCD_CS_SET;
}

/* ================================================================
 *  绘制整个网格 (只画变化的格子)
 * ================================================================ */
static void draw_grid(void)
{
    uint8_t r, c;
    for (r = 0; r < D2048_ROWS; r++) {
        for (c = 0; c < D2048_COLS; c++) {
            uint8_t idx = r * D2048_COLS + c;
            if (!g_full_redraw && g_grid[idx] == g_prev_grid[idx]) continue;  /* 未变化, 跳过 */

            uint16_t val = g_grid[idx];
            uint8_t ci = (val == 0) ? 0 : get_color_idx(val);
            uint16_t bg = (val == 0) ? 0xCE59 : tile_colors[ci];  /* 空格用浅灰色 */
            draw_tile_bg(r, c, bg);
            draw_tile_number(r, c, val);
        }
    }
    /* 更新快照 */
    uint8_t i;
    for (i = 0; i < 16; i++) g_prev_grid[i] = g_grid[i];
    g_full_redraw = 0;
}

/* ================================================================
 *  绘制分数
 * ================================================================ */
static void draw_score(void)
{
    /* "SCORE:" 标题已在初始化时绘制, 此处仅更新数字 */
    LCD_Fill(0, 0, 239, D2048_GRID_Y - 1, LCD_BLACK);

    /* 右对齐绘制分数数字 */
    uint8_t digits[8];
    uint8_t dc = uint_to_digits(g_score, digits);

    uint16_t total_w = dc * D2048_DIGIT_W;
    uint16_t start_x = (240 - total_w) / 2;
    uint16_t y = 2;

    LCD_CS_CLR;
    uint8_t i;
    for (i = 0; i < dc; i++) {
        uint8_t d = digits[dc - 1 - i];
        sok_draw_digit_16x32(start_x + i * D2048_DIGIT_W, y, d, LCD_WHITE);
    }
    LCD_CS_SET;
}

/* ================================================================
 *  随机生成一个新方块 (2: 90%, 4: 10%)
 * ================================================================ */
static void spawn_tile(void)
{
    uint8_t empties[16];
    uint8_t ec = 0;
    uint8_t i;
    for (i = 0; i < 16; i++) {
        if (g_grid[i] == 0) empties[ec++] = i;
    }
    if (ec == 0) return;

    uint8_t idx = empties[game_rand() % ec];
#if D2048_DEBUG
    {
        uint16_t maxv = 2;
        uint8_t i;
        for (i = 0; i < 16; i++) { if (g_grid[i] > maxv) maxv = g_grid[i]; }
        g_grid[idx] = maxv;
    }
#else
    g_grid[idx] = (game_rand() % 10 == 0) ? 4 : 2;
#endif
}

/* ================================================================
 *  单行滑动合并 (从start向end方向)
 *  返回1表示有变化
 * ================================================================ */
static uint8_t slide_row(uint16_t row[4])
{
    uint8_t moved = 0;
    int8_t i, j;

    /* 第一步: 去除0 */
    uint16_t tmp[4] = {0};
    j = 0;
    for (i = 0; i < 4; i++) {
        if (row[i] != 0) { tmp[j++] = row[i]; }
    }
    if (j == 0) return 0;

    /* 第二步: 合并 */
    uint16_t merged[4] = {0};
    uint8_t merged_flags[4] = {0};
    uint8_t k = 0;
    for (i = 0; i < j; i++) {
        if (i < j - 1 && tmp[i] == tmp[i + 1] && !merged_flags[i]) {
            merged[k] = tmp[i] << 1;   /* val * 2 */
            g_score += merged[k];
            merged_flags[i] = 1;
            merged_flags[i + 1] = 1;
            k++;
            i++;  /* skip next */
            moved = 1;
        } else if (!merged_flags[i]) {
            merged[k++] = tmp[i];
        }
    }

    /* 比较是否变化 */
    for (i = 0; i < 4; i++) {
        if (row[i] != merged[i]) moved = 1;
    }

    for (i = 0; i < 4; i++) row[i] = merged[i];
    return moved;
}

/* ================================================================
 *  按方向提取一行 (处理4个方向的翻转)
 * ================================================================ */
static void get_row(uint8_t idx, uint16_t row[4], uint8_t dir)
{
    uint8_t i;
    switch (dir) {
    case 0: /* LEFT: 从左到右 */
        for (i = 0; i < 4; i++) row[i] = g_grid[idx * 4 + i];
        break;
    case 1: /* RIGHT: 从右到左, 翻转读取 */
        for (i = 0; i < 4; i++) row[i] = g_grid[idx * 4 + 3 - i];
        break;
    case 2: /* UP: 从上到下 */
        for (i = 0; i < 4; i++) row[i] = g_grid[i * 4 + idx];
        break;
    case 3: /* DOWN: 从下到上, 翻转读取 */
        for (i = 0; i < 4; i++) row[i] = g_grid[(3 - i) * 4 + idx];
        break;
    }
}

/* ================================================================
 *  按方向写回一行
 * ================================================================ */
static void set_row(uint8_t idx, uint16_t row[4], uint8_t dir)
{
    uint8_t i;
    switch (dir) {
    case 0: /* LEFT */
        for (i = 0; i < 4; i++) g_grid[idx * 4 + i] = row[i];
        break;
    case 1: /* RIGHT: 翻转写回 */
        for (i = 0; i < 4; i++) g_grid[idx * 4 + 3 - i] = row[i];
        break;
    case 2: /* UP */
        for (i = 0; i < 4; i++) g_grid[i * 4 + idx] = row[i];
        break;
    case 3: /* DOWN: 翻转写回 */
        for (i = 0; i < 4; i++) g_grid[(3 - i) * 4 + idx] = row[i];
        break;
    }
}

/* ================================================================
 *  执行移动 (dir: 0=左, 1=右, 2=上, 3=下)
 *  返回1表示有变化
 * ================================================================ */
static uint8_t do_move(uint8_t dir)
{
    uint8_t moved = 0;
    uint8_t i;
    for (i = 0; i < 4; i++) {
        uint16_t row[4];
        get_row(i, row, dir);
        if (slide_row(row)) moved = 1;
        set_row(i, row, dir);
    }
    return moved;
}

/* ================================================================
 *  检查是否还有可用移动
 * ================================================================ */
static uint8_t can_move(void)
{
    uint8_t r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            if (g_grid[r * 4 + c] == 0) return 1;  /* 有空位 */
            if (c < 3 && g_grid[r * 4 + c] == g_grid[r * 4 + c + 1]) return 1;  /* 横向可合并 */
            if (r < 3 && g_grid[r * 4 + c] == g_grid[(r + 1) * 4 + c]) return 1;  /* 纵向可合并 */
        }
    }
    return 0;
}

/* ================================================================
 *  滑动方向检测
 * ================================================================ */
static uint8_t detect_swipe(int16_t dx, int16_t dy)
{
    int16_t adx = (dx < 0) ? -dx : dx;
    int16_t ady = (dy < 0) ? -dy : dy;
    if (adx < SWIPE_THRESH && ady < SWIPE_THRESH) return 0xFF;  /* 无效滑动 */
    if (adx > ady) return (dx > 0) ? 1 : 0;   /* 右 / 左 */
    else           return (dy > 0) ? 3 : 2;   /* 下 / 上 */
}

/* ================================================================
 *  外部接口
 * ================================================================ */

void d2048_init(void)
{
    uint8_t i;
    for (i = 0; i < 16; i++) { g_grid[i] = 0; g_prev_grid[i] = 0; }
    g_score = 0;
    g_state = D2048_STATE_PLAYING;
    g_grid_dirty = 1;
    g_full_redraw = 1;
    g_touch_down = 0;
    g_prev_pressed = 0;
    g_exit_menu_visible = 0;
    g_menu_cooldown = 0;
    g_restart_menu_visible = 0;
    g_restart_sliding = 0;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;

    /* 开局生成2个方块 */
    spawn_tile();
    spawn_tile();
}

void d2048_reset_touch(void)
{
    g_prev_pressed = 1;      /* 设为1防止进入时触发释放, 待首帧清零 */
    g_touch_down = 0;
    g_touch_start_x = 0;
    g_touch_start_y = 0;
    g_menu_cooldown = 0;
}

/**
 * @brief  2048全屏重绘，用于电池闪灭恢复，避免全屏LCD_Fill闪烁
 * @note   由 d2048_update() 下一帧执行完整grid重绘以降低SPI占用
 */
void d2048_full_redraw(void)
{
    g_full_redraw = 1;
    g_grid_dirty = 2;  /* 电池闪灭重绘，跳过全屏大填充 */
}

uint8_t d2048_is_menu_visible(void)
{
    return g_exit_menu_visible || g_restart_menu_visible;
}

/* 只恢复重开菜单上方/外侧的电池残像素细条带(不画延伸到菜单后的整格, 否则游戏图
   先于菜单显示一帧 -> 闪). d2048 GRID_Y=36: 电池上边框 y=79..80 落在 row0/row1
   缝隙(黑色)上无需处理; 菜单上方(y<110=RESTART_MENU_Y)电池主体/感叹号只压 row1,
   凸点压在菜单右侧(x=195..209, 菜单外). 格子内部数字在透明电池体内, 保持完好. */
static void d2048_restore_above_menu_slivers(void)
{
    uint16_t b1[4], b23;
    uint8_t c;
    for (c = 0; c < D2048_COLS; c++) {
        uint16_t v = g_grid[1 * D2048_COLS + c];
        b1[c] = (v == 0) ? 0xCE59 : tile_colors[get_color_idx(v)];
    }
    b23 = (g_grid[2 * D2048_COLS + 3] == 0)
        ? 0xCE59 : tile_colors[get_color_idx(g_grid[2 * D2048_COLS + 3])];

    /* 电池主体左边框压在 row1 col0 (x=45..46, y=81..109) */
    LCD_Fill(45, 81, 47, RESTART_MENU_Y, b1[0]);
    /* 电池主体右边框压在 row1 col3 (x=193..194, y=81..109) */
    LCD_Fill(193, 81, 195, RESTART_MENU_Y, b1[3]);
    /* 感叹号竖条(x=111..128, y=92..109)压在 row1 col1/col2 */
    LCD_Fill(111, 92, 118, RESTART_MENU_Y, b1[1]);
    LCD_Fill(118, 92, 122, RESTART_MENU_Y, LCD_BLACK);
    LCD_Fill(122, 92, 129, RESTART_MENU_Y, b1[2]);
    /* 电池凸点(x=195..209)在菜单外 */
    LCD_Fill(195, 104, 207, 121, b1[3]);   /* (1,3) */
    LCD_Fill(195, 121, 207, 126, LCD_BLACK);   /* row1/row2 缝隙 */
    LCD_Fill(195, 126, 207, 136, b23);     /* (2,3) */
    LCD_Fill(207, 104, 210, 136, LCD_BLACK);   /* col3 右缝 */
}

/**
 * @brief  低电图标灭段后恢复被其覆盖的底层内容
 * @note   参照推箱子的做法: LCD_ClearLowBatteryOverlay 把红色图标(边框+凸点+感叹号)
 *         刷黑, 然后同帧重画其下方内容.
 *         - 菜单可见: 立即重画菜单框(盖住电池主体外框+感叹号), 使菜单不闪一帧.
 *         - 电池凸点(x=195..210, y=104..136)在菜单框外, 压在网格格(1,3)/(2,3):
 *           立即重画它们, 凸点不留黑/红残留.
 *         - 下滑(退出)菜单覆盖整屏高, 不透明菜单框直接盖掉电池主体/感叹号, 只恢复
 *           凸点网格数据(不 draw_grid 重绘, 否则电池框内先闪一帧网格行).
 *         - 重开菜单/纯游戏重绘网格(电池顶部在重开菜单上方、属游戏区).
 */
void d2048_lowbatt_restore(void)
{
    uint8_t r;

    LCD_ClearLowBatteryOverlay();   /* 图标边框+凸点+感叹号 红→黑 */

    /* 电池凸点压在网格格(1,3),(2,3): 先重画它们.
      (要在下方不透明菜单框之前画, 因格子横跨菜单右边框; 之后重画的菜单框盖住
      压在菜单里的部分.) */
    for (r = 1; r <= 2; r++) {
        uint8_t c = 3;
        uint16_t val = g_grid[r * D2048_COLS + c];
        uint8_t ci = (val == 0) ? 0 : get_color_idx(val);
        uint16_t bg = (val == 0) ? 0xCE59 : tile_colors[ci];
        draw_tile_bg(r, c, bg);
        draw_tile_number(r, c, val);
        g_prev_grid[r * D2048_COLS + c] = val;
    }

    /* 再画不透明菜单框(盖住电池主体外框+感叹号+凸点格压在菜单内的部分)
      => 不闪/无残留 */
    if (g_exit_menu_visible) {
        /* 下滑菜单: 覆盖整个电池主体/感叹号, 菜单框直接盖掉它们. 不要在这里调
           d2048_full_redraw(), 否则 bg_redraw 会重绘网格 rows1-2(游戏)闪一帧. */
        menu_draw_nrow_frame_only(g_exit_drag_y, d2048_row_types, 5);
    } else if (g_restart_menu_visible) {
        /* 重开菜单: 只恢复菜单上方/外侧的电池残像素细条带, 再画菜单框(不重绘延伸到
          菜单后的整格, 否则游戏图先于菜单闪一帧) */
        d2048_restore_above_menu_slivers();
        menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
    } else {
        /* 纯游戏: 下一帧全网格重绘盖住图标边框/感叹号 */
        d2048_full_redraw();
    }
}

#define D2048_SAVE_LEN  37

uint8_t d2048_save_state(uint8_t *buf, uint8_t max_len)
{
    D2048State state = g_state;

    if (max_len < D2048_SAVE_LEN) return 0;
    if (state == D2048_STATE_EXIT_MENU || state == D2048_STATE_RESTART_MENU) {
        state = g_state_before_menu;
    }
    if (state > D2048_STATE_LOST) state = D2048_STATE_PLAYING;

    memcpy(buf, g_grid, sizeof(g_grid));
    memcpy(buf + sizeof(g_grid), &g_score, sizeof(g_score));
    buf[sizeof(g_grid) + sizeof(g_score)] = (uint8_t)state;
    return D2048_SAVE_LEN;
}

uint8_t d2048_restore_state(const uint8_t *buf, uint8_t len)
{
    uint8_t state;

    if (len != D2048_SAVE_LEN) return 0;
    state = buf[sizeof(g_grid) + sizeof(g_score)];
    if (state > D2048_STATE_LOST) return 0;

    memcpy(g_grid, buf, sizeof(g_grid));
    memcpy(&g_score, buf + sizeof(g_grid), sizeof(g_score));
    g_state = (D2048State)state;
    g_state_before_menu = g_state;
    g_grid_dirty = 1;
    g_full_redraw = 1;
    d2048_reset_touch();

    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    d2048_update();
    return 1;
}

/* ================================================================
 *  配置驱动菜单状态机的 hook (mark_dirty / bg_redraw)
 * ================================================================ */
void d2048_menu_mark_dirty(void)   /* 关闭/取消菜单后需重绘场景 */
{
    g_grid_dirty = 1;
    g_full_redraw = 1;
}

/* 依据脏标记重绘菜单背景: 返回1=本帧已重绘 */
uint8_t d2048_menu_bg_redraw(void)
{
    if (!g_grid_dirty) return 0;
    if (g_grid_dirty == 2) {
        /* 电池闪灭: 仅重绘第2、3行(电池下方), 避免大面积LCD_Fill闪烁 */
        uint8_t r, c;
        for (r = 1; r <= 2; r++) {
            for (c = 0; c < D2048_COLS; c++) {
                uint8_t idx = r * D2048_COLS + c;
                uint16_t val = g_grid[idx];
                uint8_t ci = (val == 0) ? 0 : get_color_idx(val);
                uint16_t bg = (val == 0) ? 0xCE59 : tile_colors[ci];
                draw_tile_bg(r, c, bg);
                draw_tile_number(r, c, val);
                g_prev_grid[idx] = val;
            }
        }
    } else {
        draw_grid();
        draw_score();
        /* draw_score 会整条清黑顶部区域(含电池图标), 此处须立即重绘电池,
          否则菜单返回/切换时电池消失又回闪(如重菜下滑返回游戏那一下闪) */
        game_battery_icon_update(164, 22, 1);
    }
    g_grid_dirty = 0;
    g_full_redraw = 0;
    return 1;
}

static const GameMenuCfg g_d2048_menu_cfg = {
    D2048_STATE_PLAYING, D2048_STATE_EXIT_MENU, D2048_STATE_RESTART_MENU,
    d2048_row_types, d2048_exit_map,
    210, 10,
    164, 22,
    d2048_menu_mark_dirty, d2048_menu_bg_redraw, d2048_menu_mark_dirty
};

uint8_t d2048_update(void)
{
    uint8_t interacted = 0;
    uint8_t battery_redraw = 0;

    /* 冷却减1 */
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 外部请求进入退出菜单 */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        g_state = D2048_STATE_EXIT_MENU;
        g_grid_dirty = 1;
        g_full_redraw = 1;
    }

    /* ---- 退出菜单 / 重新开始菜单: 配置驱动统一状态机 ---- */
    if (g_state == D2048_STATE_EXIT_MENU || g_state == D2048_STATE_RESTART_MENU) {
        uint8_t out_ret;
        uint8_t cmd = game_menu_command(&g_d2048_menu_cfg, (uint8_t *)&g_state, &out_ret);
        switch (cmd) {
            case GAME_MENU_CMD_SWITCH:   return out_ret;
            case GAME_MENU_CMD_RESTART:  d2048_init(); return GAME_RET_INTERACT;
            case GAME_MENU_CMD_SETTINGS: menu_enter_game_settings(); return EXIT_RET_SETTINGS;
            case GAME_MENU_CMD_INTERACT: return GAME_RET_INTERACT;
            default:                     return 0;
        }
    }

    /* ---- 游戏进行中 ---- */
    TouchState ts;
    Touch_Scan(&ts);

    if (ts.pressed && !g_touch_down) {
        g_touch_down = 1;
        g_touch_start_x = ts.x;
        g_touch_start_y = ts.y;
        if (ts.y <= 210)
            g_menu_cooldown = 0;  /* 触摸上半区时解除菜单冷却, 允许立刻上划 */
    }

    /* 游戏进行中: 下滑触发退出菜单拖拽 */
    if (game_menu_enter_exit_drag(&ts, (uint8_t *)&g_state,
                                   D2048_STATE_PLAYING, D2048_STATE_EXIT_MENU))
        return 0;
    /* 游戏进行中: 上划触发重新开始菜单 */
    if (game_menu_enter_restart_drag(&ts, (uint8_t *)&g_state,
                                      D2048_STATE_PLAYING, D2048_STATE_RESTART_MENU, 210))
        return 0;

    /* 松手防抖: 连续丢2帧才确认释放, 防止触摸IC抖动误触发 */
    {
        static uint8_t rel_db = 0;
        if (ts.pressed) { rel_db = 0; }
        else if (!g_touch_down) { rel_db = 0; }
        else if (++rel_db < 2) { return 0; }
    }
    if (!ts.pressed && g_touch_down) {
        g_touch_down = 0;
        /* 菜单反向滑关闭后的松手不传入游戏: 只吸收本次闭合手势的松手, 下次按下即操作游戏 */
        if (g_game_consume_release) {
            g_game_consume_release = 0;
            return interacted;
        }
        int16_t dx = (int16_t)ts.x - (int16_t)g_touch_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)g_touch_start_y;

        /* 松手确认: 退出菜单 */
        if (game_menu_commit_exit(dy, (uint8_t *)&g_state, D2048_STATE_EXIT_MENU))
            return 1;

        /* 松手确认: 重新开始菜单 */
        if (game_menu_commit_restart(dy, (uint8_t *)&g_state,
                                      D2048_STATE_RESTART_MENU, 210, 10))
            return 1;

        uint8_t dir = detect_swipe(dx, dy);
        if (dir != 0xFF && g_state == D2048_STATE_PLAYING) {
            uint8_t moved = do_move(dir);
            if (moved) {
                interacted = 1;
                Main_ClearLowBattSaveFlag();  /* 有操作变化时清除低电保存标记 */
                spawn_tile();
                g_grid_dirty = 1;

                /* 检查胜利/失败 */
                uint8_t r, c;
                for (r = 0; r < 4; r++) {
                    for (c = 0; c < 4; c++) {
                        if (g_grid[r * 4 + c] >= 32768) {
                            g_state = D2048_STATE_WON;
                        }
                    }
                }
                if (!can_move()) {
                    g_state = D2048_STATE_LOST;
                }
            }
        }
    }
    /* ---- 绘制 ---- */
    if (g_grid_dirty) {
        battery_redraw = 1;
        draw_grid();
        draw_score();
        g_grid_dirty = 0;

        /* 游戏结束 显示中文提示 */
        if (g_state == D2048_STATE_LOST) {
            uint16_t cx = (240 - 128) / 2;
            uint16_t cy = 108;
            LCD_CS_CLR;
            sok_draw_char_32(cx,       cy, FlashStore_ResLoad(FLASH_STORE_ID_32_YOU,  0, FONT_CHAR_SIZE), LCD_RED);
            sok_draw_char_32(cx + 32,  cy, FlashStore_ResLoad(FLASH_STORE_ID_32_XI3,  0, FONT_CHAR_SIZE), LCD_RED);
            sok_draw_char_32(cx + 64,  cy, FlashStore_ResLoad(FLASH_STORE_ID_32_JIE,  0, FONT_CHAR_SIZE), LCD_RED);
            sok_draw_char_32(cx + 96,  cy, FlashStore_ResLoad(FLASH_STORE_ID_32_SHU,  0, FONT_CHAR_SIZE), LCD_RED);
            LCD_CS_SET;
        }

        /* 通关 显示"恭喜通关" (4个32x32汉字) */
        if (g_state == D2048_STATE_WON) {
            uint16_t cx = (240 - 128) / 2;   /* 4个字x32=128, 居中 */
            uint16_t cy = 108;
            LCD_CS_CLR;
            sok_draw_char_32(cx, cy, FlashStore_ResLoad(FLASH_STORE_ID_32_GONG, 0, FONT_CHAR_SIZE), LCD_RED);
            sok_draw_char_32(cx + 32, cy, FlashStore_ResLoad(FLASH_STORE_ID_32_XI,   0, FONT_CHAR_SIZE), LCD_RED);
            sok_draw_char_32(cx + 64, cy, FlashStore_ResLoad(FLASH_STORE_ID_32_TONG, 0, FONT_CHAR_SIZE), LCD_RED);
            sok_draw_char_32(cx + 96, cy, FlashStore_ResLoad(FLASH_STORE_ID_32_GUAN, 0, FONT_CHAR_SIZE), LCD_RED);
            LCD_CS_SET;
        }
    }

    game_battery_icon_update(164, 22, battery_redraw);
    return interacted;
}
