#ifndef __FISH_H__
#define __FISH_H__

#include "ch32v20x.h"

/* 游戏返回码复用 tetris.h 中 GAME_RET_* */

/**
 * @brief  大鱼吃小鱼游戏状态
 */
typedef enum {
    FISH_STATE_PLAYING = 0,   /* 游戏进行中 */
    FISH_STATE_OVER,          /* 游戏结束 */
    FISH_STATE_EXIT_MENU,     /* 退出菜单中 */
    FISH_STATE_RESTART_MENU,  /* 重新开始菜单 */
} FishState;

/**
 * @brief 初始化 / 更新 / 重启 / 触摸复位
 */
void    fish_init(void);
uint8_t fish_update(void);    /* 返回: GAME_RET_* */
void    fish_reset_touch(void);
void    fish_full_redraw(void);
void    fish_clear_lowbattery(void);    /* 局部清除低电图标覆盖区(不整屏重绘) */
uint8_t fish_is_menu_visible(void);
uint8_t fish_save_state(uint8_t *buf, uint8_t max_len);
uint8_t fish_restore_state(const uint8_t *buf, uint8_t len);

#endif