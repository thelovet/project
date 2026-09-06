#ifndef __KLOTSKI_H__
#define __KLOTSKI_H__

#include "ch32v20x.h"

/* 华容道配置: 4x4 (15-puzzle) */
#define KLOTSKI_ROWS      4
#define KLOTSKI_COLS      4
#define KLOTSKI_SIZE      16   /* 4x4 */
#define KLOTSKI_TILE_SIZE 40   /* 每个方块40x40 */
#define KLOTSKI_GAP       5    /* 方块间距 */
#define KLOTSKI_GRID_W    (KLOTSKI_COLS * KLOTSKI_TILE_SIZE + (KLOTSKI_COLS - 1) * KLOTSKI_GAP)
#define KLOTSKI_GRID_X    ((240 - KLOTSKI_GRID_W) / 2)
#define KLOTSKI_GRID_Y    40   /* 顶部留空给分数/关卡 */

/* 16x32 数字居中偏移 */
#define KLOTSKI_DIGIT_W    16
#define KLOTSKI_DIGIT_H    32
#define KLOTSKI_DIGIT_OFFX ((KLOTSKI_TILE_SIZE - KLOTSKI_DIGIT_W) / 2)
#define KLOTSKI_DIGIT_OFFY ((KLOTSKI_TILE_SIZE - KLOTSKI_DIGIT_H) / 2)

/**
 * @brief 游戏状态
 */
typedef enum {
    KLOTSKI_STATE_PLAYING = 0,   /* 进行中 */
    KLOTSKI_STATE_WON,           /* 通关 */
    KLOTSKI_STATE_EXIT_MENU,     /* 退出菜单 */
    KLOTSKI_STATE_RESTART_MENU,  /* 重新开始菜单 */
} KlotskiState;

void    klotski_init(void);
uint8_t klotski_update(void);
void    klotski_reset_touch(void);
void    klotski_full_redraw(void);
uint8_t klotski_is_menu_visible(void);
void    klotski_lowbatt_restore(void);
/* 菜单状态机 hook (供二级设置菜单返回复用, 与一级菜单一致) */
void klotski_menu_mark_dirty(void);
uint8_t klotski_menu_bg_redraw(void);
uint8_t klotski_save_state(uint8_t *buf, uint8_t max_len);
uint8_t klotski_restore_state(const uint8_t *buf, uint8_t len);

#endif
