#ifndef __MENU_H__
#define __MENU_H__

#include "ch32v20x.h"
#include "lcd.h"
#include "sok_img_win.h"
#include "touch.h"
#include "battery.h"
#include "chinese_fonts.h"   /* 24x24中文字库声明 */

/* ========================= 类型定义 ========================= */

/* 主模式枚举 (模块模式切换) */
typedef enum {
    MAIN_MODE_VIDEO = 0,   /* 视频: 视频播放 */
    MAIN_MODE_MENU,        /* 主菜单界面 */
    MAIN_MODE_SOKOBAN,     /* 推箱子游戏 */
    MAIN_MODE_D2048,       /* 2048游戏 */
    MAIN_MODE_TETRIS,      /* 俄罗斯方块游戏 */
    MAIN_MODE_KLOTSKI,     /* 华容道游戏 */
    MAIN_MODE_FISH,        /* 大鱼吃小鱼游戏 */
} MainMode;

/* 配置驱动游戏菜单状态机 (d2048/klotski/tetris/fish 共用) */
typedef struct {
    uint8_t  playing;              /* 游戏进行中状态值 */
    uint8_t  exit;                 /* 退出菜单状态值 */
    uint8_t  restart;              /* 重新开始菜单状态值 */
    const uint8_t *exit_row_types; /* 退出菜单6行类型数组 */
    const uint8_t *exit_map;       /* 退出菜单5格→返回码映射表 */
    uint16_t y_threshold;          /* 底部上划起点Y阈值 (d2048/klotski/tetris=210, fish=220) */
    uint8_t  release_thresh;       /* 松手上划距离阈值 (tetris=25, 其余=10) */
    uint16_t bat_x, bat_y;         /* 重菜静止时电池图标位置; 0xFFFF=不绘制 */
    void (*mark_dirty)(void);      /* 置游戏自身背景脏标记(关闭/取消菜单后需重绘场景) */
    uint8_t (*bg_redraw)(void);    /* 依据自身脏标记重绘菜单背景(场景/结束画面), 返回1=本帧已重绘 */
    void (*on_to_exit)(void);      /* 重菜下滑打断进退出菜单后的游戏侧补充处理(可为NULL) */
} GameMenuCfg;

/* ===================== 常量: 尺寸/坐标 ===================== */

/* 主菜单尺寸常量 (布局 & 位置计算) */
#define MENU_W      150  /* 菜单总宽度 */
#define MENU_H      230  /* 菜单高度 (5行菜单) */
#define MENU_BDR    5    /* 边框宽度 */
#define MENU_X      ((240 - MENU_W) / 2)  /* 菜单左上角X */
#define MENU_Y      ((240 - MENU_H) / 2)  /* 菜单左上角Y */

/* "重新开始"菜单尺寸 (3行: "重新开始" + "设置" + "返回") */
#define RESTART_MENU_W  150  /* 菜单宽度 */
#define RESTART_MENU_H  160  /* 菜单高度 (3行, 与 VIDEO_MENU_H 必须一致)
                                ★ 130 时行距只有38px/命中区40px, 拿在手里点"设置"常被
                                  判成"返回"(触摸屏低位坐标会漂) → 加大到160: 行距45/命中区50 */
#define RESTART_MENU_BDR 5   /* 边框宽度 */
#define RESTART_MENU_X  ((240 - RESTART_MENU_W) / 2)  /* 菜单左上角X (居中) */
#define RESTART_MENU_Y  (240 - RESTART_MENU_H)        /* 菜单贴底, 上划弹出, =110 */
#define RESTART_MENU_GAP 7             /* 行间距 (2行时使用, 3行时由内部计算) */
#define RESTART_MENU_DRAG_MAX_STEP 3   /* 上划拖拽每帧最大位移 (与视频菜单一致) */
#define RESTART_MENU_RELEASE_GAP 36    /* 松手后最小上划距离 */

/* 视频上滑菜单尺寸 */
#define VIDEO_MENU_W   150
#define VIDEO_MENU_H   160  /* 3行: 与 RESTART_MENU_H 一致(游戏的"重新开始/设置/返回"框) */
#define VIDEO_MENU_H4  188  /* 4行: 2*5 + 4*32 + 5*10 = 188 (同主菜单间距) */
#define VIDEO_MENU_H5  230  /* 5行: 2*5 + 5*32 + 6*10 = 230 (= MENU_H, 与主菜单同尺寸) */
#define VIDEO_MENU_BDR 5
#define VIDEO_MENU_X   ((240 - VIDEO_MENU_W) / 2)
#define VIDEO_MENU_Y   (240 - VIDEO_MENU_H4)  /* 底部对齐, 4行顶部Y=72 */
#define VIDEO_MENU_Y5  (240 - VIDEO_MENU_H5)  /* 底部对齐, 5行顶部Y=10 */

/* "重新开始"菜单行Y坐标 */
#define RESTART_MENU_ROW0_Y (RESTART_MENU_Y + RESTART_MENU_BDR + RESTART_MENU_GAP)       /* 第1行Y ("重新开始") */
#define RESTART_MENU_ROW1_Y (RESTART_MENU_ROW0_Y + 32 + RESTART_MENU_GAP)                /* 第2行Y ("设置") */
#define RESTART_MENU_ROW2_Y (RESTART_MENU_ROW1_Y + 32 + RESTART_MENU_GAP)                /* 第3行Y ("返回") */

/* ====================== 常量: 颜色 ====================== */

/* 颜色定义 */
#define MENU_BORDER_COLOR  LCD_GREEN  /* 菜单边框色 */
#define MENU_BG_COLOR      LCD_CYAN   /* 菜单背景色 */
#define MENU_TEXT_COLOR    LCD_BRED   /* 菜单文字色 */
#define MENU_TEXT_BDR_COLOR LCD_BLUE  /* 文字边框色 */
#define MENU_TEXT_BDR_W     5         /* 边框线宽 */
#define MENU_TEXT_BDR_PAD   10        /* 文字距边距 */

/* ================= 常量: 滑动/拖拽阈值 ================= */

#define MENU_SWIPE_START_Y      40 /* 下滑菜单触发区域Y上限 */
#define MENU_SWIPE_TRIGGER_DIST  5 /* 开始拖拽所需滑动距离 */
#define MENU_SWIPE_COMMIT_DIST  20 /* 松手确认展开所需滑动距离 */
#define MENU_SLIDE_SPEED          3 /* 菜单自动滑动速度 (每帧像素) */
#define MENU_DRAG_MAX_STEP        4 /* 下滑菜单拖拽时每帧最大位移 */
/* 注: 曾加过 MENU_DRAG_MAX_LAG(拖拽最大滞后, 让快滑更跟手), 菜单观感变差已回退;
   同一做法在"鱼拖动""亮度条"上仍保留(它们更需要跟手, 见开发注意事项 §24) */
#define MENU_RELEASE_GAP         36 /* 松手后菜单最小下滑距离 */
#define RESTART_MENU_SLIDE_SPEED 2  /* 上滑菜单移动速度 (每帧像素, 与视频菜单一致) */

/* ================= 常量: 清屏宏 ================= */

/* 菜单区域快捷清屏 (DMA 低阻塞, 清除色可被游戏设为背景色, 避免切菜单黑闪) */
#define LCD_CLEAR_MENU_AREA() \
    LCD_Fill_DMA(MENU_X, 0, MENU_X + MENU_W, MENU_Y + MENU_H, g_menu_clear_color)
#define LCD_CLEAR_RESTART_AREA() \
    LCD_Fill_DMA(RESTART_MENU_X, g_restart_slide_y, \
             RESTART_MENU_X + RESTART_MENU_W, 240, g_menu_clear_color)

/* ================= 常量: 菜单行类型 ================= */

/* 5行动态菜单行类型 (调用 menu_draw_5row_frame_only) */
#define MENU_ROW_2048    0  /* "2048" 4位数字 */
#define MENU_ROW_TUI     1  /* "推箱子" */
#define MENU_ROW_SHIPIN  2  /* "视频" */
#define MENU_ROW_FANHUI  3  /* "返回" */
#define MENU_ROW_TETRIS  4  /* 俄罗斯方块T图标 */
#define MENU_ROW_KLOTSKI   5  /* 华容道4x4图标 */
#define MENU_ROW_FISH      6  /* 大鱼吃小鱼图标 */
#define MENU_ROW_CHONGXIN  7  /* "重新开始" 4字 */
#define MENU_ROW_LDSZ      8  /* "亮度设置" 4字 */
#define MENU_ROW_XPSJ      9  /* "息屏时间" 4字 */
#define MENU_ROW_PMFX     10  /* "屏幕方向" 4字 */
#define MENU_ROW_SHEZHI   11  /* "设置" 2字 */
#define MENU_ROW_YYSH     12  /* "语言设置" 4字 */
#define MENU_ROW_MAX      12  /* 行类型最大值(双语文字表用) */

/* ================= 常量: 返回值/命令 ================= */

/* 游戏退出菜单返回值 */
#define EXIT_RET_NONE     0
#define EXIT_RET_VIDEO    2
#define EXIT_RET_D2048    3
#define EXIT_RET_SOKOBAN  4
#define EXIT_RET_TETRIS   5
#define EXIT_RET_KLOTSKI  6
#define EXIT_RET_FISH     7   /* 大鱼吃小鱼 */
#define EXIT_RET_SETTINGS 8   /* 游戏内点击"设置"按钮 */
#define EXIT_RET_BACK     0xFF  /* 内部"关闭菜单"信号 */

/* 主菜单动作返回值 */
#define MAIN_MENU_ACT_NONE    0
#define MAIN_MENU_ACT_D2048   1
#define MAIN_MENU_ACT_SOKOBAN 2
#define MAIN_MENU_ACT_TETRIS  3
#define MAIN_MENU_ACT_KLOTSKI 4
#define MAIN_MENU_ACT_FISH    5
#define MAIN_MENU_ACT_VIDEO   6

/* game_menu_command 返回命令 */
#define GAME_MENU_CMD_NONE      0   /* 菜单已处理, 调用方 return 0 */
#define GAME_MENU_CMD_RETURN    1   /* 关闭菜单(点空白/返回条): 已回 g_state_before_menu, 调用方 return 0 */
#define GAME_MENU_CMD_INTERACT  2   /* 需返回 GAME_RET_INTERACT(上划切到重菜/重菜"返回") */
#define GAME_MENU_CMD_SETTINGS  3   /* 点"设置": 调用方 menu_enter_game_settings() 并 return EXIT_RET_SETTINGS */
#define GAME_MENU_CMD_RESTART   4   /* 点"重新开始": 已清菜单, 调用方 xxx_init() 并 return GAME_RET_INTERACT */
#define GAME_MENU_CMD_SWITCH    5   /* 切换游戏: *out_ret = EXIT_RET_*, 调用方 return *out_ret */

/* 供各游戏定义自身 GameMenuCfg 时使用的电池位置哨兵 */
#define GAME_MENU_BAT_NONE 0xFFFF

/* ===================== 外部数组/变量 ===================== */

/* 重新开始菜单行类型数组 (3行: "重新开始" + "设置" + "返回") */
extern const uint8_t g_restart_row_types[3];

/* 视频上滑设置菜单行类型数组 (5行: 亮度设置/息屏时间/屏幕方向/语言设置/返回) */
extern const uint8_t g_video_menu_row_types[5];

/**
 * @brief 语言切换后让菜单字体缓存失效(下次绘制按当前语言重载, 见 menu.c)
 */
void Menu_FontCacheInvalidate(void);

/* 游戏退出/重新开始菜单共用变量 */
extern uint8_t  g_touch_down;
extern uint16_t g_touch_start_y;
extern uint8_t  g_menu_cooldown;
extern uint8_t  g_exit_menu_visible;
extern uint8_t  g_exit_dragging;
extern uint8_t  g_exit_sliding;
extern uint8_t  g_state_before_menu;     /* 进入菜单前的游戏状态 */
extern int16_t  g_exit_drag_y;           /* 退出菜单拖拽Y偏移 */
extern uint16_t g_exit_drag_start_y;     /* 退出菜单拖拽起始触摸Y */
extern uint8_t  g_restart_menu_visible;  /* 重新开始菜单可见 */
extern uint8_t  g_restart_sliding;       /* 重新开始菜单滑动中 */
extern uint8_t  g_restart_dragging;      /* 重新开始菜单拖拽中 */
extern int16_t  g_restart_slide_y;       /* 重新开始菜单滑动Y位置 */

/* 菜单/重开区域清除色 (游戏可设为自身背景色, 避免清菜单黑闪) */
extern uint16_t g_menu_clear_color;
extern uint8_t  g_lowbatt_showing;   /* 低电图标正显示中(亮段), 菜单重绘跳过电池区 */

/* 从游戏进入设置的标志 (MAIN_MODE_VIDEO=无, 其他=来源游戏模式) */
extern uint8_t  g_settings_from_game;

/* 视频设置菜单状态 (main.c定义, 游戏设置路径直接访问) */
extern uint8_t  s_video_menu_visible;
extern int16_t  s_video_menu_slide_y;
extern uint8_t  vm_touch_down;

/* 设置菜单中顶部下滑触发: GameSettings_Update设置, 各游戏update检查 */
extern uint8_t g_game_enter_exit_menu;

/* 共享触摸状态 (从各游戏静态提升为全局, 供 game_menu_command 复用) */
extern uint8_t  g_prev_pressed;
extern uint16_t g_touch_start_x;

/* 反向滑关闭后的松手吸收标志: 游戏移动判定处检查, 置1时吸收本次松手不误触移动 */
extern uint8_t  g_game_consume_release;

/* ===================== 函数声明 ===================== */

/* ---- 随机数/电池图标 ---- */
uint32_t game_rand(void);
/**
 * @brief  复位触摸状态(只操作 menu.h 的全局: g_prev_pressed / g_touch_down /
 *         g_touch_start_x / g_touch_start_y / g_menu_cooldown)
 * @note   d2048 与 klotski 原本各有一份逐字节相同的实现, 已合并到这一份
 */
void game_reset_touch_common(void);

void game_battery_icon_reset(void);
void game_battery_icon_update(uint16_t x, uint16_t y, uint8_t force_redraw);
BatteryLevel game_battery_icon_get_level(void);
void game_battery_set_charge_flash(uint8_t enable);

/* ---- 菜单边框/绘制 ---- */
/**
 * @brief  绘制菜单边框 (边框 + 颜色背景)
 *         公共工具函数, 不绘制菜单内容
 */
void menu_draw_frame(void);

/**
 * @brief  绘制主菜单 (5行: 2048/推箱子/俄罗斯方块/华容道/视频)
 */
void main_menu_draw(void);

/**
 * @brief  仅绘制主菜单边框和静态内容 (拖拽/下滑动画用, 不含动态元素)
 * @param  y_top  菜单顶部Y坐标
 */
void main_menu_draw_frame_only(int16_t y_top);

/**
 * @brief  通用n行动态菜单渲染 (仅边框, 用于主菜单/游戏退出菜单/重新开始菜单)
 * @param  y_top      菜单顶部Y坐标
 * @param  row_types  行类型描述数组 (MENU_ROW_*, n_rows个元素)
 * @param  n_rows     行数 (2=重新开始, 5=主菜单/退出菜单)
 */
void menu_draw_nrow_frame_only(int16_t y_top, const uint8_t *row_types,
                                uint8_t n_rows);

/**
 * @brief  绘制华容道图标 (4x4格子, 缺一角)
 * @param  y  行绘制Y坐标
 */
void menu_draw_klotski_icon(uint16_t y);

/**
 * @brief  绘制俄罗斯方块 T 字图标
 * @param  y  行绘制Y坐标
 */
void menu_draw_tetris_icon(uint16_t y);

/**
 * @brief  绘制大鱼吃小鱼图标
 * @param  y  行绘制Y坐标
 */
void menu_draw_fish_icon(uint16_t y);

/* ---- 菜单拖拽/滑动轮询 ---- */
/**
 * @brief  游戏退出菜单拖拽跟手轮询 (每个游戏复用)
 * @param  drag_y       拖拽位置 (输入传出, -MENU_H ~ MENU_Y)
 * @param  drag_touch_y 上一帧触摸Y (输入传出)
 * @param  dragging     拖拽中标志 (输出: 释放后清零)
 * @param  ts           当前触摸状态
 * @param  row_types    5行行类型数组
 * @return 0=拖拽中(已处理), 1=已释放(调用方处理触摸切换逻辑)
 */
uint8_t game_menu_drag_poll(int16_t *drag_y, uint16_t *drag_touch_y,
                             uint8_t *dragging, const TouchState *ts,
                             const uint8_t row_types[6]);

/**
 * @brief  重置 game_menu_drag_poll 内部状态，确保下次拖拽从干净状态启动
 */
void game_menu_drag_reset(void);

/**
 * @brief  重新开始菜单的上滑拖拽轮询
 * @return 0=拖拽中, 1=已释放-提交, 2=已释放-取消
 */
uint8_t game_restart_menu_drag_poll(int16_t *slide_y, uint16_t drag_start_y,
                                     uint8_t *dragging, const TouchState *ts);

/**
 * @brief  检测重新开始菜单中的底部上滑手势
 * @return 1 表示检测到有效上滑, 0 表示未检测到
 */
uint8_t game_restart_menu_down_poll(const TouchState *ts, uint8_t *touch_down,
                                    uint16_t *start_x, uint16_t *start_y);

/**
 * @brief  视频上滑设置菜单拖拽轮询
 * @param  slide_y     菜单顶部Y坐标 (输入传出)
 * @param  drag_start_y 拖拽起始触摸Y
 * @param  dragging    拖拽中标志 (输入传出)
 * @param  ts          当前触摸状态
 * @return 0=拖拽中(已处理), 1=已释放
 */
uint8_t video_menu_drag_poll(int16_t *slide_y, uint16_t drag_start_y,
                              uint8_t *dragging, const TouchState *ts);

/**
 * @brief  检测视频模式下的上滑手势 (底部区域)
 * @return 1 表示检测到有效上滑, 0 表示未检测到
 */
uint8_t video_menu_swipe_up_poll(const TouchState *ts, uint8_t *touch_down,
                                  uint16_t *start_x, uint16_t *start_y);

/* ---- 菜单切换/清理 ---- */
/**
 * @brief  按拖拽位置清退出菜单可见区域
 * @param  y  当前拖拽Y
 */
void menu_clear_exit_area_to(int16_t y);

/**
 * @brief  统一重启菜单关闭清理
 */
void menu_restart_cleanup(void);

/**
 * @brief  上划菜单下滑打断→进入退出菜单
 * @param  state       游戏状态指针
 * @param  exit_state  退出菜单状态值
 * @param  touch_y     当前触摸Y
 */
void game_menu_restart_to_exit(uint8_t *state, uint8_t exit_state,
                                uint16_t touch_y);

/**
 * @brief  底部上划打断退出菜单→切换重启菜单
 * @param  y_threshold   触摸起点Y阈值
 * @param  release_thresh 释放滑动距离阈值
 * @return 0=无动作 1=拖拽 2=滑入
 */
uint8_t menu_swipe_to_restart_handler(uint16_t y_threshold, uint8_t release_thresh);

/**
 * @brief  数字分解为十进制位
 * @param  val     数值
 * @param  digits  输出缓冲(个位在前)
 * @return 位数
 */
uint8_t uint_to_digits(uint32_t val, uint8_t *digits);

/**
 * @brief  统一退出菜单关闭清理
 */
void menu_exit_back_cleanup(void);

/**
 * @brief  中间区域上划关闭"下滑"菜单(退出/主菜单), 起点在[10,y_threshold)才判定
 * @param  y_threshold    判定起点Y上限
 * @param  release_thresh 释放滑动距离阈值
 * @return 1=已检测到并处理, 0=未检测到
 */
uint8_t game_menu_swipe_mid_up_close(uint16_t y_threshold, uint8_t release_thresh);

/**
 * @brief  中间/底部区域下滑关闭"上滑"菜单(设置菜单/二级菜单), 起点需>=start_min
 * @param  start_min      判定起点Y下限
 * @param  release_thresh 释放滑动距离阈值
 * @return 1=已检测到并处理, 0=未检测到
 */
uint8_t game_menu_swipe_mid_down_close(uint16_t start_min, uint8_t release_thresh);

/**
 * @brief  反向滑关闭菜单后修正共享触摸状态, 使松手不传入游戏(重录起点/清冷却)
 */
void game_menu_fix_release_after_close(void);

/* ---- 菜单碰撞/处理 ---- */
/**
 * @brief  按"视觉行中线"把触摸 Y 归属到菜单某一行 (触摸区无缝隙, 显示保留行间距)
 * @param  y_top   菜单框顶部Y (静止态: 3行=RESTART_MENU_Y, 5行=MENU_Y/VIDEO_MENU_Y5)
 * @param  y       触摸Y
 * @param  n_rows  行数 (2~6)
 * @return 行索引 (0-based); -1 表示点在菜单框上方(点空白关闭)
 * @note   行几何与 menu_draw_nrow_frame_only 完全一致(row_gap=(mh-2*mb-n*32)/(n+1)),
 *         行与行之间那半个行距的空隙按"最近行"各分一半 → 触摸区连续、没有无归属的缝隙,
 *         行边界正好落在相邻两行文字的视觉中线上(与眼睛看到的行中心对齐);
 *         框内上下边框带及菜单框下沿到屏幕最下(y=239)都夹到最近行。
 */
int8_t menu_row_from_y(uint16_t y_top, uint16_t y, uint8_t n_rows);

/**
 * @brief  菜单碰撞检测 (主菜单/退出菜单, 5行, 用 MENU_X/MENU_W/MENU_Y 几何)
 * @note   行归属见 menu_row_from_y; 只有点在菜单框上方或左右外侧才返回 -1
 * @param  x, y        触摸坐标
 * @param  item_count  菜单项数量 (行数)
 * @return 命中的菜单项索引 (0-based), -1 表示点在菜单框上方或左右外侧
 */
int8_t menu_hit_test(uint16_t x, uint16_t y, uint8_t item_count);

/**
 * @brief  退出菜单触摸轮询 (通用: 4个游戏共用重复的代码)
 *         按下时记录菜单行，松手仍在同一行才触发
 * @param  ret_map       hit索引(0-3)到返回值的映射表 (4元素数组)
 * @param  prev_pressed  上一帧触摸状态指针
 * @param  out_ts        当前帧触摸输出 (用于"关闭"按钮测距)
 * @return EXIT_RET_NONE(0) = 无操作
 *         EXIT_RET_BACK(0xFF) = 点击"关闭"
 *         其他 = EXIT_RET_* 切换目标
 */
uint8_t game_exit_menu_poll(const uint8_t ret_map[5], uint8_t *prev_pressed,
                            TouchState *out_ts);

/**
 * @brief  主菜单触摸处理 + 模式切换 (5行: 2048/推箱子/俄罗斯方块/华容道/视频)
 *         封装 menu_hit_test + 模式切换 (LCD_Clear/菜单/游戏初始化)
 * @param  x, y        触摸坐标
 * @param  mode        当前模式指针 (s_main_mode)
 * @param  menu_drawn  菜单已绘制标志指针
 * @param  game_inited 游戏已初始化标志指针
 * @param  touch_down  触摸按下状态指针
 * @param  key_resp    按键响应标志指针
 * @return 1 = 已处理, 0 = 未处理
 */
uint8_t main_menu_handle(uint16_t x, uint16_t y,
                          MainMode *mode, uint8_t *menu_drawn,
                          uint8_t *game_inited, uint8_t *touch_down,
                          uint8_t *key_resp);

/**
 * @brief  统一游戏返回值处理 (4个游戏共用)
 *         根据 GAME_RET_* 来执行 LCD清屏/模式切换/视频恢复
 * @param  ret        游戏 update 返回值
 * @param  mode       s_main_mode 指针
 * @param  game_inited s_game_inited 指针
 * @param  touch_down s_touch_down 指针
 * @param  key_resp   key_response 指针
 * @return 原值 ret
 */
uint8_t game_handle_ret(uint8_t ret, MainMode *mode, uint8_t *game_inited,
                         uint8_t *touch_down, uint8_t *key_resp);

/* ---- 游戏滑动触发菜单的公共函数 ---- */
/**
 * @brief  检测底部上划打断退出菜单 (d2048/tetris/klotski 共用)
 *         内部维护 g_touch_down/g_touch_start_y，检测底部到顶部滑动
 * @param  y_threshold    触摸起点Y阈值 (d2048/klotski=220, tetris=210)
 * @param  release_thresh 释放上划距离阈值 (d2048/klotski=10, tetris=25)
 * @param  dy_out         释放路径的输出滑动距离
 * @return 0=无动作, 1=拖拽中断(已清屏), 2=释放中断(已清屏)
 */
uint8_t game_menu_swipe_up_poll(uint16_t y_threshold, uint8_t release_thresh,
                                int16_t *dy_out);

/**
 * @brief  顶部下滑拖拽中触发退出菜单
 * @param  ts            当前触摸状态
 * @param  state         游戏状态变量指针
 * @param  playing_state 游戏的PLAYING状态值
 * @param  exit_state    游戏的EXIT_MENU状态值
 * @return 1=已进入退出菜单拖拽模式, 0=未触发
 */
uint8_t game_menu_enter_exit_drag(const TouchState *ts, uint8_t *state,
                                   uint8_t playing_state, uint8_t exit_state);

/**
 * @brief  底部上划拖拽中触发重新开始菜单
 * @param  ts             当前触摸状态
 * @param  state          游戏状态变量指针
 * @param  playing_state  游戏的PLAYING状态值
 * @param  restart_state  游戏的RESTART_MENU状态值
 * @param  y_threshold    底部触摸起点Y阈值 (d2048/klotski=220, tetris=210)
 * @return 1=已进入重新开始菜单拖拽模式, 0=未触发
 */
uint8_t game_menu_enter_restart_drag(const TouchState *ts, uint8_t *state,
                                      uint8_t playing_state, uint8_t restart_state,
                                      uint16_t y_threshold);

/**
 * @brief  松手时尝试提交退出菜单
 * @param  dy          触摸Y总位移 (ts.y - g_touch_start_y)
 * @param  state       游戏状态变量指针
 * @param  exit_state  游戏的EXIT_MENU状态值
 * @return 1=已提交退出菜单滑动, 0=未触发
 */
uint8_t game_menu_commit_exit(int16_t dy, uint8_t *state, uint8_t exit_state);

/**
 * @brief  松手时尝试提交重新开始菜单
 * @param  dy            触摸Y总位移
 * @param  state         游戏状态变量指针
 * @param  restart_state 游戏的RESTART_MENU状态值
 * @param  y_threshold   底部触摸起点Y阈值
 * @param  swipe_dist    上划距离阈值 (d2048/klotski=10, tetris=25)
 * @return 1=已提交重新开始菜单滑动, 0=未触发
 */
uint8_t game_menu_commit_restart(int16_t dy, uint8_t *state, uint8_t restart_state,
                                  uint16_t y_threshold, uint8_t swipe_dist);

/* ---- 进入设置/统一状态机 ---- */
/**
 * @brief  游戏进入设置: 保存来源并打开视频设置菜单
 */
void menu_enter_game_settings(void);

/**
 * @brief  游戏"二次上划"进入设置并跟手滑出 (推箱子用, 复用 VideoMenu_Update 拖拽状态机)
 * @param  start_y  上划起点Y
 */
void menu_enter_game_settings_swipe(uint16_t start_y);

/**
 * @brief  配置驱动游戏退出/重新开始菜单统一状态机 (d2048/klotski/tetris/fish 共用)
 * @param  cfg       菜单配置 (状态枚举/行类型/映射/阈值/回调)
 * @param  state     游戏状态指针 (进入时为 EXIT/RESTART 状态, 内部会切回 PLAYING/返回值)
 * @param  out_ret   切换游戏时输出 EXIT_RET_* 返回值
 * @return GAME_MENU_CMD_*
 */
uint8_t game_menu_command(const GameMenuCfg *cfg, uint8_t *state, uint8_t *out_ret);

#endif
