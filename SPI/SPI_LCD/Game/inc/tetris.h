#ifndef __TETRIS_H__
#define __TETRIS_H__

#include "ch32v20x.h"

/* 游戏返回值 (用于菜单切换) */
#define GAME_RET_NONE     0
#define GAME_RET_INTERACT 1
#define GAME_RET_VIDEO    2
#define GAME_RET_D2048    3
#define GAME_RET_SOKOBAN  4
#define GAME_RET_TETRIS   5
#define GAME_RET_KLOTSKI  6

/**
 * @brief  俄罗斯方块游戏状态枚举
 */
typedef enum {
    TETRIS_STATE_PLAYING = 0,
    TETRIS_STATE_PAUSED,
    TETRIS_STATE_OVER,
    TETRIS_STATE_EXIT_MENU,
    TETRIS_STATE_RESTART_MENU,
} TetrisState;

void    tetris_init(void);
uint8_t tetris_update(void);
void    tetris_reset_touch(void);
void    tetris_full_redraw(void);
uint8_t tetris_is_menu_visible(void);
void    tetris_lowbatt_restore(void);
/* 菜单状态机 hook (供二级设置菜单返回复用, 与一级菜单一致) */
void tetris_menu_mark_dirty(void);
uint8_t tetris_menu_bg_redraw(void);
uint8_t tetris_save_state(uint8_t *buf, uint8_t max_len);
uint8_t tetris_restore_state(const uint8_t *buf, uint8_t len);

#endif
