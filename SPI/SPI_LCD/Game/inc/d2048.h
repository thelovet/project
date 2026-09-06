#ifndef __D2048_H__
#define __D2048_H__

#include "ch32v20x.h"

/* 游戏参数 */
#define D2048_ROWS       4    /* 4行 */
#define D2048_COLS       4    /* 4列 */
#define D2048_TILE_SIZE  40   /* 每个格子尺寸40x40 */
#define D2048_GAP        5    /* 格子间距 */
#define D2048_GRID_W     (D2048_COLS * D2048_TILE_SIZE + (D2048_COLS - 1) * D2048_GAP)  /* 175 */
#define D2048_GRID_X     ((240 - D2048_GRID_W) / 2)   /* 32 */
#define D2048_GRID_Y     36   /* 网格Y起点 (上方留空给分数) */

/* 16x32 数字居中偏移 */
#define D2048_DIGIT_W    16
#define D2048_DIGIT_H    32
#define D2048_DIGIT_OFFX ((D2048_TILE_SIZE - D2048_DIGIT_W) / 2)  /* 12 */
#define D2048_DIGIT_OFFY ((D2048_TILE_SIZE - D2048_DIGIT_H) / 2)  /* 4 */

/* 分数显示位置 */
#define D2048_SCORE_X    120
#define D2048_SCORE_Y    8

/**
 * @brief 2048 游戏状态
 */
typedef enum {
    D2048_STATE_PLAYING = 0,   /* 游戏进行中 */
    D2048_STATE_WON,           /* 达成2048 */
    D2048_STATE_LOST,          /* 无可用移动 */
    D2048_STATE_EXIT_MENU,     /* 退出菜单中 */
    D2048_STATE_RESTART_MENU,  /* 重新开始菜单 */
} D2048State;

/**
 * @brief 初始化 / 更新 / 重启 / 触摸复位
 */
void    d2048_init(void);
uint8_t d2048_update(void);    /* 返回: 0=无交互, 1=有交互, 2=返回视频, 3=切换推箱子 */
void    d2048_reset_touch(void);
void    d2048_full_redraw(void);
uint8_t d2048_is_menu_visible(void);
void    d2048_lowbatt_restore(void);
/* 菜单状态机 hook (供二级设置菜单返回复用, 与一级菜单一致) */
void d2048_menu_mark_dirty(void);
uint8_t d2048_menu_bg_redraw(void);
uint8_t d2048_save_state(uint8_t *buf, uint8_t max_len);
uint8_t d2048_restore_state(const uint8_t *buf, uint8_t len);

#endif
