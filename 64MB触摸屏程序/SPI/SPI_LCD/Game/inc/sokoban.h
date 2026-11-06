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
 * @note  地图本体已移到**外部 Flash 资源区**(见下面的 SOKOBAN_MAP_RES_LIST):
 *        运行时由 sokoban.c 在选关时读进 RAM 缓冲再解压, 这里不再持有地图指针 ——
 *        否则 50 张地图会被链接器一起带进内部 Flash(约 3KB, 而游戏固件只剩几百字节余量)。
 */
typedef struct {
    uint8_t rows;       /* 行数 */
    uint8_t cols;       /* 列数 */
} SokobanLevel;

/**
 * @brief 关卡地图资源清单 (50 张, 定义在 sokoban_maps.c, 非 static)
 *        flash_store.c 的 s_resources[] 用下面的 X 宏把它们登记成外部 Flash 资源;
 *        游戏模式只按 FLASH_STORE_ID_SOK_MAP_0 + 序号 读取, 不直接引用数组本体。
 */
#define SOKOBAN_MAP_RES_LIST(X) \
    X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) \
    X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) \
    X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) \
    X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) \
    X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50)

#define SOKOBAN_MAP_DECL(n)  extern const uint8_t sok_map##n[];
SOKOBAN_MAP_RES_LIST(SOKOBAN_MAP_DECL)
#undef SOKOBAN_MAP_DECL

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
 * @brief 教程(自动演示)解法数据 (sokoban_maps.c, 工具/推箱子求解/gen_demo_data.py 生成)
 *        sok_demo_off[i]..sok_demo_off[i+1] 是第 i 关的步号区间;
 *        sok_demo_bits 每步 2bit(上0/下1/左2/右3, 高位在前), 每关按字节对齐。
 *        ★ 固件运行时**不引用** sok_demo_bits(2.4KB 内部 Flash 放不下): 它由
 *          BTN_IMG_STORE_MODE 烧到外部 Flash 的 SOK_DEMO_ADDR, 进教程页时按关读一片。
 */
extern const uint16_t sok_demo_off[SOKOBAN_LEVEL_COUNT + 1];
extern const uint8_t  sok_demo_bits[];

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
void          sokoban_menu_settings_restore(void);  /* 从设置菜单返回到按钮面板 */
void sokoban_menu_settings_restore_full(void);      /* 从设置页(整屏绘制)返回: 整幅重画地图+按钮面板 */

#endif
