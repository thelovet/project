/**
 * @file    klotski.c
 * @brief   华容道 (15-Puzzle) 游戏模块
 *          4x4 数字拼图, 触摸滑动, 步数+计时
 */
 /* 调试模式: 直接设为目标排列, 滑动一次即通关验证 */
/* #define KLOTSKI_TEST_MODE */

#include "klotski.h"
#include "tetris.h"       /* 游戏返回码 */
#include "menu.h"         /* 菜单/退出/重开 */
#include "sok_img_win.h"  /* 数字字模 */
#include "flash_store.h"   /* 字库资源从外部Flash读取 */
#include "lcd.h"    
#include "display.h"
#include "touch.h"
#include "timer.h"
#include "string.h"

/* 颜色 */
#define KLOTSKI_EMPTY_COLOR  0xE73C   /* 空格: 蓝灰色 */
#define KLOTSKI_TILE_COLOR   0x2DC5   /* 方块色: 绿色 */

/* 存档缓冲长度: 网格16 + 步数2 + 秒数2 + 状态1 = 21 */
#define KLOTSKI_SAVE_LEN  21

/* 滑动最小阈值(移出空格) */
#define SWIPE_THRESH  10

/* 游戏状态 */
static uint8_t  g_grid[KLOTSKI_SIZE];     /* 4x4: 1~15=方块编号, 0=空格 */
static uint8_t  g_empty_r, g_empty_c;     /* 空格位置 */
static uint16_t g_moves;                  /* 移动步数 */
static uint32_t g_timer_start;           /* 计时起点(ticks) */
static uint16_t g_time_sec;              /* 已用时间秒数 */
static KlotskiState g_state;
static uint8_t  g_grid_dirty;

/* 下划菜单游戏映射 (顺序: 视频/推箱子/2048/俄罗斯方块, 减当前=华容道) */
static const uint8_t klotski_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_SOKOBAN, EXIT_RET_D2048, EXIT_RET_TETRIS, EXIT_RET_FISH
};

/* 退出菜单行类型 (klotski专属) */
static const uint8_t klotski_row_types[5] = {
    MENU_ROW_SHIPIN, MENU_ROW_TUI, MENU_ROW_2048,
    MENU_ROW_TETRIS, MENU_ROW_FISH
};

/* 目标排列 */
static const uint8_t goal_grid[KLOTSKI_SIZE] = {
    1,  2,  3,  4,
    5,  6,  7,  8,
    9,  10, 11, 12,
    13, 14, 15, 0
};

/**
 * @brief  列号转格子左上角 X 坐标。
 * @param  col 列号(0~3)
 * @return 该列左上角 X 像素
 */
static uint16_t tile_x(uint8_t col) {
    return KLOTSKI_GRID_X + col * (KLOTSKI_TILE_SIZE + KLOTSKI_GAP);
}

/**
 * @brief  行号转格子顶部 Y 坐标。
 * @param  row 行号(0~3)
 * @return 该行顶部 Y 像素
 */
static uint16_t tile_y(uint8_t row) {
    return KLOTSKI_GRID_Y + row * (KLOTSKI_TILE_SIZE + KLOTSKI_GAP);
}

/**
 * @brief  绘制格子背景
 * @param  row   行号(0~3)
 * @param  col   列号(0~3)
 * @param  color 背景色
 */
static void draw_tile_bg(uint8_t row, uint8_t col, uint16_t color) {
    uint16_t x = tile_x(col);
    uint16_t y = tile_y(row);
    /* ★ 同 2048 的坑: LCD_Fill 的 end 参数不含, 传 -1 会漏掉格子最右列/最底行,
       数字消失后那几列会留下不消失的残留(见 d2048.c draw_tile_bg 的说明)。 */
    LCD_Fill(x, y, x + KLOTSKI_TILE_SIZE, y + KLOTSKI_TILE_SIZE, color);
}

/**
 * @brief  绘制格子中的数字 (1~2位用16x32, 3位缩放)
 * @param  row 行号(0~3)
 * @param  col 列号(0~3)
 * @param  val 数值
 */
static void draw_tile_number(uint8_t row, uint8_t col, uint8_t val) {
    /* 华容道恒白字; 居中排版的绘制本体已合并到 menu.c 的 game_grid_draw_number */
    game_grid_draw_number(tile_x(col), tile_y(row), KLOTSKI_TILE_SIZE,
                          val, LCD_WHITE, KLOTSKI_DIGIT_OFFY, 1);
}

/**
 * @brief  绘制整个网格
 */
static void draw_grid(void) {
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

/**
 * @brief  绘制状态 (步数显示)
 */
static void draw_status(void) {
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

/**
 * @brief  查找空格位置
 */
static void find_empty(void) {
    uint8_t i;
    for (i = 0; i < KLOTSKI_SIZE; i++) {
        if (g_grid[i] == 0) {
            g_empty_r = i / KLOTSKI_COLS;
            g_empty_c = i % KLOTSKI_COLS;
            return;
        }
    }
}

/**
 * @brief  检查是否通关
 * @return 1=已通关, 0=未通关
 */
static uint8_t is_won(void) {
    uint8_t i;
    for (i = 0; i < KLOTSKI_SIZE; i++) {
        if (g_grid[i] != goal_grid[i]) return 0;
    }
    return 1;
}

/**
 * @brief  移动: 从用户视角, 滑动方向数字移入空格。
 * @param  dir 0=下(用户向上滑, 下方数字上移), 1=上, 2=右, 3=左
 * @return 1=移动有效, 0=无效
 */
static uint8_t do_move(uint8_t dir) {
    int8_t dr = 0, dc = 0;
    /* 数字移入空格的方向 -> 数字来源(反向) */
    switch (dir) {
    case 0: dr = 1; dc = 0; break;  /* 下滑: 上方数字下移 */
    case 1: dr = -1; dc = 0; break;  /* 上滑: 下方数字上移 */
    case 2: dr = 0; dc = 1; break;  /* 右滑: 左侧数字右移 */
    case 3: dr = 0; dc = -1; break;  /* 左滑: 右侧数字左移 */
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

#ifndef KLOTSKI_TEST_MODE
/**
 * @brief  打乱: 从目标排列执行N步随机移动 (保证有解)
 */
static void shuffle(void) {
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
        /* 上滑(dir=1):空格行>0 */  if (g_empty_r > 0) { valid_dirs[vc++] = 1; }
        /* 右滑(dir=2):空格列<3 */  if (g_empty_c < KLOTSKI_COLS - 1) { valid_dirs[vc++] = 2; }
        /* 左滑(dir=3):空格列>0 */  if (g_empty_c > 0) { valid_dirs[vc++] = 3; }

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
#endif /* !KLOTSKI_TEST_MODE */
/**
 * @brief  滑动方向检测
 * @param  dx X位移
 * @param  dy Y位移
 * @return 0=下 1=上 2=右 3=左, 0xFF=无效
 */
static uint8_t detect_swipe(int16_t dx, int16_t dy) {
    int16_t adx = (dx < 0) ? -dx : dx;
    int16_t ady = (dy < 0) ? -dy : dy;
    if (adx < SWIPE_THRESH && ady < SWIPE_THRESH) return 0xFF;
    if (adx > ady) return (dx > 0) ? 3 : 2;   /* 左滑 / 右滑 */
    else           return (dy > 0) ? 1 : 0;    /* 下滑 / 上滑 */
}

/**
 * @brief  初始化华容道: 打乱/复位状态。
 */
void klotski_init(void) {
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
/**
 * @brief  华容道全屏重绘，用于电池闪灭恢复，避免全屏LCD_Fill闪烁
 * @note   由 klotski_update() 下一帧执行增量grid重绘以降低SPI占用
 */
void klotski_full_redraw(void) {
    g_grid_dirty = 2;  /* 电池闪灭重绘，跳过全屏大填充 */
}

/* klotski_redraw_battery_rows 定义在本文件下方, 此处前向声明 */
static void klotski_redraw_battery_rows(void);
static void klotski_redraw_win_rows(int16_t menu_top, int16_t menu_h);
static void klotski_restore_battery_rows(int16_t menu_top, int16_t menu_h);

/**
 * @brief  恢复电池覆盖的网格行(框外露出部分, 框内由菜单框盖住)。
 * @param  menu_top 菜单框顶 Y
 * @param  menu_h   菜单框高
 * @note   只重绘退出菜单框外露出的电池区: 按行判断, 行完全被菜单框 y 覆盖(框内)则只补
 *         恒框外的凸点列(3)(横跨菜单右边框), 否则整行重绘(含凸点)。框内主体由菜单框盖住,
 *         避免先棋盘后菜单闪一帧。
 */
static void klotski_restore_battery_rows(int16_t menu_top, int16_t menu_h) {
    int16_t menu_bot = menu_top + menu_h;
    uint8_t r;
    for (r = 1; r <= 2; r++) {
        uint16_t ty = tile_y(r);
        uint16_t tb = ty + (uint16_t)KLOTSKI_TILE_SIZE - 1;
        uint8_t c;
        if ((int16_t)ty >= menu_top && (int16_t)tb <= menu_bot) {
            uint16_t val = g_grid[r * KLOTSKI_COLS + 3];
            draw_tile_bg(r, 3, (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR);
            if (val != 0) draw_tile_number(r, 3, val);
        }
        else {
            for (c = 0; c < KLOTSKI_COLS; c++) {
                uint16_t val = g_grid[r * KLOTSKI_COLS + c];
                draw_tile_bg(r, c, (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR);
                if (val != 0) draw_tile_number(r, c, val);
            }
        }
    }
}

/**
 * @brief  绘制通关画面("恭喜通关"+步数+时间)。
 * @note   菜单态时 g_state 已是 EXIT/RESTART, 用 g_state_before_menu 判定; 纯游戏态用 g_state.
 *         供 klotski_update 正常绘制 与 低电/菜单背景重绘复用; 含清中间区, 防叠字.
 */
static void klotski_draw_win_title(void) {
    /* "恭喜通关" / "You Win" —— 居中由 sok_draw_big_text 内部算(中英宽度不同) */
    sok_draw_big_text(SOK_BIGTEXT_WIN, 90, LCD_RED);
}

static void klotski_draw_win_steps(void) {
    uint8_t digits[8];
    uint8_t dc = uint_to_digits(g_moves, digits);
    uint16_t start_x = (240 - dc * 16) / 2;
    uint16_t y = 130;
    uint8_t i;
    LCD_CS_CLR;
    for (i = 0; i < dc; i++) {
        sok_draw_digit_16x32(start_x + (dc - 1 - i) * 16, y, digits[i], LCD_YELLOW);
    }
    LCD_CS_SET;
}

static void klotski_draw_win_panel(void) {
    uint16_t min = g_time_sec / 60;
    uint16_t sec = g_time_sec % 60;
    /* 步数 */
    klotski_draw_win_steps();
    /* 时间 MM:SS */
    {
        uint16_t y = 170;
        uint16_t start_x = (240 - 56) / 2;
        LCD_CS_CLR;
        if (min >= 10) {
            sok_draw_digit_16x32(start_x, y, min / 10, LCD_YELLOW);
            sok_draw_digit_16x32(start_x + 16, y, min % 10, LCD_YELLOW);
        }
        else {
            sok_draw_digit_16x32(start_x + 8, y, min, LCD_YELLOW);
        }
        /* 冒号 : */
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
        sok_draw_digit_16x32(start_x + 40, y, sec / 10, LCD_YELLOW);
        sok_draw_digit_16x32(start_x + 56, y, sec % 10, LCD_YELLOW);
        LCD_CS_SET;
    }
}

static void klotski_draw_win_screen(uint8_t icon_only) {
    uint8_t rstate = (g_state == KLOTSKI_STATE_EXIT_MENU || g_state == KLOTSKI_STATE_RESTART_MENU)
        ? g_state_before_menu : g_state;
    if (rstate != KLOTSKI_STATE_WON) return;

    /* 清除中间区域避免叠字; icon_only(通关画面低电恢复): 面板已黑, 只重画文字,
       不重填面板(否则步数/时间文字先被刷掉再重画, 闪一帧) */
    if (!icon_only) {
        LCD_Fill(40, 122, 199, 207, LCD_BLACK);
    }

    klotski_draw_win_title();
    klotski_draw_win_panel();
}

/**
 * @brief  低电图标灭段后恢复被其覆盖的底层内容
 * @note   参照 d2048_lowbatt_restore(推箱子样式).
 *         - 电池凸点(x=195..210, y=104..136)在菜单框外, 压在网格格(1,3)/(2,3): 先重画.
 *         - 下滑(退出)菜单: 只重画不透明菜单框(盖住菜单内电池主体/感叹号) => 不重绘
 *           网格, 否则网格行闪一帧.
 *         - 重开菜单: 只恢复菜单上方/外侧的图标细条带, 再画菜单框; 不重绘延伸到
 *           菜单后面的网格行(曾导致游戏图先于菜单显示一帧).
 *         - 纯游戏: 只重绘电池覆盖的网格区域(rows1-2).
 */
void klotski_lowbatt_restore(void) {
    LCD_ClearLowBatteryOverlay();   /* 图标边框+凸点+感叹号 红→黑 */

    if (g_exit_menu_visible) {
        /* 退出菜单: 框内主体由菜单框直接盖住, 只恢复未盖住的行 + 恒框外的凸点列(1,3)/(2,3).
           通关态:
           - 拖拽/滑入中(菜单未全盖): 重绘面板上方可见网格 rows0-1 + 叠回文字;
           - 静止显示(菜单全盖 y=5~235): 用菜单感知的 klotski_restore_battery_rows, 因
             rows1-2 全被菜单盖住 → 只画凸点列, 不会把游戏画面画到菜单上闪.
           非通关态: 正常整行重绘(游戏画面本就是网格). */
        if (g_state_before_menu == KLOTSKI_STATE_WON) {
            /* 通关态: 菜单感知 win_rows(被菜单盖住行只画凸点右缘, 跨顶行只画露出部分),
               不把棋盘画到菜单上. 只画**菜单上方露出**的标题/面板文字, 被菜单盖住的不画(否则
               画到菜单上再被盖住会闪). */
            klotski_redraw_win_rows(g_exit_drag_y, MENU_H);
            /* 标题(恭喜通关 y=90~121)/步数(y=130~161)被电池盖黑, 在菜单上方或下方**完全露出**时重绘,
               清除黑残留(不画被菜单盖住的部分, 避免闪). 拖拽初始菜单从顶部滑入(menu_top 为负)时,
               文字在菜单下方暴露, 也须重绘. */
            if (g_exit_drag_y >= 121 || g_exit_drag_y <= -140) klotski_draw_win_title();
            if (g_exit_drag_y >= 161 || g_exit_drag_y <= -100) klotski_draw_win_steps();
        } else {
            klotski_restore_battery_rows(g_exit_drag_y, MENU_H);
        }
        menu_draw_nrow_frame_only(g_exit_drag_y, klotski_row_types, 5);
    }
    else if (g_restart_menu_visible) {
        /* 重开菜单: 恢复菜单上方电池区残留; 通关态用面板外恢复+叠回文字, 非通关态整行重绘. */
        if (g_state_before_menu == KLOTSKI_STATE_WON) {
            klotski_redraw_win_rows(240, 0);   /* 上滑(重开)菜单保持完整重绘: 面板上可见网格 rows0-1 */
            klotski_draw_win_screen(1);   /* 文字only, 不重填面板 */
        } else {
            klotski_redraw_battery_rows();
        }
        menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
    }
    else if (g_settings_from_game) {
        /* 设置模式(二级菜单条带 x=45..195 已盖住电池主体/感叹号): 只重绘凸点格子(1,3)/(2,3),
           不重绘 rows1-2 -- 否则先画游戏(rows1-2)再画菜单框会闪一帧(同 2048 坑).
           凸点在菜单框外(x=195..210), 须由游戏网格格恢复; 菜单框内部分由条带覆盖. */
        uint8_t r;
        for (r = 1; r <= 2; r++) {
            uint8_t c = 3;
            uint8_t val = g_grid[r * KLOTSKI_COLS + c];
            uint16_t bg = (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR;
            draw_tile_bg(r, c, bg);
            if (val != 0) draw_tile_number(r, c, val);
        }
    }
    else {
        /* 纯游戏(无菜单框): 通关态只重绘电池区(row1-2)+通关画面, 避免整屏网格重绘闪一帧;
           其余保持原设脏, 下一帧 klotski_update 重绘电池覆盖行 */
        if (g_state == KLOTSKI_STATE_WON) {
            /* 通关画面: 只重绘面板外的电池残留(rows0-1+面板右侧凸点列), 不重绘面板区
               (row2-3)网格 -- 面板区(步数/时间)保持纯黑, 避免游戏画面浮现在黑色区/闪. */
            klotski_redraw_win_rows(240, 0);
            klotski_draw_win_screen(1);   /* 文字only: 面板已黑, 不重填面板 */
        } else {
            klotski_full_redraw();
        }
    }
}

/**
 * @brief  序列化华容道当前状态到存档缓冲。
 * @param  buf     输出缓冲
 * @param  max_len 缓冲上限
 * @return 写入字节数
 */
uint8_t klotski_save_state(uint8_t* buf, uint8_t max_len) {
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
/**
 * @brief  从存档缓冲恢复华容道状态并重绘。
 * @param  buf 存档缓冲
 * @param  len 长度
 * @return 1=成功, 0=失败
 */
uint8_t klotski_restore_state(const uint8_t* buf, uint8_t len) {
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
    game_reset_touch_common();

    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    klotski_update();
    return 1;
}

/**
 * @brief  关闭/取消菜单后标记需重绘场景。
 */
void klotski_menu_mark_dirty(void) {
    g_grid_dirty = 1;
}

/**
 * @brief  重绘电池覆盖的网格行(rows1-2, 含凸点)。
 * @note   供菜单背景重绘(g_grid_dirty==2)与 klotski_lowbatt_restore(立即区域恢复)共用。
 */
static void klotski_redraw_battery_rows(void) {
    uint8_t r, c;
    for (r = 1; r <= 2; r++) {
        for (c = 0; c < KLOTSKI_COLS; c++) {
            uint8_t val = g_grid[r * KLOTSKI_COLS + c];
            uint16_t bg = (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR;
            draw_tile_bg(r, c, bg);
            if (val != 0) draw_tile_number(r, c, val);
        }
    }
}

/**
 * @brief  绘制行 r 格子: 只显示 y > y_min 的露出部分(底格裁剪, 数字全画)。
 */
static void klotski_draw_row_clip_bottom(uint8_t r, int16_t y_min) {
    uint8_t c;
    uint16_t ty = tile_y(r);
    uint16_t tb = ty + (uint16_t)KLOTSKI_TILE_SIZE - 1;
    uint16_t y_start = ((int16_t)ty > y_min) ? ty : (uint16_t)y_min;
    if ((uint16_t)y_start > tb) return;
    for (c = 0; c < KLOTSKI_COLS; c++) {
        uint16_t val = g_grid[r * KLOTSKI_COLS + c];
        uint16_t bg = (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR;
        LCD_Fill(tile_x(c), y_start, tile_x(c) + KLOTSKI_TILE_SIZE - 1, tb, bg);
        if (val != 0) draw_tile_number(r, c, val);
    }
}

/* 绘制行 r 格子: 只显示 y < y_max 的露出部分(底格裁剪)。
 * @note   退出菜单拖拽时菜单盖住部分格子, 只重绘菜单上方露出部分, 不把游戏画面画到菜单上闪。
 */
static void klotski_draw_row_clip(uint8_t r, int16_t y_max) {
    uint8_t c;
    uint16_t ty = tile_y(r);
    uint16_t tb = ty + (uint16_t)KLOTSKI_TILE_SIZE - 1;
    uint16_t y_end = ((int16_t)tb < y_max) ? tb : (uint16_t)(y_max - 1);
    if ((int16_t)ty > y_end) return;
    for (c = 0; c < KLOTSKI_COLS; c++) {
        uint16_t val = g_grid[r * KLOTSKI_COLS + c];
        uint16_t bg = (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR;
        LCD_Fill(tile_x(c), ty, tile_x(c) + KLOTSKI_TILE_SIZE - 1, y_end, bg);
        if (val != 0) draw_tile_number(r, c, val);
    }
}

/**
 * @brief  通关画面低电恢复/菜单清除恢复: 只重绘可见区域。
 * @param  menu_top 菜单框顶 Y (纯游戏无菜单传 240, 菜单顶全在屏幕外)
 * @param  menu_h   菜单框高 (纯游戏无菜单传 0)
 * @note   通关态黑色面板(y=122~207)里是纯黑+步数/时间文字, 不能重绘其下网格(否则
 *         游戏画面浮现在黑色区/闪一帧)。通关画面可见网格 = 面板(y=122)上方 rows0-1;
 *         行被菜单**完全盖住**则只画凸点右缘(x=196..206, 菜单右缘外); 行**跨菜单顶**则
 *         只画露出部分(裁剪到 y<menu_top); 整行露出才完整重绘。
 */
static void klotski_redraw_win_rows(int16_t menu_top, int16_t menu_h) {
    uint8_t r, c;
    int16_t menu_bot = menu_top + menu_h;
    /* 面板上方网格 rows0-1(y=40..124): 电池外框/凸点/感叹号 + 菜单清除/覆盖残留 */
    for (r = 0; r <= 1; r++) {
        uint16_t ty = tile_y(r);
        uint16_t tb = ty + (uint16_t)KLOTSKI_TILE_SIZE - 1;
        if ((int16_t)ty >= menu_bot) {
            /* 整行在菜单下方(露出): 完整重绘 (拖拽初始菜单从顶部滑入, 底在行上方) */
            for (c = 0; c < KLOTSKI_COLS; c++) {
                uint16_t val = g_grid[r * KLOTSKI_COLS + c];
                draw_tile_bg(r, c, (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR);
                if (val != 0) draw_tile_number(r, c, val);
            }
        } else if ((int16_t)tb <= menu_top) {
            /* 整行在菜单上方(露出): 完整重绘 */
            for (c = 0; c < KLOTSKI_COLS; c++) {
                uint16_t val = g_grid[r * KLOTSKI_COLS + c];
                draw_tile_bg(r, c, (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR);
                if (val != 0) draw_tile_number(r, c, val);
            }
        } else if ((int16_t)ty >= menu_top && (int16_t)tb <= menu_bot) {
            /* 整行被菜单盖住: 只画凸点右缘(x=196..206, 菜单右缘外) */
            uint16_t val = g_grid[r * KLOTSKI_COLS + 3];
            LCD_Fill(196, ty, 206, tb, (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR);
        } else if ((int16_t)ty < menu_top) {
            /* 行跨菜单顶: 只画菜单上方露出部分 */
            klotski_draw_row_clip(r, menu_top);
        } else {
            /* 行跨菜单底: 只画菜单下方露出部分 */
            klotski_draw_row_clip_bottom(r, menu_bot);
        }
    }
    /* 面板顶(y=122)盖住行1下缘, 面板保持纯黑 */
    LCD_Fill(40, 122, 199, 124, LCD_BLACK);
    /* 面板右侧凸点列(row2 col3 右缘 x=200..206, 面板外): 重绘 tile 右缘底 */
    {
        uint16_t val = g_grid[2 * KLOTSKI_COLS + 3];
        LCD_Fill(200, tile_y(2), 206, tile_y(2) + KLOTSKI_TILE_SIZE - 1,
                 (val == 0) ? KLOTSKI_EMPTY_COLOR : KLOTSKI_TILE_COLOR);
    }
}

/**
 * @brief  依据脏标记重绘菜单背景(网格/状态)。
 * @return 1=已重绘, 0=无脏
 */
uint8_t klotski_menu_bg_redraw(void) {
    if (!g_grid_dirty) return 0;
    if (g_grid_dirty == 2) {
        /* 电池闪灭: 仅重绘中段行(row1-2)以减少SPI冲突 */
        klotski_redraw_battery_rows();
        klotski_draw_win_screen(0);   /* 电池闪灭恢复: 面板填充盖住恢复的网格行 */
    }
    else {
        /* 菜单关闭返回: 通关态只重绘通关画面(顶部状态栏 + 面板上方可见网格 rows0-1 + 文字),
           不 draw_grid 整行重绘(rows2-3 是面板下网格, 画出会闪一帧);
           非通关态整行重绘(游戏画面本就是棋盘, 不会突兀). */
        if (g_state_before_menu == KLOTSKI_STATE_WON) {
            draw_status();
            klotski_redraw_win_rows(240, 0);
            klotski_draw_win_screen(1);   /* 文字only: 面板区已被清为黑, 不重填 */
        } else {
            draw_grid();
            draw_status();
            klotski_draw_win_screen(0);   /* 非通关态此调用为no-op */
        }
    }
    g_grid_dirty = 0;
    return 1;
}

/**
 * @brief  重菜下滑打断进退出菜单后的补充处理(置脏重绘)。
 */
static void klotski_menu_on_to_exit(void) {
    g_grid_dirty = 1;
}

static const GameMenuCfg g_klotski_menu_cfg = {
    KLOTSKI_STATE_PLAYING, KLOTSKI_STATE_EXIT_MENU, KLOTSKI_STATE_RESTART_MENU,
    klotski_row_types, klotski_exit_map,
    210, 10,
    164, 22,
    klotski_menu_mark_dirty, klotski_menu_bg_redraw, klotski_menu_on_to_exit
};
/**
 * @brief  华容道每帧更新: 处理触摸、菜单、步数与渲染。
 * @return GAME_RET_* 返回命令
 */
uint8_t klotski_update(void) {
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
        uint8_t cmd = game_menu_command(&g_klotski_menu_cfg, (uint8_t*)&g_state, &out_ret);
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

    /* 游戏进行中/通关: 下滑触发退出菜单拖拽 (通关态也可跟手弹菜单) */
    if (game_menu_enter_exit_drag(&ts, (uint8_t*)&g_state,
        g_state, KLOTSKI_STATE_EXIT_MENU))
        return 0;
    /* 游戏进行中/通关: 上划触发重新开始菜单 */
    if (game_menu_enter_restart_drag(&ts, (uint8_t*)&g_state,
        g_state, KLOTSKI_STATE_RESTART_MENU, 210))
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
        if (game_menu_commit_exit(dy, (uint8_t*)&g_state, KLOTSKI_STATE_EXIT_MENU))
            return GAME_RET_INTERACT;

        /* 松手确认: 重新开始菜单 */
        if (game_menu_commit_restart(dy, (uint8_t*)&g_state,
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

                if (is_won()
#ifdef KLOTSKI_TEST_MODE
                    || 1   /* 调试: 华容道任意滑动一次即通关, 测"恭喜通关" */
#endif
                   ) {
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
        klotski_draw_win_screen(0);   /* 整屏/完整重绘 */
    }

    game_battery_icon_update(164, 22, 0);
    return interacted;
}

#ifdef SIM_BUILD
/* ---- sim-only: report klotski's playing-state touch regions -------------- */
int klotski_sim_touch_rects(void)
{
    sim_touch_add(KLOTSKI_GRID_X, KLOTSKI_GRID_Y,
                  KLOTSKI_GRID_X + KLOTSKI_GRID_W, KLOTSKI_GRID_Y + KLOTSKI_GRID_W,
                  SIM_TOUCH_SWIPE);                             /* swipe the board */
    sim_touch_add(0, 0, 240, MENU_SWIPE_START_Y, SIM_TOUCH_BAND);/* top: exit menu */
    sim_touch_add(0, 211, 240, 240, SIM_TOUCH_BAND);             /* y>210: restart menu */
    return 1;
}

/* ---- sim-only: 一键通关(曹操归位), 真机不编 ---- */
void klotski_sim_force_win(void)
{
    g_state = KLOTSKI_STATE_WON;
    klotski_draw_win_screen(0);
}
#endif