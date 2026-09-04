/**
 * @file    klotski.c
 * @brief   华容道 (15-Puzzle) 游戏模块
 *          4x4 数字拼图, 触摸滑动, 步数+计时
 */
/* 调试模式: 直接设为目标排列, 滑动一次即通关验证 */
//#define KLOTSKI_TEST_MODE

#include "klotski.h"
#include "tetris.h"       /* GAME_RET_* */
#include "menu.h"
#include "sok_img_win.h"
#include "flash_store.h"   /* 字库资源从外部Flash读取 */
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "timer.h"
#include "string.h"

/* ================================================================
 *  颜色
 * ================================================================ */
#define KLOTSKI_EMPTY_COLOR  0xE73C   /* 空格: 蓝灰色 */
#define KLOTSKI_TILE_COLOR   0x2DC5   /* 方块色: 绿色 */

/* ================================================================
 *  游戏状态
 * ================================================================ */
static uint8_t  g_grid[KLOTSKI_SIZE];     /* 4x4: 1~15=方块编号, 0=空格 */
static uint8_t  g_empty_r, g_empty_c;     /* 空格位置 */
static uint16_t g_moves;                  /* 移动步数 */
static uint32_t g_timer_start;           /* 计时起点(ticks) */
static uint16_t g_time_sec;              /* 已用时间秒数 */
static KlotskiState g_state;
static uint8_t  g_grid_dirty;

/* 触摸状态 */

/* 下划菜单游戏映射 (顺序: 视频/推箱子/2048/俄罗斯方块, 减当前=华容道) */
static const uint8_t klotski_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_SOKOBAN, EXIT_RET_D2048, EXIT_RET_TETRIS, EXIT_RET_FISH
};

#define SWIPE_THRESH  10   /* 滑动最小阈值(移出空格) */

/* 退出菜单行类型 (klotski专属) */
static const uint8_t klotski_row_types[5] = {
    MENU_ROW_SHIPIN, MENU_ROW_TUI, MENU_ROW_2048,
    MENU_ROW_TETRIS, MENU_ROW_FISH
};

/* ================================================================
 *  目标排列
 * ================================================================ */
static const uint8_t goal_grid[KLOTSKI_SIZE] = {
    1,  2,  3,  4,
    5,  6,  7,  8,
    9,  10, 11, 12,
    13, 14, 15, 0
};

/* ================================================================
 *  格子左上角坐标
 * ================================================================ */
static uint16_t tile_x(uint8_t col) { return KLOTSKI_GRID_X + col * (KLOTSKI_TILE_SIZE + KLOTSKI_GAP); }
static uint16_t tile_y(uint8_t row) { return KLOTSKI_GRID_Y + row * (KLOTSKI_TILE_SIZE + KLOTSKI_GAP); }

/* ================================================================
 *  绘制格子背景
 * ================================================================ */
static void draw_tile_bg(uint8_t row, uint8_t col, uint16_t color)
{
    uint16_t x = tile_x(col);
    uint16_t y = tile_y(row);
    LCD_Fill(x, y, x + KLOTSKI_TILE_SIZE - 1, y + KLOTSKI_TILE_SIZE - 1, color);
}

/* ================================================================
 *  绘制格子中的数字 (1~2位用16x32, 3位缩放)
 * ================================================================ */
static void draw_tile_number(uint8_t row, uint8_t col, uint8_t val)
{
    if (val == 0) return;

    uint16_t text_color = LCD_WHITE;

    uint8_t digits[8];
    uint8_t dc = uint_to_digits(val, digits);

    LCD_CS_CLR;
    uint8_t i;

    if (dc <= 2) {
        /* 1~2位用16x32大字体 */
        uint8_t gap = (dc == 1) ? 0 : (KLOTSKI_TILE_SIZE - 16) / 2;
        uint16_t total_w = 16 + (dc - 1) * gap;
        uint16_t start_x = tile_x(col) + (KLOTSKI_TILE_SIZE - total_w) / 2;
        uint16_t y = tile_y(row) + KLOTSKI_DIGIT_OFFY;
        for (i = 0; i < dc; i++) {
            uint8_t d = digits[dc - 1 - i];
            sok_draw_digit_16x32(start_x + i * gap, y, d, text_color);
        }
    } else {
        /* 3位缩放绘制 (正常华容道最多15即2位, 预留扩展) */
        uint8_t tw = 10, th = 20, gap = 1;
        uint16_t total_w = dc * tw + (dc - 1) * gap;
        uint16_t start_x = tile_x(col) + (KLOTSKI_TILE_SIZE - total_w) / 2;
        uint16_t y = tile_y(row) + (KLOTSKI_TILE_SIZE - th) / 2;
        for (i = 0; i < dc; i++) {
            uint8_t d = digits[dc - 1 - i];
            sok_draw_digit_scaled(start_x + i * (tw + gap), y, d, text_color, tw, th);
        }
    }
    LCD_CS_SET;
}

/* ================================================================
 *  绘制整个网格
 * ================================================================ */
static void draw_grid(void)
{
    uint8_t r, c;
    for (r = 0; r < KLOTSKI_ROWS; r++) {
        for (c = 0; c < KLOTSKI_COLS; c++) {
            uint8_t val = g_grid[r * KLOTSKI_COLS + c];
            uint16_t bg = (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR;
            draw_tile_bg(r, c, bg);
            if (val != 0) {
                draw_tile_number(r, c, val);
            }
        }
    }
}

/* ================================================================
 *  绘制状态 (步数显示)
 * ================================================================ */
static void draw_status(void)
{
    LCD_Fill(0, 0, 239, KLOTSKI_GRID_Y - 1, LCD_BLACK);

    LCD_CS_CLR;

    /* 步数: 居中绘制 */
    {
        uint8_t digits[8];
        uint8_t dc = uint_to_digits(g_moves, digits);

        uint16_t y = (KLOTSKI_GRID_Y - 32) / 2;
        uint16_t start_x = (240 - dc * 16) / 2;
        uint8_t i;
        for (i = 0; i < dc; i++) {
            sok_draw_digit_16x32(start_x + (dc - 1 - i) * 16, y, digits[i], LCD_YELLOW);
        }
    }

    LCD_CS_SET;
    game_battery_icon_update(164, 22, 1);
}

/* ================================================================
 *  查找空格位置
 * ================================================================ */
static void find_empty(void)
{
    uint8_t i;
    for (i = 0; i < KLOTSKI_SIZE; i++) {
        if (g_grid[i] == 0) {
            g_empty_r = i / KLOTSKI_COLS;
            g_empty_c = i % KLOTSKI_COLS;
            return;
        }
    }
}

/* ================================================================
 *  检查是否通关
 * ================================================================ */
static uint8_t is_won(void)
{
    uint8_t i;
    for (i = 0; i < KLOTSKI_SIZE; i++) {
        if (g_grid[i] != goal_grid[i]) return 0;
    }
    return 1;
}

/* ================================================================
 *  移动: 从用户视角, 滑动方向数字移入空格
 *  dir: 0=下(用户向上滑, 下方数字上移), 1=上, 2=右, 3=左
 *  返回1表示移动有效
 * ================================================================ */
static uint8_t do_move(uint8_t dir)
{
    int8_t dr = 0, dc = 0;
    /* 数字移入空格的方向 -> 数字来源(反向) */
    switch (dir) {
    case 0: dr =  1; dc =  0; break;  /* 下滑: 上方数字下移 */
    case 1: dr = -1; dc =  0; break;  /* 上滑: 下方数字上移 */
    case 2: dr =  0; dc =  1; break;  /* 右滑: 左侧数字右移 */
    case 3: dr =  0; dc = -1; break;  /* 左滑: 右侧数字左移 */
    default: return 0;
    }

    int8_t tr = (int8_t)g_empty_r + dr;
    int8_t tc = (int8_t)g_empty_c + dc;

    if (tr < 0 || tr >= KLOTSKI_ROWS || tc < 0 || tc >= KLOTSKI_COLS)
        return 0;

    /* 交换空格与来源数字 */
    uint8_t ei = g_empty_r * KLOTSKI_COLS + g_empty_c;
    uint8_t ti = (uint8_t)tr * KLOTSKI_COLS + (uint8_t)tc;
    g_grid[ei] = g_grid[ti];
    g_grid[ti] = 0;
    g_empty_r = (uint8_t)tr;
    g_empty_c = (uint8_t)tc;
    g_moves++;

    return 1;
}

/* ================================================================
 *  打乱: 从目标排列执行N步随机移动 (保证有解)
 * ================================================================ */
static void shuffle(void)
{
    uint8_t i;
    /* 初始化为目标排列 */
    for (i = 0; i < KLOTSKI_SIZE; i++) g_grid[i] = goal_grid[i];
    find_empty();

    /* 执行100步随机移动 */
    uint8_t last_dir = 0xFF;  /* 防止来回抵消 */
    for (i = 0; i < 100; i++) {
        /* 收集合法方向 0-3 */
        uint8_t dir;
        uint8_t valid_dirs[4];
        uint8_t vc = 0;

        /* 检查4个方向(移入方向) */
        /* 下滑(dir=0):空格行<3 */  if (g_empty_r < KLOTSKI_ROWS - 1) { valid_dirs[vc++] = 0; }
        /* 上滑(dir=1):空格行>0 */  if (g_empty_r > 0)                  { valid_dirs[vc++] = 1; }
        /* 右滑(dir=2):空格列<3 */  if (g_empty_c < KLOTSKI_COLS - 1)  { valid_dirs[vc++] = 2; }
        /* 左滑(dir=3):空格列>0 */  if (g_empty_c > 0)                  { valid_dirs[vc++] = 3; }

        if (vc == 0) break;

        dir = valid_dirs[game_rand() % vc];

        /* 避免来回抵消 */
        if ((last_dir == 0 && dir == 1) || (last_dir == 1 && dir == 0) ||
            (last_dir == 2 && dir == 3) || (last_dir == 3 && dir == 2)) {
            if (vc > 1) {
                /* 换一个非反向的方向 */
                uint8_t j;
                for (j = 0; j < vc; j++) {
                    if (valid_dirs[j] != dir &&
                        !((last_dir == 0 && valid_dirs[j] == 1) ||
                          (last_dir == 1 && valid_dirs[j] == 0) ||
                          (last_dir == 2 && valid_dirs[j] == 3) ||
                          (last_dir == 3 && valid_dirs[j] == 2))) {
                        dir = valid_dirs[j];
                        break;
                    }
                }
            }
        }

        do_move(dir);
        last_dir = dir;
    }

    g_moves = 0;
    g_timer_start = GetTimerCount();
    g_time_sec = 0;
    find_empty();
}
/* ================================================================
 *  滑动方向检测
 * ================================================================ */
static uint8_t detect_swipe(int16_t dx, int16_t dy)
{
    int16_t adx = (dx < 0) ? -dx : dx;
    int16_t ady = (dy < 0) ? -dy : dy;
    if (adx < SWIPE_THRESH && ady < SWIPE_THRESH) return 0xFF;
    if (adx > ady) return (dx > 0) ? 3 : 2;   /* 左滑 / 右滑 */
    else           return (dy > 0) ? 1 : 0;    /* 下滑 / 上滑 */
}

/* ================================================================
 *  外部接口
 * ================================================================ */

void klotski_init(void)
{
#ifdef KLOTSKI_TEST_MODE
    /* 调试模式: 直接完成排列, 滑动一步即显示通关 */
    {
        uint8_t i;
        for (i = 0; i < KLOTSKI_SIZE; i++) g_grid[i] = goal_grid[i];
        find_empty();
        g_moves = 0;
        g_timer_start = GetTimerCount();
        g_time_sec = 0;
    }
#else
    shuffle();
#endif
    g_state = KLOTSKI_STATE_PLAYING;
    g_grid_dirty = 1;
    g_touch_down = 0;
    g_prev_pressed = 0;
    g_exit_menu_visible = 0;
    g_menu_cooldown = 0;
    g_restart_menu_visible = 0;
    g_restart_sliding = 0;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
}

void klotski_reset_touch(void)
{
    g_prev_pressed = 1;
    g_touch_down = 0;
    g_touch_start_x = 0;
    g_touch_start_y = 0;
    g_menu_cooldown = 0;
}

/**
 * @brief  华容道全屏重绘，用于电池闪灭恢复，避免全屏LCD_Fill闪烁
 * @note   由 klotski_update() 下一帧执行增量grid重绘以降低SPI占用
 */
void klotski_full_redraw(void)
{
    g_grid_dirty = 2;  /* 电池闪灭重绘，跳过全屏大填充 */
}

/**
 * @brief  检查退出菜单/重新开始菜单是否可见
 * @retval 1=菜单可见, 0=不可见
 */
uint8_t klotski_is_menu_visible(void)
{
    return g_exit_menu_visible || g_restart_menu_visible;
}

#define KLOTSKI_SAVE_LEN  21

uint8_t klotski_save_state(uint8_t *buf, uint8_t max_len)
{
    KlotskiState state = g_state;

    if (max_len < KLOTSKI_SAVE_LEN) return 0;
    if (state == KLOTSKI_STATE_EXIT_MENU || state == KLOTSKI_STATE_RESTART_MENU) {
        state = g_state_before_menu;
    }
    if (state > KLOTSKI_STATE_WON) state = KLOTSKI_STATE_PLAYING;

    memcpy(buf, g_grid, sizeof(g_grid));
    memcpy(buf + 16, &g_moves, sizeof(g_moves));
    memcpy(buf + 18, &g_time_sec, sizeof(g_time_sec));
    buf[20] = (uint8_t)state;
    return KLOTSKI_SAVE_LEN;
}

uint8_t klotski_restore_state(const uint8_t *buf, uint8_t len)
{
    uint8_t i;
    uint16_t seen = 0;

    if (len != KLOTSKI_SAVE_LEN || buf[20] > KLOTSKI_STATE_WON) return 0;
    for (i = 0; i < KLOTSKI_SIZE; i++) {
        if (buf[i] >= KLOTSKI_SIZE || (seen & (1U << buf[i]))) return 0;
        seen |= (uint16_t)(1U << buf[i]);
    }

    memcpy(g_grid, buf, sizeof(g_grid));
    memcpy(&g_moves, buf + 16, sizeof(g_moves));
    memcpy(&g_time_sec, buf + 18, sizeof(g_time_sec));
    g_state = (KlotskiState)buf[20];
    g_state_before_menu = g_state;
    find_empty();
    g_timer_start = GetTimerCount() - (uint32_t)g_time_sec * 1000000UL;
    g_grid_dirty = 1;
    klotski_reset_touch();

    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    klotski_update();
    return 1;
}

/* ================================================================
 *  配置驱动菜单状态机的 hook (mark_dirty / bg_redraw / on_to_exit)
 * ================================================================ */
void klotski_menu_mark_dirty(void)   /* 关闭/取消菜单后需重绘场景 */
{
    g_grid_dirty = 1;
}

/* 依据脏标记重绘菜单背景: 返回1=本帧已重绘 */
uint8_t klotski_menu_bg_redraw(void)
{
    if (!g_grid_dirty) return 0;
    if (g_grid_dirty == 2) {
        /* 电池闪灭: 仅重绘中段行(row1-2)以减少SPI冲突 */
        uint8_t r, c;
        for (r = 1; r <= 2; r++) {
            for (c = 0; c < KLOTSKI_COLS; c++) {
                uint8_t val = g_grid[r * KLOTSKI_COLS + c];
                uint16_t bg = (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR;
                draw_tile_bg(r, c, bg);
                if (val != 0) draw_tile_number(r, c, val);
            }
        }
    } else {
        draw_grid();
        draw_status();
    }
    g_grid_dirty = 0;
    return 1;
}

/* 重菜下滑打断进退出菜单后的补充处理: 置脏以便重绘背景 */
static void klotski_menu_on_to_exit(void)
{
    g_grid_dirty = 1;
}

static const GameMenuCfg g_klotski_menu_cfg = {
    KLOTSKI_STATE_PLAYING, KLOTSKI_STATE_EXIT_MENU, KLOTSKI_STATE_RESTART_MENU,
    klotski_row_types, klotski_exit_map,
    210, 10,
    164, 22,
    klotski_menu_mark_dirty, klotski_menu_bg_redraw, klotski_menu_on_to_exit
};

uint8_t klotski_update(void)
{
    uint8_t interacted = 0;

    /* 冷却倒计时 */
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 外部请求进入退出菜单 */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        g_state = KLOTSKI_STATE_EXIT_MENU;
        g_grid_dirty = 1;
    }

    /* ---- 退出菜单 / 重新开始菜单: 配置驱动统一状态机 ---- */
    if (g_state == KLOTSKI_STATE_EXIT_MENU || g_state == KLOTSKI_STATE_RESTART_MENU) {
        uint8_t out_ret;
        uint8_t cmd = game_menu_command(&g_klotski_menu_cfg, (uint8_t *)&g_state, &out_ret);
        switch (cmd) {
            case GAME_MENU_CMD_SWITCH:   return out_ret;
            case GAME_MENU_CMD_RESTART:  klotski_init(); return GAME_RET_INTERACT;
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
                                   KLOTSKI_STATE_PLAYING, KLOTSKI_STATE_EXIT_MENU))
        return 0;
    /* 游戏进行中: 上划触发重新开始菜单 */
    if (game_menu_enter_restart_drag(&ts, (uint8_t *)&g_state,
                                      KLOTSKI_STATE_PLAYING, KLOTSKI_STATE_RESTART_MENU, 210))
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
        if (game_menu_commit_exit(dy, (uint8_t *)&g_state, KLOTSKI_STATE_EXIT_MENU))
            return GAME_RET_INTERACT;

        /* 松手确认: 重新开始菜单 */
        if (game_menu_commit_restart(dy, (uint8_t *)&g_state,
                                      KLOTSKI_STATE_RESTART_MENU, 210, 10))
            return GAME_RET_INTERACT;

        /* 滑动移动数字 */
        uint8_t dir = detect_swipe(dx, dy);
        if (dir != 0xFF && g_state == KLOTSKI_STATE_PLAYING) {
            uint8_t old_r = g_empty_r, old_c = g_empty_c;
            uint8_t moved = do_move(dir);
            if (moved) {
                interacted = 1;
                Main_ClearLowBattSaveFlag();  /* 有操作变化时清除低电保存标记 */
                /* 增量绘制仅变化的两个格子 */
                {
                    uint8_t val;
                    /* 来源格(原空格位置) */
                    val = g_grid[old_r * KLOTSKI_COLS + old_c];
                    draw_tile_bg(old_r, old_c, KLOTSKI_TILE_COLOR);
                    draw_tile_number(old_r, old_c, val);
                    /* 空格(新位置) */
                    draw_tile_bg(g_empty_r, g_empty_c, KLOTSKI_EMPTY_COLOR);
                }
                draw_status();

                if (is_won()) {
                    g_state = KLOTSKI_STATE_WON;
                    g_grid_dirty = 1;  /* 触发通关画面 */
                }
            }
        }
    }

    /* ---- 计时 (仅进行中, 上限99:59) ---- */
    if (g_state == KLOTSKI_STATE_PLAYING) {
        uint32_t sec32 = (GetTimerCount() - g_timer_start) / 1000000;
        if (sec32 > 5999) sec32 = 5999;
        uint16_t sec = (uint16_t)sec32;
        if (sec != g_time_sec) {
            g_time_sec = sec;
        }
    }
    /* ---- 绘制 ---- */
    if (g_grid_dirty) {
        /* 电池闪灭恢复(g_grid_dirty==2)时跳过全屏大填充 */
        if (g_grid_dirty != 2) {
            LCD_Fill(0, KLOTSKI_GRID_Y, 239,
                     KLOTSKI_GRID_Y + KLOTSKI_ROWS * KLOTSKI_TILE_SIZE + (KLOTSKI_ROWS - 1) * KLOTSKI_GAP - 1,
                     LCD_BLACK);
        }
        draw_grid();
        draw_status();
        g_grid_dirty = 0;

        /* 通关画面 */
        if (g_state == KLOTSKI_STATE_WON) {
            uint16_t min = g_time_sec / 60;
            uint16_t sec = g_time_sec % 60;

            /* 清除中间区域避免叠字 */
            LCD_Fill(40, 122, 199, 207, LCD_BLACK);

            LCD_CS_CLR;
            /* "恭喜通关" (4个32x32汉字) */
            {
                uint16_t cx = (240 - 128) / 2;
                sok_draw_char_32(cx,       90, FlashStore_ResLoad(FLASH_STORE_ID_32_GONG, 0, FONT_CHAR_SIZE), LCD_RED);
                sok_draw_char_32(cx + 32,  90, FlashStore_ResLoad(FLASH_STORE_ID_32_XI,   0, FONT_CHAR_SIZE), LCD_RED);
                sok_draw_char_32(cx + 64,  90, FlashStore_ResLoad(FLASH_STORE_ID_32_TONG, 0, FONT_CHAR_SIZE), LCD_RED);
                sok_draw_char_32(cx + 96,  90, FlashStore_ResLoad(FLASH_STORE_ID_32_GUAN, 0, FONT_CHAR_SIZE), LCD_RED);
            }
            /* 步数 (居中16x32数字) */
            {
                uint8_t digits[8];
                uint8_t dc = uint_to_digits(g_moves, digits);
                uint16_t start_x = (240 - dc * 16) / 2;
                uint16_t y = 130;
                uint8_t i;
                for (i = 0; i < dc; i++) {
                    sok_draw_digit_16x32(start_x + (dc - 1 - i) * 16, y, digits[i], LCD_YELLOW);
                }
            }
            /* 时间 MM:SS */
            {
                uint16_t y = 170;
                /* 分钟 */
                uint16_t start_x = (240 - 56) / 2;
                if (min >= 10) {
                    sok_draw_digit_16x32(start_x,      y, min / 10, LCD_YELLOW);
                    sok_draw_digit_16x32(start_x + 16, y, min % 10, LCD_YELLOW);
                } else {
                    sok_draw_digit_16x32(start_x + 8,  y, min,      LCD_YELLOW);
                }
                /* 冒号 : (两点, 跳过LCD_Fill避免对CS干扰) */
                {
                    uint8_t k;
                    LCD_Address_Set(start_x + 31, y + 7, start_x + 33, y + 11);
                    for (k = 0; k < 15; k++) LCD_WR_DATA_16Bit(LCD_YELLOW);
                }
                {
                    uint8_t k;
                    LCD_Address_Set(start_x + 31, y + 20, start_x + 33, y + 24);
                    for (k = 0; k < 15; k++) LCD_WR_DATA_16Bit(LCD_YELLOW);
                }
                /* 秒 */
                sok_draw_digit_16x32(start_x + 40, y, sec / 10, LCD_YELLOW);
                sok_draw_digit_16x32(start_x + 56, y, sec % 10, LCD_YELLOW);
            }
            LCD_CS_SET;
        }
    }

    game_battery_icon_update(164, 22, 0);
    return interacted;
}
