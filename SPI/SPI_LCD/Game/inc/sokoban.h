#ifndef __SOKOBAN_H__
#define __SOKOBAN_H__

#include "ch32v20x.h"
#include "menu.h"

/**
 * @brief 地图元素编码
 */
#define SOK_FLOOR            0   /* 空地 */
#define SOK_WALL             1   /* 墙 */
#define SOK_TARGET           2   /* 目标点 */
#define SOK_PLAYER           3   /* 玩家初始位置 */
#define SOK_BOX              4   /* 箱子 */
#define SOK_BOX_ON_TARGET    5   /* 箱子在目标上(已完成) */
#define SOK_OUTER            6   /* 外界 */
#define SOK_PLAYER_ON_TARGET 7   /* 玩家在目标上(保留) */

/**
 * @brief 关卡总数 / 格子尺寸 / 最大格子数
 */
#define SOKOBAN_LEVEL_COUNT  50
#define SOK_TILE_SIZE        30   /* 每个格子30x30像素 */
#define SOK_TILE_PIXELS      (SOK_TILE_SIZE * SOK_TILE_SIZE)  /* 900 */
#define SOK_PLAYER_SIZE      25   /* 人物25x25像素 */
#define SOK_PLAYER_PIXELS    (SOK_PLAYER_SIZE * SOK_PLAYER_SIZE)  /* 625 */
#define SOK_TARGET_SIZE      25   /* 目标25x25像素 */
#define SOK_TARGET_PIXELS    (SOK_TARGET_SIZE * SOK_TARGET_SIZE)  /* 625 */
#define SOK_MAX_CELLS        (16 * 16)  /* 最大格子数, 防止极限地图越界 */

/**
 * @brief 关卡数据结构
 */
typedef struct {
    uint8_t       rows;       /* 行数 */
    uint8_t       cols;       /* 列数 */
    const uint8_t *map;       /* 关卡地图数据 [rows * cols], 存Flash */
} SokobanLevel;

extern const uint16_t sok_img_bg_palette[];
extern const uint16_t sok_img_wall_palette[];
extern const uint16_t sok_img_box_palette[];

/**
 * @brief 关卡表
 */
extern const SokobanLevel sokoban_levels[SOKOBAN_LEVEL_COUNT];

/**
 * @brief 30x30 RGB565 位图数据 (每个900个uint16_t)
 */
extern const uint8_t sok_img_bg[SOK_TILE_PIXELS];    /* 地板(背景) */
extern const uint8_t sok_img_wall[SOK_TILE_PIXELS];  /* 墙 */
extern const uint16_t sok_img_player[SOK_PLAYER_PIXELS];/* 玩家 25x25 */
extern const uint8_t sok_img_box[SOK_TILE_PIXELS];   /* 箱子 */
extern const uint16_t sok_img_target_palette[];
extern const uint8_t sok_img_target[SOK_TARGET_PIXELS];/* 目标 25x25 */
extern const uint16_t sok_img_box_on_target_palette[];
extern const uint8_t sok_img_box_on_target[SOK_TILE_PIXELS];/* 箱子在目标上 30x30 */

/**
 * @brief  绘制关卡到LCD
 * @param  level_index  关卡序号 (0 ~ SOKOBAN_LEVEL_COUNT-1)
 */
void sokoban_draw_level(uint8_t level_index);

/**
 * @brief 移动方向枚举
 */
typedef enum {
    SOK_DIR_UP = 0,
    SOK_DIR_DOWN,
    SOK_DIR_LEFT,
    SOK_DIR_RIGHT,
    SOK_DIR_NONE
} SokobanDir;

/**
 * @brief 核心函数
 */
void     sokoban_init_level(uint8_t level_index);   /* 初始化关卡, 加载地图数据并计算自适应缩放 */
uint8_t  sokoban_move(SokobanDir dir);               /* 移动人物, 返回1=实际移动 */
uint8_t  sokoban_is_won(void);                       /* 检查通关, 1=通关 */

/**
 * @brief 游戏状态枚举 (用于主循环)
 */
typedef enum {
    SOK_STATE_PLAYING = 0,  /* 游戏进行中 */
    SOK_STATE_WON,          /* 通关, 显示"恭喜通关"和3个按钮 */
    SOK_STATE_MENU,         /* 菜单界面显示中 */
} SokobanState;

/**
 * @brief  每帧调用, 处理触摸输入和游戏状态机
 * @retval  1=本帧有用户交互(触摸/按键), 0=无
 */
uint8_t       sokoban_update(void);
void          sokoban_reset_touch(void);
uint8_t       sokoban_save_state(uint8_t *buf, uint8_t max_len);
uint8_t       sokoban_restore_state(const uint8_t *buf, uint8_t len);
void          sokoban_full_redraw(void);
void          sokoban_lowbatt_restore(void);
uint8_t       sokoban_is_menu_visible(void);
void          sokoban_menu_settings_restore(void);  /* 从设置菜单返回到按钮面板 */

#endif
