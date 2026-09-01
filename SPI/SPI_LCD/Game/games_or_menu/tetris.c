/**
 * @file    tetris.c
 * @brief   俄罗斯方块游戏模块
 *          10×20 标准棋盘, 7种方块, 触摸手势控制
 */

#include "tetris.h"
#include "menu.h"
#include "sok_img_win.h"
#include "flash_store.h"   /* 字库资源从外部Flash读取 */
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "debug.h"
#include "string.h"

/* ================================================================
 *  棋盘参数
 * ================================================================ */
#define BOARD_COLS     13          /* 棋盘列数 */
#define BOARD_ROWS     19          /* 棋盘行数 */
#define CELL_SIZE      10          /* 每格像素 */
#define BOARD_X        50          /* 棋盘左上角X (居中偏左, 右侧留给侧边栏) */
#define BOARD_Y        20          /* 棋盘左上角Y (上方留空) */
#define BOARD_W        (BOARD_COLS * CELL_SIZE)   /* 110 */
#define BOARD_H        (BOARD_ROWS * CELL_SIZE)   /* 220 */

/* 预览区域 (棋盘右侧) */
#define PREVIEW_X      190         /* 预览区域X坐标 */
#define PREVIEW_Y       110         /* 预览区域Y坐标 (分数下方) */
#define PREVIEW_CELL    8          /* 预览格像素大小 */

/* 下落速度 (帧数间隔, 越大越慢, 60fps) */
#define DROP_INTERVAL_INIT  80     /* 初始下落间隔 */
#define DROP_INTERVAL_MIN    8     /* 最快下落间隔 (难度上限) */
#define DROP_INTERVAL_HARD   3     /* 硬降间隔 (手动快速下落) */
#define DROP_ACCEL_STEP      5     /* 每消N行加速的帧数 */
#define FRAME_DELAY_MS      5      /* 每帧延时(ms), ~60fps */
#define LOCK_DELAY_FRAMES  60      /* 落地锁定延迟帧数 (~1s@60fps) */

/* 格子颜色 */
#define BOARD_BG       0x18E3      /* 深灰绿 */
#define BOARD_GRID     0x4228      /* 网格线 (中灰) */

/* 网格线宽度 (像素) */
#define GRID_WIDTH     2
#define BOARD_EMPTY    0x0861      /* 空格底色 */

/* 方块颜色 */
#define CLR_I          0x07FF      /* 青色 */
#define CLR_O          0xFFE0      /* 黄色 */
#define CLR_T          0x8010      /* 紫色 */
#define CLR_S          0x07E0      /* 绿色 */
#define CLR_Z          0xF800      /* 红色 */
#define CLR_J          0x001F      /* 蓝色 */
#define CLR_L          0xFD20      /* 橙色 */

static uint16_t piece_colors[7] = {
    CLR_I, CLR_O, CLR_T, CLR_S, CLR_Z, CLR_J, CLR_L
};

/* ================================================================
 *  方块定义: 7种 × 4旋转 × 4行(每行4位=列0~3, LSB=列0)
 *  用1D数组模拟3D数组, 每行用一个8位数的低4位表示
 *  访问: PIECES[piece*16 + rotation*4 + row]
 * ================================================================ */
static uint8_t PIECES[7 * 4 * 4] = {
    /* I 0度 */  0x00,0x0F,0x00,0x00, /* I 90度 */ 0x02,0x02,0x02,0x02,
    /* I 180度*/ 0x00,0x00,0x0F,0x00, /* I 270度*/ 0x04,0x04,0x04,0x04,
    /* O 0度 */  0x06,0x06,0x00,0x00, /* O 90度 */ 0x06,0x06,0x00,0x00,
    /* O 180度*/ 0x06,0x06,0x00,0x00, /* O 270度*/ 0x06,0x06,0x00,0x00,
    /* T 0度 */  0x02,0x07,0x00,0x00, /* T 90度 */ 0x02,0x06,0x02,0x00,
    /* T 180度*/ 0x00,0x07,0x02,0x00, /* T 270度*/ 0x02,0x03,0x02,0x00,
    /* S 0度 */  0x06,0x03,0x00,0x00, /* S 90度 */ 0x02,0x06,0x04,0x00,
    /* S 180度*/ 0x00,0x06,0x03,0x00, /* S 270度*/ 0x01,0x03,0x02,0x00,
    /* Z 0度 */  0x03,0x06,0x00,0x00, /* Z 90度 */ 0x04,0x06,0x02,0x00,
    /* Z 180度*/ 0x00,0x03,0x06,0x00, /* Z 270度*/ 0x02,0x03,0x01,0x00,
    /* J 0度 */  0x01,0x07,0x00,0x00, /* J 90度 */ 0x06,0x02,0x02,0x00,
    /* J 180度*/ 0x00,0x07,0x04,0x00, /* J 270度*/ 0x02,0x02,0x03,0x00,
    /* L 0度 */  0x04,0x07,0x00,0x00, /* L 90度 */ 0x02,0x02,0x06,0x00,
    /* L 180度*/ 0x00,0x07,0x01,0x00, /* L 270度*/ 0x03,0x02,0x02,0x00,
};

/* 辅助宏: 将 piece/rot/row 转为1D索引 */
#define PIECE_IDX(piece, rot, row)  ((piece) * 16 + (rot) * 4 + (row))

/* ================================================================
 *  游戏状态
 * ================================================================ */
static uint16_t g_board[BOARD_ROWS][BOARD_COLS];  /* 0=空, 1~7=颜色索引 */
static uint32_t g_score;
static uint32_t g_lines;
static uint16_t g_level;
static TetrisState g_state;
static uint8_t  g_board_dirty;     /* 1=增量更新, 2=全量重绘 */
static uint8_t  g_preview_dirty;   /* 预览需要更新时才置1 */
static uint8_t  g_sidebar_dirty;   /* 侧边栏需要更新 */
static uint32_t g_prev_score;      /* 上次绘制的分数 */
static uint32_t g_prev_lines;      /* 上次绘制的行数 */
static uint8_t  g_gameover_drawn;  /* 结束画面已绘制 */

/* 当前方块 */
static uint8_t  g_piece_type;      /* 0~6 */
static uint8_t  g_piece_rot;       /* 0~3 */
static int8_t   g_piece_x;         /* 列 (0~9) */
static int8_t   g_piece_y;         /* 行 (0~19) */

/* 上一帧方块位置 (增量擦除用) */
static int8_t   g_prev_px, g_prev_py;
static uint8_t  g_prev_type, g_prev_rot;

/* 投影方块 (显示落点) */
static int8_t   g_ghost_y;         /* 投影Y坐标 */
static int8_t   g_prev_ghost_y;    /* 上一帧投影Y (增量擦除用) */

/* 下一个方块 */
static uint8_t  g_next_type;

/* 自动下落 */
static uint16_t g_drop_timer;
static uint16_t g_drop_interval;   /* 帧数间隔 */
static uint16_t g_saved_interval;  /* 加速前原始速度 */
static uint8_t  g_lock_delay;     /* 落地锁定延迟计时器 */

/* 触摸手势 */
static uint16_t g_touch_start_x;
static uint8_t  g_prev_pressed;
static uint8_t  g_wait_touch_release;

/* 长按重复 */
#define HOLD_REPEAT_START  30    /* 按住开始重复的帧数 */
#define HOLD_REPEAT_RATE    10    /* 重复间隔帧数 */
static uint8_t  g_hold_zone;     /* 0=无, 1=左, 2=右 */
static uint8_t  g_hold_timer;    /* 重复计时器 */

/* 消行动画 */
#define CLEAR_FLASH_FRAMES  4     /* 闪烁切换次数 (2白+2正常 = 2次白闪) */
#define FLASH_HOLD          8     /* 每次切换保持帧数 (~40ms@60fps) */
static uint8_t  g_clear_flash;    /* 剩余切换次数, 0=无动画 */
static uint8_t  g_flash_hold;     /* 当前状态剩余帧数 */
static uint32_t g_clear_mask;     /* 要消除的行位掩码 */

/* 退出菜单 (视频/推箱子/2048/华容道/返回, 减当前=俄罗斯方块) */
static const uint8_t tetris_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_SOKOBAN, EXIT_RET_D2048, EXIT_RET_KLOTSKI, EXIT_RET_FISH
};

/* 退出菜单行类型 (供 menu_draw_nrow_frame_only / game_menu_drag_poll 使用) */
static const uint8_t tetris_row_types[6] = {
    MENU_ROW_SHIPIN, MENU_ROW_TUI, MENU_ROW_2048, MENU_ROW_KLOTSKI, MENU_ROW_FISH, MENU_ROW_FANHUI
};

#define SWIPE_THRESH  25
#define TAP_THRESH    8    /* 小于此距离视为点击(旋转) */

/**
 * @brief  碰撞检测: 方块在指定位置是否与棋盘/边界冲突
 * @param  type  方块类型 (0~6)
 * @param  rot   旋转状态 (0~3)
 * @param  px    列坐标 (0~12)
 * @param  py    行坐标 (-1~18)
 * @retval 1=冲突, 0=无冲突
 */
static uint8_t collides(uint8_t type, uint8_t rot, int8_t px, int8_t py)
{
    uint8_t r;
    for (r = 0; r < 4; r++) {
        uint8_t row_bits = PIECES[PIECE_IDX(type, rot, r)];
        uint8_t c;
        for (c = 0; c < 4; c++) {
            if (row_bits & (1 << c)) {
                int8_t bx = px + c;
                int8_t by = py + r;
                if (bx < 0 || bx >= BOARD_COLS || by >= BOARD_ROWS) return 1;
                if (by >= 0 && g_board[by][bx] != 0) return 1;
            }
        }
    }
    return 0;
}

/**
 * @brief  将当前活动方块锁定到棋盘, 将g_board对应格写入方块颜色
 */
static void lock_piece(void)
{
    uint8_t r, c;
    for (r = 0; r < 4; r++) {
        uint8_t row_bits = PIECES[PIECE_IDX(g_piece_type, g_piece_rot, r)];
        for (c = 0; c < 4; c++) {
            if (row_bits & (1 << c)) {
                int8_t by = g_piece_y + r;
                int8_t bx = g_piece_x + c;
                if (by >= 0 && by < BOARD_ROWS && bx >= 0 && bx < BOARD_COLS) {
                    g_board[by][bx] = g_piece_type + 1;  /* 1~7 */
                }
            }
        }
    }
}

/**
 * @brief  消行检查: 找出满行并下移上方行, 返回消除行数
 * @return 消除的行数
 */
static uint8_t clear_lines(void)
{
    uint8_t cleared = 0;
    int8_t r;
    for (r = BOARD_ROWS - 1; r >= 0; r--) {
        uint8_t full = 1;
        uint8_t c;
        for (c = 0; c < BOARD_COLS; c++) {
            if (g_board[r][c] == 0) { full = 0; break; }
        }
        if (full) {
            /* 把上方行全部下移 */
            int8_t sr;
            for (sr = r; sr > 0; sr--) {
                uint8_t sc;
                for (sc = 0; sc < BOARD_COLS; sc++) {
                    g_board[sr][sc] = g_board[sr - 1][sc];
                }
            }
            /* 第0行清空 */
            {
                uint8_t sc;
                for (sc = 0; sc < BOARD_COLS; sc++) g_board[0][sc] = 0;
            }
            cleared++;
            r++;  /* 重新检查当前行 */
        }
    }
    return cleared;
}

/**
 * @brief  生成新方块: 将预览方块变为当前, 随机生成下一个预览, 从顶部中间下落
 */
static void spawn_piece(void)
{
    g_piece_type = g_next_type;
    g_next_type = game_rand() % 7;
    g_piece_rot = 0;
    g_piece_x = 3;   /* 从中间列开始 */
    g_piece_y = -1;  /* 从屏幕顶部外开始 */
    g_preview_dirty = 1;

    if (collides(g_piece_type, g_piece_rot, g_piece_x, g_piece_y)) {
        g_state = TETRIS_STATE_OVER;
    }
}

/**
 * @brief  计算投影Y坐标: 当前方块能下落到的最低位
 * @return 投影所在行号
 */
static int8_t find_ghost_y(void)
{
    int8_t gy = g_piece_y;
    while (!collides(g_piece_type, g_piece_rot, g_piece_x, gy + 1)) {
        gy++;
    }
    return gy;
}

static void draw_cell(int8_t col, int8_t row, uint16_t color);

/**
 * @brief  绘制投影方块: 仅绘制外框 + 中心贯穿2px横线, 视觉2px粗边框
 * @param  type  方块类型
 * @param  rot   旋转状态
 * @param  px    列坐标
 * @param  py    行坐标
 */
static void draw_ghost_at(uint8_t type, uint8_t rot, int8_t px, int8_t py)
{
    uint16_t color = piece_colors[type];
    int8_t r, c;
    uint8_t row_bits;

    for (r = 0; r < 4; r++) {
        row_bits = PIECES[PIECE_IDX(type, rot, r)];
        for (c = 0; c < 4; c++) {
            if (!(row_bits & (1 << c))) continue;
            int8_t bx = px + c;
            int8_t by = py + r;
            if (by < 0 || bx < 0 || bx >= BOARD_COLS || by >= BOARD_ROWS) continue;

            uint16_t x = BOARD_X + bx * CELL_SIZE;
            uint16_t y = BOARD_Y + by * CELL_SIZE;

            /* 中心横线 2px: x+3..x+6, y+4..y+5 (贯穿) */
            LCD_Fill(x + 3, y + 4, x + CELL_SIZE - 4, y + 5, color);

            /* 上边 2px: x+1..x+8, y+1..y+2 */
            LCD_Fill(x + 1, y + 1, x + CELL_SIZE - 2, y + 2, color);
            /* 下边 2px: x+1..x+8, y+7..y+8 */
            LCD_Fill(x + 1, y + CELL_SIZE - 3,
                     x + CELL_SIZE - 2, y + CELL_SIZE - 2, color);
            /* 左边 2px: x+1..x+2, y+3..y+6 */
            LCD_Fill(x + 1, y + 3, x + 2, y + CELL_SIZE - 3, color);
            /* 右边 2px: x+7..x+8, y+3..y+6 */
            LCD_Fill(x + CELL_SIZE - 3, y + 3,
                     x + CELL_SIZE - 2, y + CELL_SIZE - 3, color);
        }
    }
}

/**
 * @brief  绘制棋盘背景: 填充底色 + 绘制网格线
 */
static void draw_board_bg(void)
{
    uint8_t i;
    LCD_Fill(BOARD_X, BOARD_Y,
             BOARD_X + BOARD_W - 1, BOARD_Y + BOARD_H - 1, BOARD_BG);

    /* 绘制网格线 */
    for (i = 0; i <= BOARD_COLS; i++) {
        uint16_t x = BOARD_X + i * CELL_SIZE;
        LCD_Fill(x, BOARD_Y, x + GRID_WIDTH - 1, BOARD_Y + BOARD_H - 1, BOARD_GRID);
    }
    for (i = 0; i <= BOARD_ROWS; i++) {
        uint16_t y = BOARD_Y + i * CELL_SIZE;
        LCD_Fill(BOARD_X, y, BOARD_X + BOARD_W - 1, y + GRID_WIDTH - 1, BOARD_GRID);
    }
}

/**
 * @brief  绘制单个格子: 填充指定颜色的方块, 留1px边距不覆盖网格线
 * @param  col   列坐标
 * @param  row   行坐标
 * @param  color 填充颜色
 */
static void draw_cell(int8_t col, int8_t row, uint16_t color)
{
    if (row < 0 || col < 0 || col >= BOARD_COLS || row >= BOARD_ROWS) return;
    uint16_t x = BOARD_X + col * CELL_SIZE;
    uint16_t y = BOARD_Y + row * CELL_SIZE;
    /* 方块覆盖网格, 留1px边距 */
    LCD_Fill(x + 1, y + 1, x + CELL_SIZE - 2, y + CELL_SIZE - 2, color);
}

/**
 * @brief  擦除或绘制一个完整方块 (4x4格子, 不含网格线)
 * @param  type   方块类型
 * @param  rot    旋转状态
 * @param  px     起始列坐标
 * @param  py     起始行坐标
 * @param  color  填充颜色 (BOARD_BG=擦除, piece_colors[type]=绘制)
 */
static void draw_piece_at(uint8_t type, uint8_t rot, int8_t px, int8_t py, uint16_t color)
{
    uint8_t r, c;
    for (r = 0; r < 4; r++) {
        uint8_t row_bits = PIECES[PIECE_IDX(type, rot, r)];
        for (c = 0; c < 4; c++) {
            if (row_bits & (1 << c)) {
                draw_cell(px + c, py + r, color);
            }
        }
    }
}

/**
 * @brief  绘制完整棋盘: 背景网格 + 已锁方块 + 投影/活动方块 + 消行动画
 * @note   g_board_dirty==2 时全量重绘, ==1 时增量擦除重绘, ==3 时仅局部重绘电池区域
 */
static void draw_board(void)
{
    uint8_t r, c;

    if (g_board_dirty >= 2) {
        /* g_board_dirty==3: 只重绘电池图标覆盖的棋盘区域 (列0~10, 行5~14) */
        if (g_board_dirty == 2) {
            draw_board_bg();
        } else {
            /* 电池区域与棋盘交集 x:50~179, y:79~161 */
            uint16_t fill_x1 = BOARD_X;
            uint16_t fill_y1 = BOARD_Y + 5 * CELL_SIZE + 1;
            uint16_t fill_x2 = BOARD_X + BOARD_W - 1;
            uint16_t fill_y2 = BOARD_Y + 15 * CELL_SIZE - 1;
            uint8_t gi;
            LCD_Fill(fill_x1, fill_y1, fill_x2, fill_y2, BOARD_BG);
            for (gi = 0; gi <= BOARD_COLS; gi++) {
                uint16_t x = BOARD_X + gi * CELL_SIZE;
                LCD_Fill(x, fill_y1, x + GRID_WIDTH - 1, fill_y2, BOARD_GRID);
            }
            for (gi = 6; gi <= 14; gi++) {
                uint16_t y = BOARD_Y + gi * CELL_SIZE;
                LCD_Fill(fill_x1, y, fill_x2, y + GRID_WIDTH - 1, BOARD_GRID);
            }
        }
        for (r = 0; r < BOARD_ROWS; r++) {
            for (c = 0; c < BOARD_COLS; c++) {
                if (g_board[r][c] != 0) {
                    draw_cell(c, r, piece_colors[g_board[r][c] - 1]);
                }
            }
        }
        if (g_state == TETRIS_STATE_PLAYING) {
            /* 投影块 */
            g_ghost_y = find_ghost_y();
            if (g_ghost_y != g_piece_y) {
                draw_ghost_at(g_piece_type, g_piece_rot,
                              g_piece_x, g_ghost_y);
            }
            draw_piece_at(g_piece_type, g_piece_rot,
                          g_piece_x, g_piece_y,
                          piece_colors[g_piece_type]);
        }
        g_prev_ghost_y = g_ghost_y;
    } else {
        /* 增量更新: 擦除上一帧方块+投影, 再重新绘制, 并修补网格线+背景 */
        g_ghost_y = find_ghost_y();

        /* 擦除旧方块位置 (背景色覆盖) */
        draw_piece_at(g_prev_type, g_prev_rot,
                      g_prev_px, g_prev_py, BOARD_BG);
        /* 擦除旧投影 (位置或方块没变化时) */
        if (g_prev_ghost_y != g_prev_py
            && (g_prev_ghost_y != g_ghost_y || g_prev_px != g_piece_x || g_prev_rot != g_piece_rot)) {
            draw_piece_at(g_prev_type, g_prev_rot,
                          g_prev_px, g_prev_ghost_y, BOARD_BG);
        }
        /* 修复旧位置下方被覆盖的背景格子 */
        {
            uint8_t rr, cc;
            /* 被旧方块遮挡的背景 */
            for (rr = 0; rr < 4; rr++) {
                uint8_t row_bits = PIECES[PIECE_IDX(g_prev_type, g_prev_rot, rr)];
                for (cc = 0; cc < 4; cc++) {
                    if (row_bits & (1 << cc)) {
                        int8_t by = g_prev_py + rr;
                        int8_t bx = g_prev_px + cc;
                        if (by >= 0 && by < BOARD_ROWS && bx >= 0 && bx < BOARD_COLS) {
                            if (g_board[by][bx] != 0) {
                                draw_cell(bx, by, piece_colors[g_board[by][bx] - 1]);
                            }
                        }
                    }
                }
            }
            /* 被旧投影遮挡的背景 (投影位置/方块变化) */
            if (g_prev_ghost_y != g_prev_py && g_prev_ghost_y != g_piece_y
                && (g_prev_ghost_y != g_ghost_y || g_prev_px != g_piece_x || g_prev_rot != g_piece_rot)) {
                for (rr = 0; rr < 4; rr++) {
                    uint8_t row_bits = PIECES[PIECE_IDX(g_prev_type, g_prev_rot, rr)];
                    for (cc = 0; cc < 4; cc++) {
                        if (row_bits & (1 << cc)) {
                            int8_t by = g_prev_ghost_y + rr;
                            int8_t bx = g_prev_px + cc;
                            if (by >= 0 && by < BOARD_ROWS && bx >= 0 && bx < BOARD_COLS) {
                                if (g_board[by][bx] != 0) {
                                    draw_cell(bx, by, piece_colors[g_board[by][bx] - 1]);
                                }
                            }
                        }
                    }
                }
            }
        }
        /* 绘制新方块位置 */
        if (g_state == TETRIS_STATE_PLAYING) {
            /* 投影 (幽灵方块, 不覆盖活动方块) */
            if (g_ghost_y != g_piece_y) {
                draw_ghost_at(g_piece_type, g_piece_rot,
                              g_piece_x, g_ghost_y);
            }
            /* 绘制活动方块 */
            draw_piece_at(g_piece_type, g_piece_rot,
                          g_piece_x, g_piece_y,
                          piece_colors[g_piece_type]);
        }
        g_prev_ghost_y = g_ghost_y;
    }

    /* 保存当前位置供下一帧增量使用 */
    g_prev_px = g_piece_x;
    g_prev_py = g_piece_y;
    g_prev_type = g_piece_type;
    g_prev_rot = g_piece_rot;

    /* 消行动画: 闪烁要消除的行 */
    if (g_clear_flash > 0) {
        uint8_t rr;
        for (rr = 0; rr < BOARD_ROWS; rr++) {
            if (g_clear_mask & (1UL << rr)) {
                uint16_t ry = BOARD_Y + (uint16_t)rr * CELL_SIZE;
                if (!(g_clear_flash & 1)) {
                    /* 白闪: 整行画白色 (不触及网格线) */
                    LCD_Fill(BOARD_X + 1, ry + 1,
                             BOARD_X + BOARD_W - 2, ry + CELL_SIZE - 2,
                             LCD_WHITE);
                } else {
                    /* 正常: 恢复为背景色 (消除效果) */
                    LCD_Fill(BOARD_X + 1, ry + 1,
                             BOARD_X + BOARD_W - 2, ry + CELL_SIZE - 2,
                             LCD_BLACK);
                }
            }
        }
    }
}

/**
 * @brief  绘制侧边栏: 分数和消行数 (棋盘右侧, X=180~239)
 * @note   仅在 g_sidebar_dirty 为真时绘制, 节省 SPI 带宽
 */
static void draw_sidebar(void)
{
    if (!g_sidebar_dirty) return;

    uint16_t sx = BOARD_X + BOARD_W;  /* 侧边栏起始X = 180 */

    /* 清除侧边栏上半区域 */
    LCD_Fill(sx, 0, 239, PREVIEW_Y - 1, LCD_BLACK);

    /* 分数: 8x14 右对齐数字串, 右对齐 (挨着 dx<=225 边界) */
    {
        LCD_CS_CLR;
        uint8_t digits[8];
        uint8_t dc = uint_to_digits(g_score, digits);

        uint16_t dx = 225;
        uint8_t i;
        for (i = 0; i < dc; i++) {
            dx -= 9;
            sok_draw_digit_scaled(dx, 60, digits[i], LCD_WHITE, 8, 14);
        }
        LCD_CS_SET;
    }

    /* 消行计数: 8x14 右对齐数字串 */
    {
        LCD_CS_CLR;
        uint8_t digits[8];
        uint8_t dc = uint_to_digits(g_lines, digits);

        uint16_t dx = 225;
        uint8_t i;
        for (i = 0; i < dc; i++) {
            dx -= 9;
            sok_draw_digit_scaled(dx, 85, digits[i], LCD_GRAY, 8, 14);
        }
        LCD_CS_SET;
    }

    g_prev_score = g_score;
    g_prev_lines = g_lines;
    g_sidebar_dirty = 0;
    game_battery_icon_update(184, 40, 1);
}

/**
 * @brief  绘制下一个方块预览 (棋盘右侧, X=190, Y=110)
 */
static void draw_preview(void)
{
    uint8_t r, c;
    /* 清除预览区域 */
    LCD_Fill(PREVIEW_X, PREVIEW_Y,
             PREVIEW_X + 4 * PREVIEW_CELL - 1,
             PREVIEW_Y + 4 * PREVIEW_CELL - 1, BOARD_BG);

    uint16_t pcolor = piece_colors[g_next_type];
    for (r = 0; r < 4; r++) {
        uint8_t row_bits = PIECES[PIECE_IDX(g_next_type, 0, r)];  /* 旋转0 */
        for (c = 0; c < 4; c++) {
            if (row_bits & (1 << c)) {
                uint16_t px = PREVIEW_X + c * PREVIEW_CELL;
                uint16_t py = PREVIEW_Y + r * PREVIEW_CELL;
                LCD_Fill(px + 1, py + 1,
                         px + PREVIEW_CELL - 2,
                         py + PREVIEW_CELL - 2, pcolor);
            }
        }
    }
}

/**
 * @brief  绘制游戏结束画面: 居中显示"游戏结束" 32x32文字 + 16x32数字得分
 */
static void draw_game_over(void)
{
    /* 全黑背景: 覆盖棋盘区域 */
    LCD_Fill(BOARD_X, BOARD_Y,
             BOARD_X + BOARD_W - 1, BOARD_Y + BOARD_H - 1, LCD_BLACK);

    /* 居中显示 "游戏结束" (4字, 32x32) */
    {
        uint16_t gw = 4 * 32;
        uint16_t gx = BOARD_X + (BOARD_W - gw) / 2;
        uint16_t gy = 75;  /* 上方, 给分数留出空间 */
        LCD_CS_CLR;
        sok_draw_char_32(gx,       gy, FlashStore_ResLoad(FLASH_STORE_ID_32_YOU,  0, FONT_CHAR_SIZE), LCD_WHITE);
        sok_draw_char_32(gx + 32,  gy, FlashStore_ResLoad(FLASH_STORE_ID_32_XI3,  0, FONT_CHAR_SIZE), LCD_WHITE);
        sok_draw_char_32(gx + 64,  gy, FlashStore_ResLoad(FLASH_STORE_ID_32_JIE,  0, FONT_CHAR_SIZE), LCD_WHITE);
        sok_draw_char_32(gx + 96,  gy, FlashStore_ResLoad(FLASH_STORE_ID_32_SHU,  0, FONT_CHAR_SIZE), LCD_WHITE);
        LCD_CS_SET;
    }

    /* 下方显示得分数 (16x32 数字) */
    {
        uint8_t digits[8];
        uint8_t dc = uint_to_digits(g_score, digits);

        uint16_t sw = dc * 16;
        uint16_t sx = BOARD_X + (BOARD_W - sw) / 2;
        uint16_t sy = 130;
        LCD_CS_CLR;
        uint8_t i;
        for (i = 0; i < dc; i++) {
            sok_draw_digit_16x32(sx + i * 16, sy, digits[dc - 1 - i], LCD_WHITE);
        }
        LCD_CS_SET;
    }
}

/**
 * @brief  清理棋盘上方区域: 中线上方清黑, 防止残留上一画面内容
 */
static void draw_score_area(void)
{
    /* 将棋盘中线上方区域清黑 (避免残留上一画面) */
    LCD_Fill(0, 0, BOARD_X + BOARD_W - 1, BOARD_Y - 1, LCD_BLACK);
}

/**
 * @brief  初始化俄罗斯方块游戏: 清空棋盘, 重置状态, 生成首个方块, 全屏绘制
 */
void tetris_init(void)
{
    /* ---- 清空棋盘 ---- */
    uint8_t r, c;
    for (r = 0; r < BOARD_ROWS; r++) {
        for (c = 0; c < BOARD_COLS; c++) {
            g_board[r][c] = 0;
        }
    }
    /* ---- 重置游戏状态 ---- */
    g_score = 0;
    g_lines = 0;
    g_level = 0;
    g_state = TETRIS_STATE_PLAYING;
    g_board_dirty = 2;   /* 2=首次全量重绘, 区别于1=增量仅重绘变化格子 */
    g_preview_dirty = 1;
    g_sidebar_dirty = 1;
    g_prev_score = 0;
    g_prev_lines = 0;
    g_gameover_drawn = 0;
    g_drop_timer = 0;
    g_drop_interval = DROP_INTERVAL_INIT;
    g_saved_interval = 0;

    /* ---- 重置触摸/菜单状态 ---- */
    g_touch_down = 0;
    g_prev_pressed = 0;
    g_menu_cooldown = 0;
    g_wait_touch_release = 0;   /* 重新开始后需松手防抖, 防止误触硬降 */
    g_hold_zone = 0;
    g_hold_timer = 0;
    g_lock_delay = 0;
    g_exit_menu_visible = 0;
    g_restart_menu_visible = 0;
    g_restart_sliding = 0;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
    g_ghost_y = 0;
    g_prev_ghost_y = 0;

    /* ---- 生成第一个方块 ---- */
    g_next_type = game_rand() % 7;
    spawn_piece();

    /* ---- 全屏绘制 ---- */
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    draw_board();
    draw_sidebar();
    draw_preview();
    draw_score_area();
    g_board_dirty = 0;
    g_sidebar_dirty = 0;
    g_preview_dirty = 0;
}

/**
 * @brief  重置触摸状态: 从其他游戏返回时调用, 释放触摸态并标记等待松手
 */
void tetris_reset_touch(void)
{
    g_prev_pressed = 1;
    g_touch_down = 0;
    g_touch_start_x = 0;
    g_touch_start_y = 0;
    g_menu_cooldown = 0;
    g_wait_touch_release = 1;
}

/**
 * @brief  从其他游戏返回时，标记所有区域为脏强制全量重绘
 * @note   调用 tetris_update() 下一帧会重绘所有区域，无需立即绘制
 */
void tetris_full_redraw(void)
{
    g_board_dirty = 2;     /* 完整重绘: 填充棋盘背景+网格+所有锁定格 */
    g_sidebar_dirty = 1;
    g_preview_dirty = 1;
}

/**
 * @brief  判断退出/重开菜单是否正在显示
 * @retval 1=菜单可见, 0=不可见
 */
uint8_t tetris_is_menu_visible(void)
{
    return g_exit_menu_visible || g_restart_menu_visible;
}

/**
 * @brief  移动当前方块: 若无碰撞则更新位置并重置锁定延迟
 * @param  dx  列偏移 (-1=左, 1=右, 0=无)
 * @param  dy  行偏移 (通常为0或1)
 */
static void move_piece(int8_t dx, int8_t dy)
{
    if (!collides(g_piece_type, g_piece_rot, g_piece_x + dx, g_piece_y + dy)) {
        g_piece_x += dx;
        g_piece_y += dy;
        g_board_dirty = 1;
        if (g_lock_delay > 0) g_lock_delay = LOCK_DELAY_FRAMES;  /* 每次移动重置锁定计时 */
    }
}

/**
 * @brief  旋转当前方块: 顺时针90度, 支持踢墙偏移 (左/右1~2格)
 */
static void rotate_piece(void)
{
    uint8_t new_rot = (g_piece_rot + 1) & 3;
    uint8_t rotated = 0;

    /* 尝试直接旋转 */
    if (!collides(g_piece_type, new_rot, g_piece_x, g_piece_y)) {
        g_piece_rot = new_rot;
        rotated = 1;
    }
    /* 踢墙: 尝试左右偏移 */
    else if (!collides(g_piece_type, new_rot, g_piece_x - 1, g_piece_y)) {
        g_piece_x -= 1; g_piece_rot = new_rot;
        rotated = 1;
    }
    else if (!collides(g_piece_type, new_rot, g_piece_x + 1, g_piece_y)) {
        g_piece_x += 1; g_piece_rot = new_rot;
        rotated = 1;
    }
    else if (!collides(g_piece_type, new_rot, g_piece_x - 2, g_piece_y)) {
        g_piece_x -= 2; g_piece_rot = new_rot;
        rotated = 1;
    }
    else if (!collides(g_piece_type, new_rot, g_piece_x + 2, g_piece_y)) {
        g_piece_x += 2; g_piece_rot = new_rot;
        rotated = 1;
    }

    if (rotated) {
        g_board_dirty = 1;
        if (g_lock_delay > 0) g_lock_delay = LOCK_DELAY_FRAMES;  /* 每次旋转重置锁定计时 */
    }
    /* 旋转失败, 不做任何操作 */
}

/* Flash 存档接口 */
#define TETRIS_BOARD_CELLS  (BOARD_ROWS * BOARD_COLS)
#define TETRIS_BOARD_BYTES  ((TETRIS_BOARD_CELLS + 1) / 2)
#define TETRIS_SAVE_LEN     (TETRIS_BOARD_BYTES + 18)

/**
 * @brief  保存俄罗斯方块存档: 棋盘(3bit/格压缩) + 分数 + 方块状态
 * @param  buf      输出缓冲区
 * @param  max_len  缓冲区最大长度
 * @return 实际写入长度, 0=失败
 */
uint8_t tetris_save_state(uint8_t *buf, uint8_t max_len)
{
    uint16_t i;
    const uint16_t *board = &g_board[0][0];
    uint8_t *p;
    TetrisState state = g_state;

    if (max_len < TETRIS_SAVE_LEN) return 0;
    if (state == TETRIS_STATE_EXIT_MENU || state == TETRIS_STATE_RESTART_MENU) {
        state = g_state_before_menu;
    }
    if (state > TETRIS_STATE_OVER) state = TETRIS_STATE_PLAYING;

    for (i = 0; i + 1 < TETRIS_BOARD_CELLS; i += 2) {
        buf[i >> 1] = (uint8_t)((board[i] & 7)
                        | ((board[i + 1] & 7) << 4));
    }
    buf[TETRIS_BOARD_CELLS >> 1] =
        (uint8_t)(board[TETRIS_BOARD_CELLS - 1] & 7);

    p = buf + TETRIS_BOARD_BYTES;
    memcpy(p, &g_score, sizeof(g_score)); p += sizeof(g_score);
    memcpy(p, &g_lines, sizeof(g_lines)); p += sizeof(g_lines);
    memcpy(p, &g_level, sizeof(g_level)); p += sizeof(g_level);
    *p++ = g_piece_type;
    *p++ = g_piece_rot;
    *p++ = (uint8_t)g_piece_x;
    *p++ = (uint8_t)g_piece_y;
    *p++ = g_next_type;
    memcpy(p, &g_drop_interval, sizeof(g_drop_interval));
    p += sizeof(g_drop_interval);
    *p = (uint8_t)state;
    return TETRIS_SAVE_LEN;
}

/**
 * @brief  恢复俄罗斯方块存档: 还原棋盘、分数、方块状态并全量重绘
 * @param  buf  存档数据缓冲区
 * @param  len  数据长度
 * @retval 1=恢复成功, 0=失败
 */
uint8_t tetris_restore_state(const uint8_t *buf, uint8_t len)
{
    uint16_t i;
    uint16_t *board = &g_board[0][0];
    const uint8_t *p;

    if (len != TETRIS_SAVE_LEN) return 0;
    p = buf + TETRIS_BOARD_BYTES + 10;
    if (p[0] >= 7 || p[1] >= 4 || p[4] >= 7
        || p[7] > TETRIS_STATE_OVER) {
        return 0;
    }

    for (i = 0; i + 1 < TETRIS_BOARD_CELLS; i += 2) {
        uint8_t packed = buf[i >> 1];
        board[i] = packed & 7;
        board[i + 1] = (packed >> 4) & 7;
    }
    board[TETRIS_BOARD_CELLS - 1] =
        buf[TETRIS_BOARD_CELLS >> 1] & 7;

    p = buf + TETRIS_BOARD_BYTES;
    memcpy(&g_score, p, sizeof(g_score)); p += sizeof(g_score);
    memcpy(&g_lines, p, sizeof(g_lines)); p += sizeof(g_lines);
    memcpy(&g_level, p, sizeof(g_level)); p += sizeof(g_level);
    g_piece_type = *p++;
    g_piece_rot = *p++;
    g_piece_x = (int8_t)*p++;
    g_piece_y = (int8_t)*p++;
    g_next_type = *p++;
    memcpy(&g_drop_interval, p, sizeof(g_drop_interval));
    p += sizeof(g_drop_interval);
    g_state = (TetrisState)*p;
    g_state_before_menu = g_state;
    if (g_drop_interval == 0) g_drop_interval = DROP_INTERVAL_INIT;

    g_board_dirty = 2;
    g_sidebar_dirty = 1;
    g_preview_dirty = 1;
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    draw_board();
    draw_sidebar();
    draw_preview();
    draw_score_area();
    g_board_dirty = 0;
    g_preview_dirty = 0;
    if (g_state == TETRIS_STATE_OVER) {
        draw_game_over();
        g_gameover_drawn = 1;
    }
    return 1;
}

/**
 * @brief  俄罗斯方块每帧更新: 处理触摸手势、菜单交互、自动下落、消行动画和渲染
 * @retval GAME_RET_NONE=无交互, GAME_RET_INTERACT=有交互, 其他=退出到指定游戏/视频
 */
uint8_t tetris_update(void)
{
    uint8_t interacted = 0;

    /* 菜单冷却 */
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 设置页面下滑→退出菜单 */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        g_state = TETRIS_STATE_EXIT_MENU;
        g_board_dirty = 2;
        g_sidebar_dirty = 1;
        g_preview_dirty = 1;
    }

    /* ---- 退出菜单模式 ---- */
    if (g_state == TETRIS_STATE_EXIT_MENU) {
        /* 重绘菜单背景: 从游戏画面开始绘制 */
        if (g_board_dirty >= 2) {
            uint8_t rr, cc;
            if (g_board_dirty == 2) draw_board_bg();
            else {
                uint8_t gi;
                uint16_t fill_x1 = BOARD_X;
                uint16_t fill_y1 = BOARD_Y + 5 * CELL_SIZE + 1;
                uint16_t fill_x2 = BOARD_X + BOARD_W - 1;
                uint16_t fill_y2 = BOARD_Y + 15 * CELL_SIZE - 1;
                LCD_Fill(fill_x1, fill_y1, fill_x2, fill_y2, BOARD_BG);
                for (gi = 0; gi <= BOARD_COLS; gi++) {
                    uint16_t x = BOARD_X + gi * CELL_SIZE;
                    LCD_Fill(x, fill_y1, x + GRID_WIDTH - 1, fill_y2, BOARD_GRID);
                }
                for (gi = 6; gi <= 14; gi++) {
                    uint16_t y = BOARD_Y + gi * CELL_SIZE;
                    LCD_Fill(fill_x1, y, fill_x2, y + GRID_WIDTH - 1, BOARD_GRID);
                }
            }
            for (rr = 0; rr < BOARD_ROWS; rr++) {
                for (cc = 0; cc < BOARD_COLS; cc++) {
                    if (g_board[rr][cc] != 0) {
                        draw_cell(cc, rr, piece_colors[g_board[rr][cc] - 1]);
                    }
                }
            }
            /* 电池闪灭(g_board_dirty==3)时侧边栏/预览/分数区无重叠或已
               被菜单覆盖，跳过避免大填充闪烁 */
            if (g_board_dirty == 2) {
                draw_sidebar();
                draw_preview();
                draw_score_area();
            }
            g_sidebar_dirty = 0;
            g_preview_dirty = 0;
            /* 活动方块和投影也要画 */
            g_ghost_y = find_ghost_y();
            if (g_ghost_y != g_piece_y) {
                draw_ghost_at(g_piece_type, g_piece_rot, g_piece_x, g_ghost_y);
            }
            draw_piece_at(g_piece_type, g_piece_rot, g_piece_x, g_piece_y,
                          piece_colors[g_piece_type]);
            g_board_dirty = 0;
            g_sidebar_dirty = 0;
            g_preview_dirty = 0;
            if (!g_exit_dragging && !g_exit_sliding) {
                menu_draw_nrow_frame_only(g_exit_drag_y, tetris_row_types, 6);
            }
        }

        /* 底部上划: 打断退出菜单(含拖拽/滑入中), 切换到重新开始菜单 */
        {
            uint8_t r = menu_swipe_to_restart_handler(210, 25);
            if (r) {
                g_board_dirty = 2;
                g_sidebar_dirty = 1;
                g_preview_dirty = 1;
                g_gameover_drawn = 0;
                g_state = TETRIS_STATE_RESTART_MENU;
                return r - 1;
            }
        }

        /* 拖拽菜单 */
        if (g_exit_dragging) {
            TouchState ts_d;
            Touch_Scan(&ts_d);
            uint8_t drag_ret = game_menu_drag_poll(&g_exit_drag_y, &g_exit_drag_start_y,
                                    &g_exit_dragging, &ts_d, tetris_row_types);
            if (drag_ret == 1) {
                g_exit_sliding = 1;
            } else if (drag_ret == 2) {
                menu_clear_exit_area_to(g_exit_drag_y);
                g_exit_menu_visible = 0;
                g_exit_sliding = 0;
                g_exit_dragging = 0;
                g_state = g_state_before_menu;
                g_board_dirty = 2;
                g_sidebar_dirty = 1;
                g_preview_dirty = 1;
                g_gameover_drawn = 0;
                g_touch_down = 0;
                g_menu_cooldown = 15;
            }
            return 0;
        }

        /* 退出菜单滑入: 手指松开, 自动滑入 */
        if (g_exit_sliding) {
            g_exit_drag_y += MENU_SLIDE_SPEED;
            if (g_exit_drag_y >= MENU_Y) {
                g_exit_drag_y = MENU_Y;
                g_exit_sliding = 0;
                g_touch_down = 0;
            }
            menu_draw_nrow_frame_only(g_exit_drag_y, tetris_row_types, 6);
            return 0;
        }

        TouchState ts;
        uint8_t ret = game_exit_menu_poll(tetris_exit_map, &g_prev_pressed, &ts);
        if (ret == EXIT_RET_BACK) {
            menu_exit_back_cleanup();
            g_state = g_state_before_menu;
            g_board_dirty = 2;
            g_sidebar_dirty = 1;
            g_preview_dirty = 1;
            g_gameover_drawn = 0;
            return 0;
        }
        if (ret != EXIT_RET_NONE) {
            g_exit_menu_visible = 0;
            g_state = TETRIS_STATE_PLAYING;
            return ret;
        }
        return 0;
    }

    /* ---- 重开菜单模式 ---- */
    if (g_state == TETRIS_STATE_RESTART_MENU) {
        /* 重绘菜单背景: 从游戏画面开始绘制 */
        if (g_board_dirty >= 2) {
            uint8_t rr, cc;
            if (g_board_dirty == 2) draw_board_bg();
            else {
                uint8_t gi;
                uint16_t fill_x1 = BOARD_X;
                uint16_t fill_y1 = BOARD_Y + 5 * CELL_SIZE + 1;
                uint16_t fill_x2 = BOARD_X + BOARD_W - 1;
                uint16_t fill_y2 = BOARD_Y + 15 * CELL_SIZE - 1;
                LCD_Fill(fill_x1, fill_y1, fill_x2, fill_y2, BOARD_BG);
                for (gi = 0; gi <= BOARD_COLS; gi++) {
                    uint16_t x = BOARD_X + gi * CELL_SIZE;
                    LCD_Fill(x, fill_y1, x + GRID_WIDTH - 1, fill_y2, BOARD_GRID);
                }
                for (gi = 6; gi <= 14; gi++) {
                    uint16_t y = BOARD_Y + gi * CELL_SIZE;
                    LCD_Fill(fill_x1, y, fill_x2, y + GRID_WIDTH - 1, BOARD_GRID);
                }
            }
            for (rr = 0; rr < BOARD_ROWS; rr++) {
                for (cc = 0; cc < BOARD_COLS; cc++) {
                    if (g_board[rr][cc] != 0) {
                        draw_cell(cc, rr, piece_colors[g_board[rr][cc] - 1]);
                    }
                }
            }
            if (g_board_dirty == 2) {
                draw_sidebar();
                draw_preview();
                draw_score_area();
            }
            g_sidebar_dirty = 0;
            g_preview_dirty = 0;
            /* 活动方块和投影也要画，电池刷新后不丢 */
            g_ghost_y = find_ghost_y();
            if (g_ghost_y != g_piece_y) {
                draw_ghost_at(g_piece_type, g_piece_rot, g_piece_x, g_ghost_y);
            }
            draw_piece_at(g_piece_type, g_piece_rot, g_piece_x, g_piece_y,
                          piece_colors[g_piece_type]);
            g_board_dirty = 0;
            g_sidebar_dirty = 0;
            g_preview_dirty = 0;
        }
        if (!g_restart_dragging && !g_restart_sliding) {
            game_battery_icon_update(184, 40, 0);
            menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
        }
        TouchState ts;

        if (g_restart_dragging) {
            Touch_Scan(&ts);
            uint8_t drag_ret = game_restart_menu_drag_poll(&g_restart_slide_y, g_touch_start_y,
                                             &g_restart_dragging, &ts);
            if (drag_ret == 1) {
                g_restart_sliding = 1;
                g_touch_down = 0;
                g_hold_zone = 0;
                g_prev_pressed = 0;
            } else if (drag_ret == 2) {
                goto restart_menu_close;
            } else if (drag_ret == 3) {
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
                    g_state = TETRIS_STATE_PLAYING;
                    tetris_init();
                    g_wait_touch_release = 1;
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
        g_board_dirty = 2;
        g_sidebar_dirty = 1;
        g_preview_dirty = 1;
        game_menu_restart_to_exit((uint8_t *)&g_state, TETRIS_STATE_EXIT_MENU, ts.y);
        return 0;
        restart_menu_close:
        menu_restart_cleanup();
        g_state = g_state_before_menu;
        g_wait_touch_release = 1;
        g_board_dirty = 2;
        g_sidebar_dirty = 1;
        g_preview_dirty = 1;
        g_gameover_drawn = 0;
        g_prev_pressed = ts.pressed;
        return interacted;
    }

    /* ---- 游戏结束状态: 显示分数, 等待手势 ---- */
    if (g_state == TETRIS_STATE_OVER) {
        TouchState ts;
        Touch_Scan(&ts);

        if (ts.pressed && !g_touch_down) {
            g_touch_down = 1;
            g_touch_start_x = ts.x;
            g_touch_start_y = ts.y;
            if (ts.y <= 210)
                g_menu_cooldown = 0;  /* 仅在非上划区清冷却，防止取消后重触发 */
        }
        else if (ts.pressed && g_touch_down
                 && g_touch_start_y < MENU_SWIPE_START_Y) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (dy_move > MENU_SWIPE_TRIGGER_DIST) {
                g_state_before_menu = g_state;
                g_state = TETRIS_STATE_EXIT_MENU;
                g_exit_menu_visible = 1;
                g_exit_dragging = 1;
                g_exit_drag_start_y = ts.y;
                if (dy_move > MENU_DRAG_MAX_STEP)
                    dy_move = MENU_DRAG_MAX_STEP;
                g_exit_drag_y = -MENU_H + dy_move;
                return 0;
            }
        }
        else if (ts.pressed && g_touch_down && g_touch_start_y > 210) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (dy_move < -5) {
                g_state_before_menu = TETRIS_STATE_OVER;
                g_state = TETRIS_STATE_RESTART_MENU;
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

            /* 底部上滑 → 重开 */
            if (g_touch_start_y > 210 && dy < -SWIPE_THRESH) {
                g_state_before_menu = TETRIS_STATE_OVER;
                g_state = TETRIS_STATE_RESTART_MENU;
                g_restart_menu_visible = 1;
                g_restart_sliding = 1;
                g_restart_dragging = 0;
                g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H + dy;
                if (g_restart_slide_y < RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP)
                    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP;
                return GAME_RET_INTERACT;
            }
            /* 顶部下滑 → 退出 */
            if (g_touch_start_y < MENU_SWIPE_START_Y
                && dy >= MENU_SWIPE_COMMIT_DIST) {
                g_state_before_menu = TETRIS_STATE_OVER;
                g_state = TETRIS_STATE_EXIT_MENU;
                g_exit_menu_visible = 1;
                g_exit_sliding = 1;
                g_exit_drag_y = -MENU_H;
                return GAME_RET_INTERACT;
            }
        }
        g_prev_pressed = ts.pressed;
        goto render_only;
    }

    /* ---- 正常游戏: 触摸手势 + 自动下落 ---- */
    TouchState ts;
    Touch_Scan(&ts);

    if (g_wait_touch_release) {
        g_touch_down = 0;
        g_hold_zone = 0;
        if (!ts.pressed) {
            g_wait_touch_release = 0;
            g_menu_cooldown = 15;
        }
    }
    else if (ts.pressed && !g_touch_down) {
        g_touch_down = 1;
        g_touch_start_x = ts.x;
        g_touch_start_y = ts.y;
        if (ts.y <= 210)
            g_menu_cooldown = 0;  /* 仅在非上划区清冷却，防止取消后重触发 */

        /* 触摸分区: 左右贯穿上中段, 中间旋转(松开触发), 下段硬降 */
        if (g_menu_cooldown == 0 && g_state == TETRIS_STATE_PLAYING
            && ts.y >= 20 && ts.y < 180) {
            int8_t dir = 0;
            if (ts.y < 110) {
                if (ts.x < 60) dir = -1;
                else if (ts.x >= 180) dir = 1;
            } else {
                if (ts.x < 120) dir = -1;
                else dir = 1;
            }
            if (dir) {
                move_piece(dir, 0);
                g_hold_zone = 1 + (dir > 0);
                g_hold_timer = HOLD_REPEAT_START;
                interacted = 1;
            }
        }
    }
    else if (ts.pressed && g_touch_down && g_hold_zone && g_state == TETRIS_STATE_PLAYING) {
        /* 长按重复: 持续移动 */
        if (g_hold_timer > 0) {
            g_hold_timer--;
        }
        if (g_hold_timer == 0) {
            if (g_hold_zone == 1) move_piece(-1, 0);
            else                   move_piece(1, 0);
            g_hold_timer = HOLD_REPEAT_RATE;
            interacted = 1;
        }
    }
    /* 顶部下滑: 进入退出菜单拖拽 */
    if (game_menu_enter_exit_drag(&ts, (uint8_t *)&g_state,
                                   TETRIS_STATE_PLAYING, TETRIS_STATE_EXIT_MENU))
        return 0;
    /* 底部上划: 进入重新开始菜单拖拽 */
    if (game_menu_enter_restart_drag(&ts, (uint8_t *)&g_state,
                                      TETRIS_STATE_PLAYING, TETRIS_STATE_RESTART_MENU, 210))
        return 0;

    /* 释放防抖: 连丢1帧才确认松开, 防止触摸IC快速滑动时偶发丢帧 */
    {
        static uint8_t rel_db = 0;
        if (ts.pressed) { rel_db = 0; }
        else if (!g_touch_down) { rel_db = 0; }
        else if (++rel_db < 2) { return 0; }
    }
    if (!ts.pressed && g_touch_down) {
        g_touch_down = 0;
        g_hold_zone = 0;
        int16_t dy = (int16_t)ts.y - (int16_t)g_touch_start_y;
        int16_t dx = (int16_t)ts.x - (int16_t)g_touch_start_x;
        int16_t adx = (dx < 0) ? -dx : dx;
        int16_t ady = (dy < 0) ? -dy : dy;

        /* 顶部下滑松手: 提交退出菜单 */
        if (game_menu_commit_exit(dy, (uint8_t *)&g_state, TETRIS_STATE_EXIT_MENU))
            return GAME_RET_INTERACT;

        /* 底部上划松手: 提交重新开始菜单 */
        if (game_menu_commit_restart(dy, (uint8_t *)&g_state,
                                      TETRIS_STATE_RESTART_MENU, 210, 25))
            return GAME_RET_INTERACT;

        if (g_menu_cooldown == 0 && g_state == TETRIS_STATE_PLAYING
            && ady < SWIPE_THRESH) {
            if (g_touch_start_y >= 180 && adx < SWIPE_THRESH) {
                if (g_saved_interval == 0) {
                    g_saved_interval = g_drop_interval;
                    g_drop_interval = DROP_INTERVAL_HARD;
                    g_drop_timer = DROP_INTERVAL_HARD;
                }
                interacted = 1;
            }
            else if (g_touch_start_y >= 20 && g_touch_start_y < 110
                     && g_touch_start_x >= 60 && g_touch_start_x < 180) {
                rotate_piece();
                interacted = 1;
            }
        }
    }
    g_prev_pressed = ts.pressed;

    /* ---- 消行动画状态机 (锁定时触发) ---- */
    if (g_clear_flash > 0) {
        if (g_flash_hold > 0) {
            g_flash_hold--;
        } else {
            g_flash_hold = FLASH_HOLD;
            g_clear_flash--;
        }
        g_board_dirty = 1;  /* 强制刷新, 闪烁画幅持续 */
        if (g_clear_flash == 0) {
            /* 闪烁结束, 真正执行消行并生成新方块 */
            uint8_t cleared = clear_lines();
            if (cleared > 0) {
                g_lines += cleared;
                static const uint16_t line_scores[] = {0, 100, 300, 500, 800};
                if (cleared <= 4) g_score += line_scores[cleared];
                /* 加分: 行越高分越多 */
                {
                    uint8_t r;
                    for (r = 0; r < BOARD_ROWS; r++) {
                        if (g_clear_mask & (1UL << r))
                            g_score += (uint32_t)(BOARD_ROWS - r) * 10;
                    }
                }
                g_sidebar_dirty = 1;
                if (g_drop_interval > DROP_INTERVAL_MIN) {
                    g_drop_interval = DROP_INTERVAL_INIT - (int16_t)(g_lines / DROP_ACCEL_STEP);
                    if (g_drop_interval < DROP_INTERVAL_MIN) g_drop_interval = DROP_INTERVAL_MIN;
                }
            }
            spawn_piece();
            g_board_dirty = 2;  /* 消行后全量刷新 */
        }
    }

    /* ---- 自动下落 (锁定前处理) ---- */
    if (g_state == TETRIS_STATE_PLAYING && g_clear_flash == 0) {
        g_drop_timer++;
        if (g_drop_timer >= g_drop_interval) {
            g_drop_timer = 0;
            if (!collides(g_piece_type, g_piece_rot, g_piece_x, g_piece_y + 1)) {
                g_piece_y++;
                g_board_dirty = 1;
                g_lock_delay = 0;   /* 下落成功, 重置锁定计时 */
            } else if (g_lock_delay == 0) {
                /* 下落失败, 开始锁定延迟 */
                g_lock_delay = LOCK_DELAY_FRAMES;
            }
        }
    }

    /* ---- 锁定延迟倒计时 ---- */
    if (g_lock_delay > 0 && g_state == TETRIS_STATE_PLAYING && g_clear_flash == 0) {
        /* 每次移动/旋转会重置锁定延迟 (防止误触墙壁/落下过快) */
        if (!collides(g_piece_type, g_piece_rot, g_piece_x, g_piece_y + 1)) {
            g_lock_delay = 0;
        } else {
            g_lock_delay--;
            if (g_lock_delay == 0) {
                /* 延迟到期, 锁定 */
                lock_piece();

                /* 恢复原始下落速度 */
                if (g_saved_interval) {
                    g_drop_interval = g_saved_interval;
                    g_saved_interval = 0;
                }

                /* 检查满行 */
                {
                    int8_t r;
                    g_clear_mask = 0;
                    for (r = 0; r < BOARD_ROWS; r++) {
                        uint8_t full = 1;
                        uint8_t c;
                        for (c = 0; c < BOARD_COLS; c++) {
                            if (g_board[r][c] == 0) { full = 0; break; }
                        }
                        if (full) g_clear_mask |= (1UL << r);
                    }
                }

                if (g_clear_mask) {
                    g_clear_flash = CLEAR_FLASH_FRAMES;
                    g_flash_hold = FLASH_HOLD;
                    g_board_dirty = 2;
                } else {
                    /* 无满行: 锁定后直接生成新方块, 不触发消行动画 */
                    spawn_piece();
                    g_board_dirty = 1;
                }
            }
        }
    }
    /* ---- 渲染 ---- */
    if (g_board_dirty) {
        Main_ClearLowBattSaveFlag();  /* 棋盘有变化时清除低电量保存标志 */
        draw_board();
        draw_score_area();
        g_board_dirty = 0;
    }
    draw_sidebar();
    if (g_preview_dirty) {
        draw_preview();
        g_preview_dirty = 0;
    }
render_only:
    if (g_state == TETRIS_STATE_OVER && !g_gameover_drawn) {
        draw_game_over();
        g_gameover_drawn = 1;
    }

    game_battery_icon_update(184, 40, 0);
    Delay_Ms(FRAME_DELAY_MS);
    return interacted ? GAME_RET_INTERACT : GAME_RET_NONE;
}
