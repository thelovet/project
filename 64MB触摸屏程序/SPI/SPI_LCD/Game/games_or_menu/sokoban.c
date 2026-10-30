/**
 * @file    sokoban.c
 * @brief   推箱子游戏模块
 * @note    地图元素: 0=空地 1=墙 2=目的地 3=箱子 4=玩家 5=箱子在目的地上
 *          地图滚动, 触摸手势控制, 拖拽窗口
 */

#include "sokoban.h"
#include "timer.h"
#include "lcd.h"
#include "string.h"
#include "touch.h"
#include "menu.h"
#include "display.h"
#include "sok_img_win.h"
#include "sok_img_prev.h"
#include "sok_img_restart.h"
#include "sok_img_next.h"
#include "sok_img_undo.h"  /* 撤销界面按钮图(外部Flash保留区, 1:1 显示) */
#include "flash_store.h"   /* 图像资源从外部Flash读取 */
#include "settings.h"    /* g_language_index: pick button set by language */

#define SOK_BATTERY_X  108
#define SOK_BATTERY_Y    2

 /* 地图数据结构 */
static uint8_t  g_runtime_map[SOK_MAX_CELLS];

/* 当前关卡的地图 BCD 原数据(RAM 缓冲): 地图"真身"在外部 Flash(资源区), 选关时读进来 ——
   只给 is_orig_target() 判断原始目标点用; 游戏过程用的是上面解压后的 g_runtime_map。
   最大的一关是 14x15 → 105 字节, 128 足够。 */
static uint8_t  g_level_bcd[128];
static uint8_t  s_bcd_level = 0xFF;   /* 缓冲里装的是第几关(0xFF=还没装) */
static uint8_t  g_level_rows, g_level_cols;
static int16_t  g_ox, g_oy;
static uint16_t g_move_count;
static uint16_t g_tile_size;              /* 每格尺寸 (动态缩放计算) */
static const SokobanLevel* g_cur_level;   /* 当前关卡 (只读, 指向关卡数组) */

/* 下方3个操作按钮: 上一关/重新开始/下一关 */
#define SOK_BTN1_X      15     // 上一关 X
#define SOK_BTN2_X      90     // 重新开始 X
#define SOK_BTN3_X     165     // 下一关 X
#define SOK_BTN_W       60     // 按钮宽度
#define SOK_BTN_H       60     // 按钮高度
#define SOK_BTN_Y      160     // 按钮起始Y
#define SOK_BTN_HIT_Y0 140     // 按钮命中区Y起点 (按钮上边 160 - 20)
#define SOK_BTN_HIT_Y1 240     // 按钮命中区Y终点: 判定是 y>=此值则不算, 所以 240 = 直到 y=239(屏幕最底)
/* 横向分界 = 相邻按钮中线, 让三个命中区"无缝"(不留死区)、且都以按钮为中心(与撤销组做法一致) */
#define SOK_BTN_HIT_SPLIT1  82   // (75+90)/2  = 82.5 -> 上一关/重新开始 的分界
#define SOK_BTN_HIT_SPLIT2 157   // (150+165)/2 = 157.5 -> 重新开始/下一关 的分界
#define SOK_SWIPE_Y    230     // 滑动检测: 下方区域Y阈值 (当Y>此值显示菜单)
#define SOK_SLIDE_MAX_STEP   2  // 跟手拖拽每帧最大步进(防跳变)
#define SOK_SLIDE_SPEED      2  // 松手后自动滑动速度(px/帧)

/* 长按与滑动阈值 */
#define SOK_HOLD_THRESHOLD    50 // 长按移动阈值(像素)
#define SOK_RELEASE_THRESHOLD 10 // 松手滑动阈值(像素)

/*关卡切换阈值*/
#define HOLD_START_US      200000UL   /* 长按启动阈值 (~200ms, 微秒) */
#define HOLD_REPEAT_US     130000UL    /* 长按重复间隔 (~130ms, 微秒) */
#define HOLD_COOLDOWN_FRAMES 100       /* 长按自动切关后菜单冷却帧数(防重复触发) */

/* 退出菜单状态 (MENU_* 定义在 sokoban.h) */
static uint8_t  s_exit_menu_visible = 0;
static uint8_t  s_exit_sliding;       /* 滑动动画进行中 */
static int16_t  s_exit_drag_y;        /* 退出菜单Y坐标 */
static uint8_t  s_exit_dragging;      /* 拖拽中标志 */
static uint16_t s_exit_drag_start_y;  /* 拖拽起点Y */

/* 退出菜单 (视频/2048/俄罗斯方块/华容道/返回, 减当前=推箱子) */
static const uint8_t sokoban_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_D2048, EXIT_RET_TETRIS, EXIT_RET_KLOTSKI, EXIT_RET_FISH
};

/* 游戏状态与菜单状态 */
static uint8_t  s_won = 0;
static uint8_t  s_menu_visible = 0;
static uint8_t  s_menu_sliding = 0;    /* 跟手滑动中 */
static int16_t  s_menu_slide_y = 240;  /* 跟手滑动: 按钮面板顶部Y */
static uint8_t  s_menu_slide_anim = 0;    /* 松手后自动滑动动画中 */
static uint8_t  s_menu_from_swipe = 0;    /* 菜单是否由上划手势进入(区分level_init) */
static uint8_t  s_current_level = 0;
static uint8_t  s_level_num = 0;  /* 当前关卡号 (1-based) */
static uint8_t  g_sok_dirty = 0;  /* 0=干净, 1=全屏重绘, 2=电池闪灭仅局部 */

/* 退出菜单行类型 (视频/2048/俄罗斯方块/华容道/返回) */
static const uint8_t sok_row_types[5] = {
    MENU_ROW_SHIPIN, MENU_ROW_2048, MENU_ROW_TETRIS, MENU_ROW_KLOTSKI, MENU_ROW_FISH
};

/* 触摸输入状态 */
static uint8_t  s_prev_pressed = 0;
static uint16_t s_touch_start_x, s_touch_start_y;
static uint16_t s_swipe_origin_y;   // 滑动起始Y坐标(用于退出菜单手势检测)
static uint8_t  s_menu_cooldown = 0; // 菜单冷却计数, 防止误触重复打开
static uint8_t  s_hold_button = 0;  // 长按按钮: 0=无, 1=上一关, 3=下一关
static uint32_t s_hold_us = 0;  // 长按累计时间(us)
static uint32_t s_hold_last_us = 0;  // 长按上次计时基准(us)
static uint8_t  s_drag_btn = 0;  // 拖拽追踪: 0=无, 1=上一关, 2=重新开始, 3=下一关
static uint8_t  s_won_transition = 0;  // WON切关后进入menu_visible的标记, 防松手再切
static uint8_t  s_btn_max_y = 0;  // 按钮栏拖拽最远Y, 松手回退>5px取消
static int16_t  s_slide_max_dy = 0;  // 上划拖拽最远位移, 松手回退>2px取消

/* ---- 二级菜单(3行: 撤销/设置/返回), 由一级按钮面板"二次上划"跟手拉出 ---- */
static uint8_t  s_lvl2_visible = 0;
/* ---- 跟手上划拉出的面板种类: 0=导航按钮组(停 y=SOK_BTN_Y), 1=二级菜单(停 y=RESTART_MENU_Y) ----
   两者共用同一套跟手滑动状态(s_menu_sliding/s_menu_slide_y), 靠 s_panel_kind 区分画什么 */
static uint8_t  s_panel_kind = 0;
static int16_t  s_panel_rest = SOK_BTN_Y;   /* 当前面板停住时的顶部Y */
static const uint8_t sok_lvl2_row_types[3] = {
    MENU_ROW_CHEXIAO, MENU_ROW_SHEZHI, MENU_ROW_FANHUI
};

/* ---- 撤销界面: 1:1(30x31) 三个按钮 上一步/返回/下一步 ----
   位置按"底边 220 不变、左钮右缘 75 不变、右钮左缘 165 不变"排:
   一级按钮 (2x, 60x62) 在 x=15/90/165, y=160; 撤销按钮 (1x, 30x31) 在下面这三个位置。
   触摸判定: 横向按"相邻按钮中线"(90/150, 见 SOK_UNDO_HIT_SPLIT1/2) 分, 让三个区都以按钮为中心
   (旧写法直接拿 X2/X3 当分界 —— 那俩是按钮左缘, 中间的"返回"会整体右偏 15px)。 */
#define SOK_UNDO_X1     45
#define SOK_UNDO_X2     105
#define SOK_UNDO_X3     165
#define SOK_UNDO_Y      189            /* = 220 - 31 */
/* 撤销按钮的触摸判定: 单独一套 —— 按钮只有 31px 高, 若沿用一级按钮那种"上方多放宽
   几十像素"的带, 上面一大片空白都会算成点按钮(中间的"返回"尤其容易误触)。
   这里只在上方放宽 10px, 下方一直到底边(与一级按钮一致)。 */
#define SOK_UNDO_HIT_Y0 179            /* = 189 - 10 */
#define SOK_UNDO_HIT_SPLIT1  90   /* 横向分界: 相邻按钮中线 (75+105)/2, 使三个区都以按钮为中心 */
#define SOK_UNDO_HIT_SPLIT2 150   /* 横向分界: (135+165)/2 (画图坐标 105/165 是按钮左缘, 直接用会让
                                     中间<返回>的区整体右偏15px) */
#define SOK_UNDO_HIT_Y1 240            /* 到底边(判定 y>=此值不算, 240 = 含 y=239) */
static uint8_t  s_undo_group = 0;      /* 1 = 撤销模式: 上划显示撤销按钮组(而不是导航按钮) */
static uint8_t  s_undo_visible = 0;    /* 撤销按钮组当前显示中 */

/* ---- 撤销/重做历史: 每步 1 字节(低2位=方向, bit2=这步推了箱子), 环形缓冲 64 步 ----
   方向与 SokobanDir 同序(上0/下1/左2/右3); 撤回/重做都靠"当前玩家位置 + 这 1 字节"反推/正推,
   所以除了这 64 字节不需要再存别的状态(见开发注意事项 §38)。
   环形: 满了覆盖最旧一条(不用搬数据), 保证永远能撤最近 64 步 */
#define SOK_HIST_MAX    64
static uint8_t  s_hist[SOK_HIST_MAX];
static uint8_t  s_hist_cnt = 0;        /* 有效步数(<=64) */
static uint8_t  s_hist_head = 0;       /* 最旧一条的下标 */
static uint8_t  s_hist_pos = 0;        /* 当前进度(0..cnt); cnt-pos = 可重做步数 */

static void sokoban_draw_cell_at(uint8_t row, uint8_t col);
static void sok_draw_undo_buttons(void);      /* 撤销按钮组(定义在下方) */
static void sok_show_nav_group(void);         /* 显示一级导航按钮组(定义在下方) */
static void sok_redraw_cell_ui(uint8_t r, uint8_t c);  /* 重绘一格(跳过被撤销按钮盖住的) */
static void sok_redraw_nav_group(void);       /* 重绘一级导航按钮组+关卡编号(定义在下方) */
static void sok_draw_nav_buttons_at(int16_t y);   /* 导航按钮组绘制(定义在下方) */
static void sok_redraw_battery_tiles(int16_t x0, int16_t y0, int16_t x1, int16_t y1); /* 按矩形重绘地图瓦片(定义在下方) */
static void sok_hist_push(uint8_t step);      /* 记录一步撤销历史(定义在下方) */
static uint8_t sok_try_open_exit_menu(uint16_t y);  /* 上方区域下滑打开退出菜单(游戏内/二级菜单/撤销界面共用) */

/** @brief  重绘 skip_y 以下的游戏格子 (0~239), 含CS控制 */
static void sok_redraw_grid_below(int16_t y_skip) {
    uint8_t br, bc;
    LCD_CS_CLR;
    for (br = 0; br < g_level_rows; br++) {
        int16_t cy = g_oy + (int16_t)br * (int16_t)g_tile_size;
        if (cy + (int16_t)g_tile_size <= y_skip) continue;
        if (cy >= 240) break;
        for (bc = 0; bc < g_level_cols; bc++)
            sokoban_draw_cell_at(br, bc);
    }
    LCD_CS_SET;
}

/**
 * @brief  判断某格是否原始目标点
 */
static uint8_t is_orig_target(uint8_t row, uint8_t col) {
    uint16_t idx = row * g_level_cols + col;
    uint8_t b = g_level_bcd[idx / 2];      /* 地图数据已搬到外部 Flash, 存在 RAM 缓冲里 */
    uint8_t v = (idx & 1) ? (b % 10) : (b / 10);
    return (v == SOK_TARGET || v == SOK_BOX_ON_TARGET || v == SOK_PLAYER_ON_TARGET);
}

/**
 * @brief  缩放绘制30x30图片到 g_tile_size x g_tile_size (含调色板)
 * @note   原实现**每个像素**都做一次 `(c*30)/g_tile_size` —— CH32V203 没有硬件除法,
 *         一次 32 位无符号除法要几十上百周期, 一关 ~80 格 × 900 像素就是几十毫秒,
 *         长按连跳切关时明显卡。这里改成: 每次重绘只算一张"列→源列"映射表(30 项)再查表。
 */
static void sok_draw_scaled_tile(uint16_t dx, uint16_t dy, const void* img,
    const uint16_t* palette) {
    uint16_t r, c;
    /* 列映射表(与 g_tile_size 绑定, 同一关内只算一次) */
    static uint8_t  col_src[SOK_TILE_SIZE];
    static uint16_t col_src_for = 0;
    if (col_src_for != g_tile_size) {
        for (c = 0; c < g_tile_size; c++) {
            col_src[c] = (uint8_t)(((uint32_t)c * SOK_TILE_SIZE) / g_tile_size);
        }
        col_src_for = g_tile_size;
    }

    for (r = 0; r < g_tile_size; r++) {
        uint16_t sr = (uint16_t)(((uint32_t)r * SOK_TILE_SIZE) / g_tile_size);
        const uint8_t *p8  = (const uint8_t *)img;
        const uint16_t *p16 = (const uint16_t *)img;
        LCD_Address_Set(dx, dy + r, dx + g_tile_size - 1, dy + r);
        for (c = 0; c < g_tile_size; c++) {
            uint16_t index = sr * SOK_TILE_SIZE + col_src[c];
            uint16_t color = palette ? palette[p8[index]] : p16[index];
            LCD_WR_DATA_16Bit(color);
        }
    }
}

/**
 * @brief  缩放绘制精灵图片 (src_size x src_size 缩放后居中于30x30格内)
 */
static void sok_draw_scaled_sprite(uint16_t dx, uint16_t dy,
    const void* img, uint16_t src_size, uint16_t transp,
    const uint16_t* palette) {
    uint16_t ss = (uint16_t)(((uint32_t)g_tile_size * src_size) / SOK_TILE_SIZE);
    uint16_t off = (g_tile_size - ss) / 2;
    uint16_t r, c, s;

    for (r = 0; r < ss; r++) {
        uint16_t sr = (uint16_t)(((uint32_t)r * src_size) / ss);
        c = 0;
        while (c < ss) {
            uint16_t sc = (uint16_t)(((uint32_t)c * src_size) / ss);
            uint16_t idx = sr * src_size + sc;
            uint16_t color = palette
                ? palette[((const uint8_t*)img)[idx]]
                : ((const uint16_t*)img)[idx];
            if (color == transp) { c++; continue; }
            s = c;
            c++;
            while (c < ss) {
                sc = (uint16_t)(((uint32_t)c * src_size) / ss);
                idx = sr * src_size + sc;
                color = palette
                    ? palette[((const uint8_t*)img)[idx]]
                    : ((const uint16_t*)img)[idx];
                if (color == transp) break;
                c++;
            }
            LCD_Address_Set(dx + off + s, dy + off + r, dx + off + c - 1, dy + off + r);
            for (; s < c; s++) {
                uint16_t sc2 = (uint16_t)(((uint32_t)s * src_size) / ss);
                idx = sr * src_size + sc2;
                LCD_WR_DATA_16Bit(palette
                    ? palette[((const uint8_t*)img)[idx]]
                    : ((const uint16_t*)img)[idx]);
            }
        }
    }
}

/* 外部Flash图像加载辅助 (图像+调色板已烧录到外部Flash, 读入共享scratch) */

/**
 * @brief  加载"调色板+图像"到共享scratch
 * @param  pal_id   调色板资源ID
 * @param  pal_bytes 调色板字节数
 * @param  img_id   图像资源ID
 * @param  img_bytes 图像字节数
 * @param  pal      输出: 调色板指针(位于scratch起始)
 * @return 图像数据指针(位于调色板之后); 失败返回0
 */
static const void* sok_load_pal_img(FlashStoreId pal_id, uint16_t pal_bytes,
    FlashStoreId img_id, uint16_t img_bytes,
    const uint16_t** pal) {
    uint8_t* s = FlashStore_Scratch();
    if (FlashStore_ResRead(pal_id, 0, s, pal_bytes) != pal_bytes) return 0;
    if (FlashStore_ResRead(img_id, 0, s + pal_bytes, img_bytes) != img_bytes) return 0;
    *pal = (const uint16_t*)s;
    return s + pal_bytes;
}

/**
 * @brief  加载纯RGB565图像(无调色板)到共享scratch
 * @return 图像数据指针; 失败返回0
 */
static const void* sok_load_img(FlashStoreId img_id, uint16_t img_bytes) {
    uint8_t* s = FlashStore_Scratch();
    return (FlashStore_ResRead(img_id, 0, s, img_bytes) == img_bytes) ? s : 0;
}

/**
 * @brief  按需加载纯RGB565图像的前vis_rows行到共享scratch (裁剪绘制用)
 * @note   源图每行src_w个像素(2字节/像素)连续存放, 因此只读可见行即可,
 *         减少拖拽阶段每帧的外部Flash读取量(由整图降为可见行)
 * @return 图像数据指针(前vis_rows行); 失败返回0
 */
static const void* sok_load_img_rows(FlashStoreId img_id, uint16_t src_w, int16_t vis_rows) {
    uint8_t* s = FlashStore_Scratch();
    uint16_t bytes = (uint16_t)vis_rows * src_w * 2;
    return (FlashStore_ResRead(img_id, 0, s, bytes) == bytes) ? s : 0;
}

/**
 * @brief  按当前语言取一张导航按钮图的前 vis_rows 行 (整图绘制/裁剪绘制共用)
 * @param  which    0=上一关 1=下一关 2=重新开始
 * @param  vis_rows 需要的行数 (整图=31)
 * @param  src_w    源图宽 (三种按钮都是 30)
 * @return scratch 里的图像数据指针; 失败/没烧返回 0
 * @note   中文那套在字库资源里(ID 71~73); 英文那套在外部 Flash 的固定保留区
 *         BTN_IMG_EN_ADDR(见 flash_store.h), 由独立模式 BTN_IMG_STORE_MODE 烧进去 ——
 *         内部 Flash 装不下两套按钮图, 所以英文这套不进资源表/目录, 单独占一块保留区
 */
static const void* sok_btn_img_rows(uint8_t which, int16_t vis_rows, uint16_t src_w)
{
    uint16_t bytes = (uint16_t)vis_rows * src_w * 2;
    uint8_t* s = FlashStore_Scratch();

    if (g_language_index == 1) {
        uint32_t off = (which == 0) ? BTN_IMG_EN_PREV :
                       (which == 1) ? BTN_IMG_EN_NEXT : BTN_IMG_EN_RESTART;
        return (FlashStore_RawRead(BTN_IMG_EN_ADDR + off, s, bytes) == bytes) ? s : 0;
    }

    {
        FlashStoreId id = (which == 0) ? FLASH_STORE_ID_IMG_PREV :
                          (which == 1) ? FLASH_STORE_ID_IMG_NEXT : FLASH_STORE_ID_IMG_RESTART;
        return sok_load_img_rows(id, src_w, vis_rows);
    }
}

/**
 * @brief  整图绘制一张导航按钮 (按当前语言选图)
 */
static void sok_draw_btn(uint16_t x, uint16_t y, uint8_t which, uint16_t src_w)
{
    const void* img = sok_btn_img_rows(which, PREV_IMG_H, src_w);
    if (img) sok_nav_draw_rgb565(x, y, (const uint16_t*)img);
}



/* 分趟绘制用的格子类型位掩码 (bit = SOK_xxx 的值) */
#define SOK_M_BG     ((1u << SOK_FLOOR) | (1u << SOK_TARGET) \
                    | (1u << SOK_PLAYER) | (1u << SOK_PLAYER_ON_TARGET))
#define SOK_M_TARGET ((1u << SOK_TARGET) | (1u << SOK_PLAYER_ON_TARGET))
#define SOK_M_PLAYER ((1u << SOK_PLAYER) | (1u << SOK_PLAYER_ON_TARGET))

/**
 * @brief  按"图块类型"分趟绘制: 把本关属于 mask 的格子全画一遍
 * @param  mask  本趟要画的格子类型位掩码
 * @param  img   本趟的图 (已在scratch里, 由调用者从外部Flash读一次)
 * @param  pal   调色板 (0=纯RGB565图)
 * @param  sprite_size  0=按30x30瓦片铺满整格; 非0=精灵源尺寸(25), 用transp做透明色
 * @param  transp 精灵的透明色 (仅 sprite_size 非0时有效)
 * @note   一张图一次重绘只从外部 Flash 读一次, 而不是"每格读一次"。原来每格要读
 *         调色板+图(约1.1KB), 一关约200格就是 ~400 次 SPI 读/~230KB, 正是"长按连跳
 *         切关很慢、而且快慢不定"的原因(格子数/图种每关不同, 读的量就不同)。
 *         整关最多 16x16 格, 所以每趟扫一遍地图的开销可以忽略。
 */
static void sok_draw_pass(uint8_t mask, const void* img, const uint16_t* pal,
    uint16_t sprite_size, uint16_t transp) {
    uint8_t row, col;
    if (!img) return;
    for (row = 0; row < g_level_rows; row++) {
        for (col = 0; col < g_level_cols; col++) {
            uint8_t cell = g_runtime_map[row * g_level_cols + col];
            if (!(mask & (1u << cell))) continue;
            uint16_t ux = (uint16_t)(g_ox + (int16_t)col * (int16_t)g_tile_size);
            uint16_t uy = (uint16_t)(g_oy + (int16_t)row * (int16_t)g_tile_size);
            if (sprite_size) sok_draw_scaled_sprite(ux, uy, img, sprite_size, transp, pal);
            else             sok_draw_scaled_tile(ux, uy, img, pal);
        }
    }
}

/**
 * @brief  全屏绘制关卡到LCD, 自动计算缩放和居中
 * @param  level_index  关卡索引 (0 ~ SOKOBAN_LEVEL_COUNT-1)
 * @note   分 6 趟画: 底色 → 墙 → 箱子 → 箱子在目标上 → 目标点 → 玩家。
 *         前 4 趟是不透明的、格子互不重叠, 先后无所谓; 后 2 趟是透明精灵,
 *         必须按"先目标点后玩家"叠在底色之上(与逐格绘制的层次一致)。
 */
void sokoban_draw_level(uint8_t level_index) {
    if (level_index >= SOKOBAN_LEVEL_COUNT) return;

    const uint16_t* pal;
    const void* img;

    /* 清屏并绘制 (清黑走DMA: 逐像素写 240x240 要 ~50ms, 是长按切关卡顿的大头) */
    LCD_Fill_DMA(0, 0, LCD_W, LCD_H, LCD_BLACK);

    LCD_CS_CLR;
    /* 底色: 空地/目标点/玩家(含玩家在目标上)的底都是同一张地板图 */
    img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
        FLASH_STORE_ID_BG_IMG, 900, &pal);
    sok_draw_pass(SOK_M_BG, img, pal, 0, 0);

    img = sok_load_pal_img(FLASH_STORE_ID_WALL_PALETTE, 108,
        FLASH_STORE_ID_WALL_IMG, 900, &pal);
    sok_draw_pass(1u << SOK_WALL, img, pal, 0, 0);

    img = sok_load_pal_img(FLASH_STORE_ID_BOX_PALETTE, 350,
        FLASH_STORE_ID_BOX_IMG, 900, &pal);
    sok_draw_pass(1u << SOK_BOX, img, pal, 0, 0);

    img = sok_load_pal_img(FLASH_STORE_ID_BOX_ON_TARGET_PALETTE, 50,
        FLASH_STORE_ID_BOX_ON_TARGET_IMG, 900, &pal);
    sok_draw_pass(1u << SOK_BOX_ON_TARGET, img, pal, 0, 0);

    img = sok_load_pal_img(FLASH_STORE_ID_TARGET_PALETTE, 72,
        FLASH_STORE_ID_TARGET_IMG, 625, &pal);
    sok_draw_pass(SOK_M_TARGET, img, pal, SOK_TARGET_SIZE, 0x0000);

    img = sok_load_img(FLASH_STORE_ID_PLAYER_IMG, 1250);
    sok_draw_pass(SOK_M_PLAYER, img, 0, SOK_PLAYER_SIZE, 0xffff);
    LCD_CS_SET;

    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
}

/**
 * @brief  绘制地图中单个单元格 (与 draw_level 一致, 但仅一格的版本)
 */
static void sokoban_draw_cell_at(uint8_t row, uint8_t col) {
    uint16_t ux = (uint16_t)(g_ox + (int16_t)col * (int16_t)g_tile_size);
    uint16_t uy = (uint16_t)(g_oy + (int16_t)row * (int16_t)g_tile_size);
    uint8_t cell = g_runtime_map[row * g_level_cols + col];

    switch (cell) {
    case SOK_OUTER: break;
    case SOK_FLOOR: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
            FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_WALL: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_WALL_PALETTE, 108,
            FLASH_STORE_ID_WALL_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_BOX: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_BOX_PALETTE, 350,
            FLASH_STORE_ID_BOX_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_BOX_ON_TARGET: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_BOX_ON_TARGET_PALETTE, 50,
            FLASH_STORE_ID_BOX_ON_TARGET_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        break;
    }
    case SOK_TARGET: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
            FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        img = sok_load_pal_img(FLASH_STORE_ID_TARGET_PALETTE, 72,
            FLASH_STORE_ID_TARGET_IMG, 625, &pal);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_TARGET_SIZE, 0x0000, pal);
        break;
    }
    case SOK_PLAYER: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
            FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        img = sok_load_img(FLASH_STORE_ID_PLAYER_IMG, 1250);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_PLAYER_SIZE, 0xffff, 0);
        break;
    }
    case SOK_PLAYER_ON_TARGET: {
        const uint16_t* pal;
        const void* img = sok_load_pal_img(FLASH_STORE_ID_BG_PALETTE, 206,
            FLASH_STORE_ID_BG_IMG, 900, &pal);
        if (img) sok_draw_scaled_tile(ux, uy, img, pal);
        img = sok_load_pal_img(FLASH_STORE_ID_TARGET_PALETTE, 72,
            FLASH_STORE_ID_TARGET_IMG, 625, &pal);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_TARGET_SIZE, 0x0000, pal);
        img = sok_load_img(FLASH_STORE_ID_PLAYER_IMG, 1250);
        if (img) sok_draw_scaled_sprite(ux, uy, img, SOK_PLAYER_SIZE, 0xffff, 0);
        break;
    }
    }
}

/**
 * @brief  在地图中查找玩家当前位置
 * @param  p_row  输出: 玩家所在行
 * @param  p_col  输出: 玩家所在列
 */
static void sokoban_find_player(uint8_t* p_row, uint8_t* p_col) {
    uint8_t r, c;
    for (r = 0; r < g_level_rows; r++) {
        for (c = 0; c < g_level_cols; c++) {
            uint8_t v = g_runtime_map[r * g_level_cols + c];
            if (v == SOK_PLAYER || v == SOK_PLAYER_ON_TARGET) {
                *p_row = r;
                *p_col = c;
                return;
            }
        }
    }
}

/**
 * @brief  判断是否通关 (所有目标点均有箱子)
 * @retval 1=通关, 0=未通关
 */
uint8_t sokoban_is_won(void) {
    uint8_t r, c;
    for (r = 0; r < g_level_rows; r++) {
        for (c = 0; c < g_level_cols; c++) {
            if (is_orig_target(r, c)) {
                if (g_runtime_map[r * g_level_cols + c] != SOK_BOX_ON_TARGET)
                    return 0;
            }
        }
    }
    return 1;
}

/**
 * @brief  清除关卡文字"关卡N"的背景 (只清文字本身居中宽度, 不全宽, 避免左右黑块)
 * @note   y=(240-32)/2-20=84, 字高32; 文字宽 total_w=64+4+digit_w (关32+卡32+数字)
 *         与 sok_level_draw 的布局公式一致
 */
static void sok_level_clear(void) {
    uint8_t lv = s_level_num;
    uint8_t dc = (lv / 10 > 0) ? 2 : 1;
    uint16_t dw = dc * 16 + (dc - 1) * 2;
    /* 中英两套布局宽度/起点不同: 中文 "关"+“卡”+数字 = 64+4+dw; 英文 "Level"+数字 = 5*16+6+dw
       (英文比中文宽 18px, 居中后左边界还要往左 9px)。只按当前语言清会漏掉另一套的边缘 —— 
       表现为切成英文后, "Level" 最左边的 L 残留在屏上。两套都居中, 窄的必被宽的包含,
       故按较宽那套整带清一次即可。*/
    uint16_t w_cn = 64 + 4 + dw;
    uint16_t w_en = 5 * 16 + 6 + dw;
    uint16_t w_wide = (w_cn > w_en) ? w_cn : w_en;
    uint16_t x = (uint16_t)((240 - w_wide) / 2);
    LCD_Fill(x, 84, x + w_wide, 116, LCD_BLACK);
}

/**
 * @brief  把第 level_index 关的地图(Bcd 原数据)从外部 Flash 读进 g_level_bcd
 * @note   资源 ID = FLASH_STORE_ID_SOK_MAP_0 + 关卡序号; 字节数 = ceil(rows*cols/2)
 *         (调用前 g_level_rows/g_level_cols 必须已填好)
 * @retval 1=缓冲可用, 0=读取失败(没烧资源/长度不符)
 */
static uint8_t sok_load_level_bcd(uint8_t level_index) {
    if (level_index >= SOKOBAN_LEVEL_COUNT) return 0;
    if (s_bcd_level == level_index) return 1;          /* 已经是这一关, 不重复读 */

    uint16_t need = ((uint16_t)g_level_rows * g_level_cols + 1) / 2;
    if (need > sizeof(g_level_bcd)) need = sizeof(g_level_bcd);
    uint16_t got = FlashStore_ResRead(
        (FlashStoreId)(FLASH_STORE_ID_SOK_MAP_0 + level_index), 0, g_level_bcd, need);
    if (got != need) { s_bcd_level = 0xFF; return 0; } /* 明确失败, 免得解压出乱码地图 */
    s_bcd_level = level_index;
    return 1;
}

/**
 * @brief  准备关卡数据结构, 不涉及LCD刷新
 * @param  level_index  关卡索引
 * @retval 1=成功, 0=关卡索引无效
 */
static uint8_t sokoban_prepare_level(uint8_t level_index) {
    if (level_index >= SOKOBAN_LEVEL_COUNT) return 0;

    /* 换关/重开: 清空撤销/重做历史(这里本来就是复位点) */
    s_hist_cnt = 0;
    s_hist_head = 0;
    s_hist_pos = 0;

    g_cur_level = &sokoban_levels[level_index];
    g_level_rows = g_cur_level->rows;
    g_level_cols = g_cur_level->cols;
    g_move_count = 0;

    {
        uint16_t max_cols = g_level_cols > g_level_rows ? g_level_cols : g_level_rows;
        g_tile_size = 200 / max_cols;
        if (g_tile_size < 10) g_tile_size = 10;
        if (g_tile_size > SOK_TILE_SIZE) g_tile_size = SOK_TILE_SIZE;
    }

    /* 地图本体在外部 Flash(资源区): 读进 RAM 缓冲; 读不到就填空"外界"格子(画出来是空白),
       至少不会刷出乱码地图, 也便于一眼看出"这块板还没烧新字库" */
    if (!sok_load_level_bcd(level_index)) {
        uint16_t k;
        for (k = 0; k < sizeof(g_level_bcd); k++) g_level_bcd[k] = 0x66;
        s_bcd_level = level_index;
    }

    /* BCD解压: 每字节=十位*10+个位, 存2个tile */
    {
        const uint8_t* src = g_level_bcd;
        uint16_t i;
        uint16_t total = g_level_rows * g_level_cols;
        for (i = 0; i < total; i += 2) {
            uint8_t b = *src++;
            g_runtime_map[i] = b / 10;
            if (i + 1 < total) {
                g_runtime_map[i + 1] = b % 10;
            }
        }
    }

    g_ox = 20 + (200 - (int16_t)g_level_cols * (int16_t)g_tile_size) / 2;
    g_oy = 20 + (200 - (int16_t)g_level_rows * (int16_t)g_tile_size) / 2;

    s_current_level = level_index;
    s_won = 0;
    s_exit_menu_visible = 0;
    s_undo_group = 0;
    s_undo_visible = 0;
    s_lvl2_visible = 0;
    s_menu_sliding = 0;
    s_menu_slide_y = 240;
    s_menu_slide_anim = 0;
    s_menu_from_swipe = 0;
    s_level_num = level_index + 1;
    s_drag_btn = 0;   /* 防旧值导致松手误触发切关 */
    s_hold_button = 0;   /* 防旧值导致menu_visible误入长按 */
    s_hold_us = 0;
    return 1;
}

/**
 * @brief  初始化并绘制指定关卡, 同时显示导航菜单
 * @param  level_index  关卡索引
 */
void sokoban_init_level(uint8_t level_index) {
    if (!sokoban_prepare_level(level_index)) return;

    /* 清屏并全部重绘 */
    sokoban_draw_level(level_index);

    /* 导航按钮(3个) + 关卡编号显示 */
    s_menu_visible = 1;
    s_prev_pressed = 0;  /* 重置触摸, 防止从其他游戏进入时菜单被误关 */
    sok_redraw_nav_group();
}

/**
 * @brief  电池闪灭恢复，设置脏标记由 update() 局部重绘
 * @note   避免全屏 LCD_Fill 造成闪烁，仅重绘电池覆盖的瓦片
 */
void sokoban_full_redraw(void) {
    g_sok_dirty = 2;  /* 电池闪灭重绘，跳过全屏大填充 */
}

/**
 * @brief  从设置返回游戏时恢复游戏内面板: 撤销模式回撤销按钮组, 否则回一级按钮组
 * @note   s_undo_group 在进设置前就被置 1(撤销界面->上划二级菜单->设置), 期间没被清过,
 *         所以这里按它判断该回哪个界面
 */
static void sok_restore_panel_by_mode(void) {
    if (s_undo_group) {
        s_menu_slide_y = SOK_BTN_Y;   /* 电池恢复按这个上缘裁剪, 别留二级菜单的旧值 */
        s_undo_visible = 1;
        sok_draw_undo_buttons();
    }
    else {
        sok_show_nav_group();
    }
}

/**
 * @brief  从设置菜单返回到按钮面板 (main.c GameSettings_Update 返回分支调用)
 * @note   局部恢复: 只清设置条带覆盖区(y>=VIDEO_MENU_Y)重绘图格, 再补面板, 不全屏重绘(会闪)
 */
void sokoban_menu_settings_restore(void) {
    LCD_Fill(0, VIDEO_MENU_Y, 239, 239, LCD_BLACK);
    sok_redraw_grid_below(VIDEO_MENU_Y);
    sok_restore_panel_by_mode();
}

/**
 * @brief  从"设置页"(整屏绘制)返回游戏: 整幅恢复地图 + 按钮面板 + 关卡编号
 * @note   设置页是整屏绘制的, 页面上有些元素落在二级菜单条带区以外(如语言页左侧的绿色
 *         选中方块), 只清条带区会留下残影。这里整屏清黑重画整张地图(与"设置页下滑进
 *         退出菜单"同一做法), 再补按钮面板与关卡编号。
 */
void sokoban_menu_settings_restore_full(void) {
    sokoban_draw_level(s_current_level);
    sok_restore_panel_by_mode();
}


/**
 * @brief  移动玩家 (含推箱子逻辑)
 * @param  dir  移动方向
 * @retval 1=移动成功, 0=无法移动
 */
uint8_t sokoban_move(SokobanDir dir) {
    if (dir == SOK_DIR_NONE) return 0;

    uint8_t pr = 0, pc = 0;
    sokoban_find_player(&pr, &pc);

    static const int8_t dr[] = { -1, 1,  0, 0 };
    static const int8_t dc[] = { 0, 0, -1, 1 };
    int8_t nr = (int8_t)pr + dr[dir];
    int8_t nc = (int8_t)pc + dc[dir];

    /* 边界检查 */
    if (nr < 0 || nr >= (int8_t)g_level_rows || nc < 0 || nc >= (int8_t)g_level_cols)
        return 0;

    uint8_t target_cell = g_runtime_map[nr * g_level_cols + nc];

    /* 撞墙或边界 */
    if (target_cell == SOK_WALL || target_cell == SOK_OUTER)
        return 0;

    /* 推箱子逻辑 */
    if (target_cell == SOK_BOX || target_cell == SOK_BOX_ON_TARGET) {
        int8_t br = nr + dr[dir];
        int8_t bc = nc + dc[dir];
        if (br < 0 || br >= (int8_t)g_level_rows || bc < 0 || bc >= (int8_t)g_level_cols)
            return 0;
        uint8_t beyond = g_runtime_map[br * g_level_cols + bc];
        if (beyond == SOK_WALL || beyond == SOK_OUTER ||
            beyond == SOK_BOX || beyond == SOK_BOX_ON_TARGET)
            return 0;

        /* 移动箱子 */
        g_runtime_map[br * g_level_cols + bc] =
            is_orig_target((uint8_t)br, (uint8_t)bc) ? SOK_BOX_ON_TARGET : SOK_BOX;
        /* 原箱子位置恢复为地板/目标 */
        g_runtime_map[nr * g_level_cols + nc] =
            is_orig_target((uint8_t)nr, (uint8_t)nc) ? SOK_TARGET : SOK_FLOOR;
    }

    /* 移动玩家 */
    g_runtime_map[pr * g_level_cols + pc] =
        is_orig_target(pr, pc) ? SOK_TARGET : SOK_FLOOR;
    g_runtime_map[nr * g_level_cols + nc] =
        is_orig_target((uint8_t)nr, (uint8_t)nc) ? SOK_PLAYER_ON_TARGET : SOK_PLAYER;

    g_move_count++;
    Main_ClearLowBattSaveFlag();  /* 移动/菜单操作清除低电量保存标志 */

    /* 记录一步撤销历史(低2位=方向, bit2=这一步推了箱子) */
    sok_hist_push((uint8_t)((uint8_t)dir
        | (uint8_t)(((target_cell == SOK_BOX || target_cell == SOK_BOX_ON_TARGET) ? 1u : 0u) << 2)));

    /* 局部刷新 (三个变化的格子; 用 sok_redraw_cell_ui(): 跳过被撤销按钮盖住的格子,
       否则箱子从底部那排移走时这一格重绘会把中间按钮抠出缺口) */
    LCD_CS_CLR;
    sok_redraw_cell_ui(pr, pc);   /* 玩家原位置 */
    sok_redraw_cell_ui((uint8_t)nr, (uint8_t)nc);   /* 玩家新位置 */
    if (target_cell == SOK_BOX || target_cell == SOK_BOX_ON_TARGET) {
        int8_t br = nr + dr[dir];
        int8_t bc = nc + dc[dir];
        sok_redraw_cell_ui((uint8_t)br, (uint8_t)bc); /* 箱子新位置 */
    }
    LCD_CS_SET;
    /* 撤销面板是不透明板: 跨在它上边缘的格子会盖住按钮, 补画一次(否则中间按钮被抠出缺口) */
    if (s_undo_visible) sok_draw_undo_buttons();
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);

    return 1;
}

/* 触摸输入 (长按移动和菜单手势) */

/**
 * @brief  按钮命中检测: 返回 0=未命中(Y 在命中带外), 1=上一关, 2=重新开始, 3=下一关
 * @note   横向按"相邻按钮中线"(82/157)分, 三个区无缝 [0,82)/[82,157)/[157,240) ——
 *         原先直接拿按钮左右缘(75/90, 150/165)判, 按钮之间会留 15px 死区(点了当"点空白")
 */
static uint8_t sok_btn_hit_test(uint16_t x, uint16_t y) {
    if (y < SOK_BTN_HIT_Y0 || y >= SOK_BTN_HIT_Y1) return 0;
    if (x < SOK_BTN_HIT_SPLIT1) return 1;
    if (x < SOK_BTN_HIT_SPLIT2) return 2;
    return 3;
}

/**
 * @brief  裁剪绘制单个按钮的可见行
 */
static void sok_draw_btn_clipped(uint16_t x, int16_t y, const uint16_t* img, int16_t vis_rows) {
    int16_t r, c, start, sr;
    for (r = 0; r < vis_rows; r++) {
        uint8_t is_last = (r == 30);  /* 最后一行(r=30)为纯黑, 不跳过0x0000 */
        for (sr = 0; sr < 2; sr++) {
            int16_t sy = y + r * 2 + sr;
            if (sy > 239) continue;
            c = 0;
            while (c < 30) {
                if (!is_last) {
                    while (c < 30 && img[r * 30 + c] == 0x0000) c++;
                }
                if (c >= 30) break;
                start = c;
                if (is_last) {
                    c = 30;
                }
                else {
                    while (c < 30 && img[r * 30 + c] != 0x0000) c++;
                }
                LCD_Address_Set(x + start * 2, (uint16_t)sy,
                    x + c * 2 - 1, (uint16_t)sy);
                for (; start < c; start++) {
                    uint8_t sc;
                    for (sc = 0; sc < 2; sc++)
                        LCD_WR_DATA_16Bit(img[r * 30 + start]);
                }
            }
        }
    }
}

/**
 * @brief  记录退出菜单是否由"撤销的3个按钮"打开: 是则上划关闭后恢复这组按钮
 * @note   由 sok_try_open_exit_menu() 清零; 撤销界面打开退出菜单时置 1
 */
static uint8_t  s_exit_from_undo = 0;

/**
 * @brief  退出菜单上划关闭后恢复撤销按钮(仅当它是由撤销按钮打开的)
 * @retval 1=已恢复(调用方不再滑入面板), 0=不是, 走常规滑回按钮面板
 */
static uint8_t sok_exit_restore_undo(void) {
    if (!s_exit_from_undo) return 0;
    s_undo_visible = 1;
    s_menu_from_swipe = 1;   /* 吸收本次手势: 松手不再命中按钮, 否则按钮会被关掉 */
    s_prev_pressed = 1;
    sok_draw_undo_buttons();
    return 1;
}

static uint8_t sok_exit_swipe_cancel(void) {
    int16_t cancel_dy = 0;
    uint8_t poll_ret = game_menu_swipe_up_poll(SOK_SWIPE_Y, MENU_SWIPE_TRIGGER_DIST, &cancel_dy);
    if (!poll_ret) {
        /* 中间区域上划只在静止态生效: 拖拽/滑入期间会误关刚打开的退出菜单(见备忘录 §437) */
        if (s_exit_dragging || s_exit_sliding) return 0;
        if (!game_menu_swipe_mid_up_close(SOK_SWIPE_Y, MENU_SWIPE_TRIGGER_DIST)) return 0;
    }
    else g_menu_cooldown = 15;


    s_exit_menu_visible = 0;
    s_exit_sliding = 0;
    s_exit_dragging = 0;
    s_prev_pressed = 0;
    s_menu_cooldown = 15;
    g_touch_down = 0;   /* 清除全局触摸状态(松手由按钮面板处理, 不设吸收标志防跨游戏残留) */
    LCD_CLEAR_MENU_AREA();           /* 清退出菜单区(含边框), 防黑色/残留 */
    sok_redraw_grid_below(MENU_Y);
    if (sok_exit_restore_undo()) {
    }
    else if (poll_ret == 2) {
        /* 松手打断: 手指已抬起, 自动动画滑入 */
        s_menu_slide_anim = 1;
        s_menu_slide_y = 240;
    }
    else {
        /* 拖拽打断: 手指还在屏幕上, 跟手滑入 */
        TouchState ts_now;
        Touch_UpdateCoords();
        Touch_Scan(&ts_now);
        s_swipe_origin_y = g_touch_start_y; /* 上划起点, 用于计算总位移 */
        s_menu_slide_y = 240;
        s_menu_sliding = 1;
        s_slide_max_dy = 0;   /* 复位最远位移, 防旧值导致回退>2误取消 */
        s_drag_btn = sok_btn_hit_test(ts_now.x, ts_now.y);
    }
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
    return 1;
}

/**
 * @brief  在指定Y绘制导航按钮(屏幕外部分裁剪, 从底部逐渐出现)
 */
static void sok_draw_nav_buttons_at(int16_t y) {
    if (y > 239) return;

    LCD_CS_CLR;

    /* 按钮完全在屏幕内(y<=180)复用已有绘制函数, 节省flash */
    if (y <= 180) {
        if (s_current_level > 0) sok_draw_btn(SOK_BTN1_X, (uint16_t)y, 0, PREV_IMG_W);
        sok_draw_btn(SOK_BTN2_X, (uint16_t)y, 2, RESTART_IMG_W);
        if (s_current_level < SOKOBAN_LEVEL_COUNT - 1)
            sok_draw_btn(SOK_BTN3_X, (uint16_t)y, 1, NEXT_IMG_W);
        LCD_CS_SET;
        return;
    }

    /* 部分在屏幕外: 逐行裁剪, 避免LCD地址回绕 */
    int16_t max_vis_row = (239 - y) / 2;
    if (max_vis_row < 0) { LCD_CS_SET; return; }
    int16_t vis_rows = max_vis_row + 1;
    if (vis_rows > 31) vis_rows = 31;

    if (s_current_level > 0) {
        const void* img = sok_btn_img_rows(0, vis_rows, PREV_IMG_W);
        if (img) sok_draw_btn_clipped(SOK_BTN1_X, y, img, vis_rows);
    }
    {
        const void* img = sok_btn_img_rows(2, vis_rows, RESTART_IMG_W);
        if (img) sok_draw_btn_clipped(SOK_BTN2_X, y, img, vis_rows);
    }
    if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
        const void* img = sok_btn_img_rows(1, vis_rows, NEXT_IMG_W);
        if (img) sok_draw_btn_clipped(SOK_BTN3_X, y, img, vis_rows);
    }
    LCD_CS_SET;
}

/* ================= 二级菜单(3行: 撤销/设置/返回) ================= */

/**
 * @brief  刚由上划拉出面板时吸收本次手势: 只看手指松开, 不把松手当点击
 * @param  pressed 本帧是否按下
 * @retval 1=本帧已吸收(调用方直接返回) 0=正常处理
 * @note   二级菜单和撤销按钮组都是上划拉出来的, 拉出时手指还按着; 若不管这次松手,
 *         松开的位置常常落在菜单行/按钮上 -> 立刻误触发(表现为"点了没反应/闪一下")
 */
static uint8_t sok_swipe_enter(uint8_t pressed) {
    if (!s_menu_from_swipe) return 0;
    if (pressed) { s_prev_pressed = 1; return 1; }
    s_menu_from_swipe = 0;
    s_prev_pressed = 0;
    return 1;
}

/**
 * @brief  按当前面板种类画在 y 处(跟手滑动用)
 * @note   种类1(二级菜单): 菜单是往上长的, 新位置必然盖住上一帧的旧位置, 所以按 y
 *         直接画一次即可 —— 不必补地图, 更不能拿整格瓦片去补(格子粒度会让菜单上边缘
 *         一格一跳地闪)。菜单上方本来就是游戏画面, 不用动。
 */
static void sok_panel_draw_at(int16_t y) {
    if (s_panel_kind == 1) {
        menu_draw_nrow_frame_only(y, sok_lvl2_row_types, 3);
    } else {
        sok_draw_nav_buttons_at(y);
    }
}

/**
 * @brief  清除 y 以下的面板残留(取消拖拽用)
 * @note   二级菜单不用清: 调用方随后 sok_redraw_grid_below() 就把这一带补回地图了;
 *         导航按钮组先刷黑再重绘瓦片(按钮分布广, 只重绘瓦片可能留边)
 */
static void sok_panel_clear_from(int16_t y) {
    LCD_Fill(0, (uint16_t)y, 240, 240, LCD_BLACK);
}

/**
 * @brief  启动跟手上划(导航按钮组 或 二级菜单, 三处共用: 游戏内上划/一级面板上划/撤销界面上划)
 * @param  kind    0=导航按钮组(停在 SOK_BTN_Y), 1=二级菜单(停在 RESTART_MENU_Y-1)
 * @param  up_move 本次上划的位移(px)
 * @note   与一级按钮面板共用同一套滑动状态(s_menu_sliding/s_menu_slide_y), 靠 s_panel_kind 区分画什么
 */
static void sok_panel_drag_start(uint8_t kind, int16_t up_move) {
    int16_t y0 = (int16_t)(240 - up_move);
    s_panel_kind = kind;
    /* 二级菜单停在 RESTART_MENU_Y-1: 顶边上移1px, 正好盖掉低电图标上边框露在菜单框外的第79行 */
    s_panel_rest = (kind == 1) ? (RESTART_MENU_Y - 1) : SOK_BTN_Y;

    s_slide_max_dy = up_move;
    if (y0 < s_panel_rest) y0 = s_panel_rest;
    s_menu_slide_y = y0;
    s_menu_sliding = 1;
}

/**
 * @brief  擦掉二级菜单框 + 恢复其下地图
 */
static void sok_lvl2_clear_frame(void) {
    s_lvl2_visible = 0;
    /* 滑动位置复位到按钮面板停靠位: 电池恢复按这个上缘裁剪, 用 240 会一直补到 y=219,
       把撤销界面的三个按钮(189 起)盖在地图下面 */
    s_menu_slide_y = SOK_BTN_Y;
    LCD_Fill_DMA(RESTART_MENU_X, RESTART_MENU_Y - 1,
                 RESTART_MENU_X + RESTART_MENU_W, 240, LCD_BLACK);
    sok_redraw_grid_below(RESTART_MENU_Y - 1);
}

/**
 * @brief  二级菜单每帧处理: 按下记录→松手同位置触发
 * @retval 0=已处理(继续游戏) EXIT_RET_SETTINGS=要进设置菜单
 * @note   行0=撤销(进撤销界面) 行1=设置 行2=返回(换回一级按钮组); 点框外空白=关闭回游戏
 */
static uint8_t sok_lvl2_update(void) {
    TouchState ts;
    int8_t row;

    Touch_Scan(&ts);

    /* 刚由上划拉出: 本次手势只看松开, 不当点击 */
    if (sok_swipe_enter(ts.pressed)) return 0;

    if (ts.pressed) {
        if (!s_prev_pressed) {         /* 按下: 记录起手Y(松手后才判定) */
            s_prev_pressed = 1;
            s_swipe_origin_y = ts.y;
        }
        /* 按住中: 上方区域(y<40)下滑 -> 收起二级菜单, 打开退出菜单(与游戏内一致) */
        else if (sok_try_open_exit_menu(ts.y)) {
            sok_lvl2_clear_frame();
            s_menu_cooldown = 15;
        }
        return 0;
    }
    if (!s_prev_pressed) return 0;

    s_prev_pressed = 0;
    /* 划动过(位移>阈值)再松手不算点击: 整段手势作废, 不触发行 */
    {
        int16_t dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
        if (dy < 0) dy = -dy;
        if (dy > MENU_SWIPE_COMMIT_DIST) return 0;
    }
    if (ts.x >= RESTART_MENU_X && ts.x < RESTART_MENU_X + RESTART_MENU_W
        && ts.y >= RESTART_MENU_Y) {
        row = menu_row_from_y(RESTART_MENU_Y, ts.y, 3);
    } else {
        row = -1;                          /* 框外/上方空白 */
    }
    if (row == 1) {                        /* 设置: 瞬间弹出整屏设置页(不做跟手动画, 省空间) */
        s_lvl2_visible = 0;
        menu_enter_game_settings();
        return EXIT_RET_SETTINGS;
    }

    sok_lvl2_clear_frame();
    if (row == 0) {                        /* 撤销界面: 底部换成 上一步/返回/下一步 */
        s_undo_group = 1;
        s_undo_visible = 1;
        sok_draw_undo_buttons();
    } else if (row == 2) {                 /* 返回: 按模式回到撤销的3个按钮 / 一级按钮组 */
        sok_restore_panel_by_mode();
    }
    s_menu_cooldown = 15;
    return 0;
}

/* ================= 撤销界面(上一步/返回/下一步) ================= */

/**
 * @brief  取撤销按钮图 (0=上一步 1=返回 2=下一步; 按语言选保留区, 整图)
 */
static const void* sok_undo_img_rows(uint8_t which) {
    uint32_t base  = (g_language_index == 1) ? BTN_IMG_UNDO_EN_ADDR : BTN_IMG_UNDO_CN_ADDR;
    uint32_t off   = (which == 0) ? BTN_IMG_UNDO_PREV :
                     (which == 1) ? BTN_IMG_UNDO_BACK : BTN_IMG_UNDO_NEXT;
    uint8_t* s = FlashStore_Scratch();
    return (FlashStore_RawRead(base + off, s, UNDO_IMG_ONE) == UNDO_IMG_ONE) ? s : 0;
}

/**
 * @brief  画整组撤销按钮(1:1 显示, 30x31)
 */
static void sok_draw_undo_buttons(void) {
    uint8_t i;
    for (i = 0; i < 3; i++) {
        const void* img = sok_undo_img_rows(i);
        if (img) sok_nav_draw_rgb565_scaled((uint16_t)(SOK_UNDO_X1 + i * 60),
                                            SOK_UNDO_Y, (const uint16_t*)img, 1);
    }
}

/**
 * @brief  重绘一级导航按钮组 + 关卡编号(不改可见状态)
 * @note   原先"按钮组+关卡编号"这对调用在文件里写了 7 处, 合并成这一份
 */
static void sok_redraw_nav_group(void) {
    sok_draw_nav_buttons_at(SOK_BTN_Y);
    sok_level_draw(s_level_num, LCD_RED);
}

/**
 * @brief  显示一级导航按钮组(上一关/重新开始/下一关) + 关卡编号
 */
static void sok_show_nav_group(void) {
    s_menu_visible = 1;
    s_menu_slide_y = SOK_BTN_Y;   /* 面板停靠位: 电池恢复按这个上缘裁剪, 别留旧值 */
    sok_redraw_nav_group();
}

/**
 * @brief  撤销按钮命中: 0=未命中 1=上一步 2=返回 3=下一步
 * @note   横向沿用 60px 槽位(45/105/165 起, 好点); 纵向用撤销按钮自己那套窄带
 */
static uint8_t sok_undo_hit_test(uint16_t x, uint16_t y) {
    if (y < SOK_UNDO_HIT_Y0 || y >= SOK_UNDO_HIT_Y1) return 0;
    if (x < SOK_UNDO_HIT_SPLIT1) return 1;
    if (x < SOK_UNDO_HIT_SPLIT2) return 2;
    return 3;
}

/**
 * @brief  重绘一格, 但跳过"被撤销按钮盖住"的格子
 * @note   撤销按钮是不透明板, 画了也看不见; 按钮关闭/换组时会 sok_redraw_grid_below
 *         把那一整块补回来, 所以这里跳过不会留下错画面
 * @note   noinline: 有 4 处调用, 内联会把这段比较复制 4 份, 反而更占 flash
 */
static void __attribute__((noinline)) sok_redraw_cell_ui(uint8_t r, uint8_t c) {
    /* 只跳'完全被按钮盖住'的格子; 跨在按钮上边缘那一格要照画(它在面板上方还露一条),
       盖住按钮的部分随后由 sok_draw_undo_buttons() 补回来 */
    if (g_oy + (int16_t)r * (int16_t)g_tile_size >= SOK_UNDO_Y) return;
    sokoban_draw_cell_at(r, c);
}

/**
 * @brief  记录一步撤销历史(环形: 满了覆盖最旧; 走新的一步作废"未来")
 */
static void sok_hist_push(uint8_t step) {
    /* 撤回后又走新的一步: 先把"未来"截断(作废已撤销的分支)。
       否则历史条数会与实际走过的步数不一致, 再撤就会多退一步 -> 人物穿墙 */
    if (s_hist_pos < s_hist_cnt) s_hist_cnt = s_hist_pos;
    if (s_hist_cnt < SOK_HIST_MAX) {
        s_hist[(uint8_t)((s_hist_head + s_hist_cnt) & (SOK_HIST_MAX - 1))] = step;
        s_hist_cnt++;
    } else {
        s_hist[s_hist_head] = step;
        s_hist_head = (uint8_t)((s_hist_head + 1) & (SOK_HIST_MAX - 1));
    }
    s_hist_pos = s_hist_cnt;
}

/**
 * @brief  按历史记录走一步: forward=0 反推(撤销), 1 正推(重做), 并局部重绘 2~4 格
 * @note   玩家位置现场扫出来(不另存), 箱子/目标靠 is_orig_target() 反查原始地图。
 *         前进/后退只差一个符号: 玩家 源→源+sgn*dir; 箱子恒为 (源+dir)→(源+dir+sgn*dir),
 *         所以两者共用这一段(见开发注意事项 §38 的"1 字节/步 + 反推")。
 */
static uint8_t sok_hist_step(uint8_t step, uint8_t forward) {
    static const int8_t dr4[4] = { -1, 1, 0, 0 };   /* 上/下/左/右 (与 SokobanDir 同序) */
    static const int8_t dc4[4] = { 0, 0, -1, 1 };
    uint8_t d = (uint8_t)(step & 3);
    uint8_t pushed = (uint8_t)((step >> 2) & 1);
    int8_t sgn = forward ? 1 : -1;
    int8_t dr = (int8_t)(dr4[d] * sgn), dc = (int8_t)(dc4[d] * sgn);
    int8_t pr, pc, nr, nc, bx = 0, bc = 0, tx = 0, tc = 0;
    uint8_t pr8 = 0, pc8 = 0;
    uint8_t cell;

    sokoban_find_player(&pr8, &pc8);
    pr = (int8_t)pr8;  pc = (int8_t)pc8;
    nr = (int8_t)(pr + dr);  nc = (int8_t)(pc + dc);            /* 玩家目标格 */
    if (pushed) {
        bx = (int8_t)(pr + dr4[d]);  bc = (int8_t)(pc + dc4[d]);  /* 箱子当前格 */
        tx = (int8_t)(bx + dr);      tc = (int8_t)(bc + dc);      /* 箱子目标格 */
    }

    /* 再保险: 越界或"目标格是墙"(说明历史与地图对不上)就整步作废,
       绝不让越界下标写坏内存、也不让墙被改成地板(人物穿墙) */
    if (nr < 0 || nr >= (int8_t)g_level_rows || nc < 0 || nc >= (int8_t)g_level_cols) return 0;
    cell = g_runtime_map[nr * g_level_cols + nc];
    if (cell == SOK_WALL || cell == SOK_OUTER) return 0;
    if (pushed) {
        if (tx < 0 || tx >= (int8_t)g_level_rows || tc < 0 || tc >= (int8_t)g_level_cols) return 0;
    }

    /* 顺序要紧: ① 玩家离开 ② 箱子移动(会用到/pr,pc 或 nr,nc 重叠的格子) ③ 玩家到达 */
    g_runtime_map[pr * g_level_cols + pc] =
        is_orig_target((uint8_t)pr, (uint8_t)pc) ? SOK_TARGET : SOK_FLOOR;
    if (pushed) {
        g_runtime_map[bx * g_level_cols + bc] =
            is_orig_target((uint8_t)bx, (uint8_t)bc) ? SOK_TARGET : SOK_FLOOR;
        g_runtime_map[tx * g_level_cols + tc] =
            is_orig_target((uint8_t)tx, (uint8_t)tc) ? SOK_BOX_ON_TARGET : SOK_BOX;
    }
    g_runtime_map[nr * g_level_cols + nc] =
        is_orig_target((uint8_t)nr, (uint8_t)nc) ? SOK_PLAYER_ON_TARGET : SOK_PLAYER;

    LCD_CS_CLR;
    sok_redraw_cell_ui((uint8_t)pr, (uint8_t)pc);
    sok_redraw_cell_ui((uint8_t)nr, (uint8_t)nc);
    if (pushed) {
        sok_redraw_cell_ui((uint8_t)bx, (uint8_t)bc);
        sok_redraw_cell_ui((uint8_t)tx, (uint8_t)tc);
    }
    LCD_CS_SET;
    if (s_undo_visible) sok_draw_undo_buttons();   /* 同上: 跨线格子会盖住按钮, 补画 */
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
    return 1;
}

/**
 * @brief  按历史走一步: dirn<0=撤销, >0=重做; 到头了/走不动返回 0(点了没反应)
 */
static uint8_t sok_hist_go(int8_t dirn) {
    uint8_t done;
    uint8_t pos0 = s_hist_pos;

    if (dirn < 0) {
        if (s_hist_pos == 0) return 0;          /* 栈空/已撤到底 */
        s_hist_pos--;
    } else if (s_hist_pos >= s_hist_cnt) {
        return 0;                               /* 没有可重做的了 */
    }
    done = sok_hist_step(s_hist[(uint8_t)((s_hist_head + s_hist_pos) & (SOK_HIST_MAX - 1))],
                         (uint8_t)(dirn > 0));
    if (!done) {                                /* 状态对不上: 指针退回, 什么也没做 */
        s_hist_pos = pos0;
        return 0;
    }
    if (dirn > 0) {
        s_hist_pos++;
    }
    Main_ClearLowBattSaveFlag();
    return 1;
}

/**
 * @brief  关闭撤销按钮组; back_to_nav=1 时换回一级导航按钮组
 */
static void sok_undo_hide(uint8_t back_to_nav) {
    s_undo_visible = 0;
    LCD_Fill(SOK_UNDO_X1, SOK_UNDO_Y,
             SOK_UNDO_X3 + UNDO_IMG_W, SOK_UNDO_Y + UNDO_IMG_H, LCD_BLACK);
    sok_redraw_grid_below(SOK_UNDO_Y);
    if (back_to_nav) {
        s_undo_group = 0;
        sok_show_nav_group();
    }
    s_menu_cooldown = 15;
}

/**
 * @brief  上方区域下滑 -> 打开退出菜单(游戏内/二级菜单/撤销界面共用)
 * @param  y  当前触点Y
 * @retval 1=已打开(调用方按需再收掉自己的面板)
 */
static uint8_t sok_try_open_exit_menu(uint16_t y) {
    if (s_menu_cooldown != 0 || s_swipe_origin_y >= MENU_SWIPE_START_Y) return 0;
    if ((int16_t)y - (int16_t)s_swipe_origin_y <= MENU_SWIPE_TRIGGER_DIST) return 0;
    s_exit_menu_visible = 1;
    s_exit_dragging = 1;
    s_exit_sliding = 0;
    s_exit_drag_y = -MENU_H + MENU_DRAG_MAX_STEP;
    s_exit_drag_start_y = y;
    s_exit_from_undo = 0;
    return 1;
}

/**
 * @brief  撤销界面每帧处理: 上一步/下一步/返回/点空白
 */
static void sok_undo_ui_update(void) {
    TouchState ts;
    uint8_t hit;

    Touch_Scan(&ts);

    /* 刚由上划拉出: 本次手势只看松开, 不当点击(否则松手位置落在按钮上会误触发) */
    if (sok_swipe_enter(ts.pressed)) return;

    if (!s_prev_pressed && ts.pressed) {
        s_prev_pressed = 1;
        s_swipe_origin_y = ts.y;      /* 记下起手Y, 供"再上划换成二级菜单"用 */
        return;
    }
    if (ts.pressed) {
        /* 按住中: 在最底部上划 -> 收起撤销按钮, 换成二级菜单(与一级面板同一手感) */
        if (s_menu_cooldown == 0 && s_swipe_origin_y > SOK_SWIPE_Y
            && (int16_t)s_swipe_origin_y - (int16_t)ts.y > MENU_SWIPE_TRIGGER_DIST) {
            int16_t up_move = (int16_t)s_swipe_origin_y - (int16_t)ts.y;
            sok_undo_hide(0);
            sok_panel_drag_start(1, up_move);
        }
        /* 上方区域下滑 -> 收起撤销按钮, 打开退出菜单(与游戏内一致) */
        if (sok_try_open_exit_menu(ts.y)) {
            s_exit_from_undo = 1;
            sok_undo_hide(0);
        }
        return;
    }
    if (!s_prev_pressed) return;
    s_prev_pressed = 0;

    hit = sok_undo_hit_test(ts.x, ts.y);
    if (hit == 1)      sok_hist_go(-1);      /* 上一步(撤销) */
    else if (hit == 3) sok_hist_go(1);       /* 下一步(重做) */
    else if (hit == 2) sok_undo_hide(1);     /* 返回 → 一级按钮组 */
    else               sok_undo_hide(0);     /* 点空白 → 关闭, 可操作游戏 */
}

/**
 * @brief  关闭一级按钮面板: 清文字带+按钮带(窄带, 不全屏黑)并恢复其下地图
 * @note   原先这段在"取消/点空白/二次上划"三处各写一遍, 合并成这一份
 */
static void sok_close_nav_panel(void) {
    sok_level_clear();                                        /* 关卡文字带 */
    LCD_Fill(0, SOK_BTN_Y, 239, SOK_BTN_Y + SOK_BTN_H, LCD_BLACK);  /* 按钮带 */
    sok_redraw_grid_below(84);                                /* 其下地图格子 */
    s_menu_visible = 0;
}

/**
 * @brief  重新开始当前关卡(保留菜单状态)
 */
static void sokoban_restart_current_level(void) {
    if (!sokoban_prepare_level(s_current_level)) return;

    s_menu_visible = 0;
    s_exit_sliding = 0;
    s_exit_dragging = 0;
    s_hold_button = 0;
    s_hold_us = 0;
    s_drag_btn = 0;
    s_menu_sliding = 0;
    s_menu_slide_y = 240;
    s_menu_slide_anim = 0;
    s_menu_from_swipe = 0;
    s_menu_cooldown = 15;
    sokoban_draw_level(s_current_level);
}

/**
 * @brief  重置触摸状态 (切换关卡时调用, 防止误触发滑动)
 */
void sokoban_reset_touch(void) {
    s_prev_pressed = 1;      /* 设为1防止首次触控被忽略, 避免立即触发菜单 */
    s_touch_start_x = 0;
    s_touch_start_y = 0;
    s_swipe_origin_y = MENU_SWIPE_START_Y;  /* 退出菜单手势起始Y坐标设为安全值 */
    s_menu_cooldown = 0;
}

/**
 * @brief  序列化推箱子当前状态到存档缓冲。
 * @param  buf     输出缓冲
 * @param  max_len 缓冲上限
 * @return 写入字节数
 */
uint8_t sokoban_save_state(uint8_t* buf, uint8_t max_len) {
    uint16_t cells;
    uint16_t i;
    uint16_t player = 0xFFFF;
    uint8_t box_count = 0;

    if (max_len < 3 || s_current_level >= SOKOBAN_LEVEL_COUNT) return 0;
    cells = (uint16_t)g_level_rows * g_level_cols;
    for (i = 0; i < cells; i++) {
        uint8_t value = g_runtime_map[i];
        if (value == SOK_PLAYER || value == SOK_PLAYER_ON_TARGET) {
            if (player != 0xFFFF) return 0;
            player = i;
        }
        else if (value == SOK_BOX || value == SOK_BOX_ON_TARGET) {
            if ((uint16_t)3 + box_count >= max_len) return 0;
            buf[3 + box_count++] = (uint8_t)i;
        }
    }
    if (player == 0xFFFF) return 0;

    buf[0] = s_current_level;
    buf[1] = (uint8_t)player;
    buf[2] = box_count;
    return (uint8_t)(3 + box_count);
}

/**
 * @brief  从存档缓冲恢复推箱子状态并重绘。
 * @param  buf 存档缓冲
 * @param  len 长度
 * @return 1=成功, 0=失败
 */
uint8_t sokoban_restore_state(const uint8_t* buf, uint8_t len) {
    const SokobanLevel* level;
    uint16_t cells;
    uint16_t i;
    uint8_t box_count;

    if (len < 3 || buf[0] >= SOKOBAN_LEVEL_COUNT) return 0;
    level = &sokoban_levels[buf[0]];
    cells = (uint16_t)level->rows * level->cols;
    box_count = buf[2];
    if (len != (uint8_t)(3 + box_count) || buf[1] >= cells) return 0;

    for (i = 0; i < box_count; i++) {
        uint16_t j;

        if (buf[3 + i] >= cells || buf[3 + i] == buf[1]) return 0;
        for (j = 0; j < i; j++) {
            if (buf[3 + i] == buf[3 + j]) return 0;
        }
    }

    if (!sokoban_prepare_level(buf[0])) return 0;
    for (i = 0; i < cells; i++) {
        uint8_t value = g_runtime_map[i];
        if (value == SOK_BOX || value == SOK_PLAYER) {
            g_runtime_map[i] = SOK_FLOOR;
        }
        else if (value == SOK_BOX_ON_TARGET || value == SOK_PLAYER_ON_TARGET) {
            g_runtime_map[i] = SOK_TARGET;
        }
    }
    for (i = 0; i < box_count; i++) {
        uint8_t pos = buf[3 + i];
        g_runtime_map[pos] = (g_runtime_map[pos] == SOK_TARGET)
            ? SOK_BOX_ON_TARGET : SOK_BOX;
    }
    g_runtime_map[buf[1]] = (g_runtime_map[buf[1]] == SOK_TARGET)
        ? SOK_PLAYER_ON_TARGET : SOK_PLAYER;

    s_won = sokoban_is_won() ? 2 : 0;
    s_menu_visible = s_won ? 1 : 0;
    s_exit_sliding = 0;
    s_exit_dragging = 0;
    s_hold_button = 0;
    s_hold_us = 0;
    s_menu_cooldown = 0;
    sokoban_draw_level(s_current_level);
    if (s_won) {              /* 未通关时 s_menu_visible=0, 不画按钮面板 */
        sok_win_draw(65, 120);
        sok_redraw_nav_group();
    }
    return 1;
}

/**
 * @brief  菜单可见且按住按钮时的长按切关处理 (真实时间节拍)
 * @note   从 sokoban_update 抽出, 减小超大函数避免 R_RISCV_RVC_JUMP 超范围
 */
static void sokoban_hold_update(void) {
    TouchState ts;
    /* 长按计数中不刷新坐标, 省I2C时间 */
    if (!s_hold_button) {
        Touch_UpdateCoords();
        Touch_Scan(&ts);
        if (s_drag_btn != 4) s_drag_btn = sok_btn_hit_test(ts.x, ts.y);   /* 已作废的手势不复活 */
        if (ts.y > s_btn_max_y) s_btn_max_y = ts.y;
    }
    /* 锁定按住目标, 防手指微移导致计时归零 */
    if (s_hold_us == 0) {
        s_hold_button = (s_drag_btn == 1 || s_drag_btn == 3)
            ? s_drag_btn : 0;
        s_hold_last_us = GetTimerCount();   /* 开始计时基准 */
    }
    if (s_hold_button == 1 || s_hold_button == 3) {
        uint32_t now_us = GetTimerCount();  /* 真实时间节拍, 与帧率无关 */
        s_hold_us += (uint32_t)(now_us - s_hold_last_us);
        s_hold_last_us = now_us;
        if (s_hold_us >= HOLD_START_US) {
            s_won_transition = 0;   /* 自动切关已发生, 清除WON标记 */
            if (s_hold_button == 1 && s_current_level > 0) {
                sokoban_init_level(s_current_level - 1);
                s_menu_visible = 1;
                s_hold_button = 1;    /* 保留, 防prepare_level清零后长按跟踪中断 */
                s_drag_btn = 0;    /* 松手不触发重复切关 */
                s_prev_pressed = 1;    /* 保持按住态, 防下一帧重新初始化 */
                s_menu_cooldown = HOLD_COOLDOWN_FRAMES;  /* 连跳期间永不过期, 松手安全 */
            }
            else if (s_hold_button == 3
                && s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                sokoban_init_level(s_current_level + 1);
                s_menu_visible = 1;
                s_hold_button = 3;    /* 保留, 防prepare_level清零后长按跟踪中断 */
                s_drag_btn = 0;    /* 松手不触发重复切关 */
                s_prev_pressed = 1;    /* 保持按住态, 防下一帧重新初始化 */
                s_menu_cooldown = HOLD_COOLDOWN_FRAMES;  /* 连跳期间永不过期, 松手安全 */
            }
            s_hold_us = HOLD_START_US - HOLD_REPEAT_US;
        }
    }
    else {
        s_hold_us = 0;
    }
}

/**
 * @brief  重绘 [x0,x1]x[y0,y1] 范围内覆盖的地图瓦片
 * @note   电池灭段恢复: 先用 LCD_ClearLowBatteryOverlay 把电池图标整块刷黑(红色像素→黑),
 *         地图外黑区/凸起变黑即无红色残留; 此处仅重绘电池区地图瓦片盖回被刷黑的形状。
 * @param  x0,y0,x1,y1  像素矩形范围(不含右/下边界, 与 LCD_Fill 一致)
 */
static void sok_redraw_battery_tiles(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {

    int16_t br_start = ((int16_t)y0 - g_oy) / (int16_t)g_tile_size;
    int16_t br_end = ((int16_t)y1 - g_oy) / (int16_t)g_tile_size;
    int16_t bc_start = ((int16_t)x0 - g_ox) / (int16_t)g_tile_size;
    int16_t bc_end = ((int16_t)x1 - g_ox) / (int16_t)g_tile_size;

    if (br_start < 0) br_start = 0;
    if (br_end >= (int16_t)g_level_rows) br_end = (int16_t)g_level_rows - 1;
    if (bc_start < 0) bc_start = 0;
    if (bc_end >= (int16_t)g_level_cols) bc_end = (int16_t)g_level_cols - 1;

    if (br_start <= br_end && bc_start <= bc_end) {
        int16_t r, c;
        LCD_CS_CLR;
        for (r = br_start; r <= br_end; r++) {
            for (c = bc_start; c <= bc_end; c++) {
                sokoban_draw_cell_at((uint8_t)r, (uint8_t)c);
            }
        }
        LCD_CS_SET;
    }
}

/**
 * @brief  低电图标灭段恢复: 按菜单覆盖范围重绘电池区瓦片。
 * @note   设置模式重绘电池区; 纯游戏重绘整屏。设置模式下 sokoban_update() 不跑,
 *         g_sok_dirty 无人消费, 故需立即恢复: 只重绘条带右缘外凸点 + 条带未盖住时
 *         框外露出的电池主体; 非设置模式保持置脏, 由下一帧 sokoban_update() 恢复。
 */
void sokoban_lowbatt_restore(void) {
    if (g_settings_from_game) {
        LCD_ClearLowBatteryOverlay();
        /* 二级设置菜单条带(x=45~195)盖住电池主体, 不重绘框内(否则先瓦片后条带闪一帧);
           只重绘条带右缘外的凸点(x=195~210) + 条带未盖住时框外露出的电池主体(滑出中) */
        sok_redraw_battery_tiles(195, 104, 210, 136);
        if (s_video_menu_slide_y > 79) {
            int16_t ye = (s_video_menu_slide_y < 161) ? s_video_menu_slide_y : 161;
            sok_redraw_battery_tiles(45, 79, 195, ye);
        }
    }
    else {
        sokoban_full_redraw();
    }
}

/**
 * @brief  推箱子每帧更新: 处理触摸、菜单、移动与渲染。
 * @return GAME_RET_* 返回命令
 */
uint8_t sokoban_update(void) {
    uint8_t interacted = 0;

    /* 菜单冷却递减 */
    if (s_menu_cooldown > 0) s_menu_cooldown--;
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 设置菜单中顶部下滑→退出菜单 (main.c 设标志, 四游戏各自消费; 推箱子需自行处理) */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        s_menu_visible = 0;
        s_exit_menu_visible = 1;
        s_exit_dragging = 1;
        s_exit_sliding = 0;
        s_exit_drag_start_y = 0;   /* game_menu_drag_poll 内部用 gdp_start_ty 记录, 此值仅作输出 */
        s_exit_drag_y = -MENU_H + MENU_DRAG_MAX_STEP;
        s_hold_button = 0;
        s_hold_us = 0;
        s_prev_pressed = 1;   /* 吸收下一帧触摸抖动 */
        sokoban_draw_level(s_current_level);   /* 设置下滑已把屏幕刷黑, 需重绘整图 */
    }

    /* ---- 电池闪灭恢复: 整块刷黑图标(红→黑)+重绘该处瓦片 ---- */
    if (g_sok_dirty) {
        LCD_ClearLowBatteryOverlay();
        /* 电池凸点/右边框落在菜单框右缘(x=195)之外, 任何状态都要补 */
        sok_redraw_battery_tiles(195, 79, 210, 161);
        if (!s_exit_menu_visible) {
            /* 重绘UI覆盖层: 二级菜单框(盖住电池区) / 导航按钮 / 通关画面。
               撤销按钮在 y=189 以下, 电池区重绘够不到它, 不用管 */
            if (s_lvl2_visible) {
                menu_draw_nrow_frame_only(RESTART_MENU_Y - 1, sok_lvl2_row_types, 3);
            }
            else {
                /* 只补面板上缘(s_menu_slide_y, 纯游戏=240)以上露出的电池区: 跟手滑动时面板
                   上缘每帧都在变, 先补瓦片再由面板盖回去会闪一帧, 面板盖住的部分交给面板 */

                sok_redraw_battery_tiles(45, 79, 195, s_menu_slide_y);
                if (s_menu_visible) {
                    if (s_won) sok_win_draw(65, 120);
                    sok_redraw_nav_group();
                }
            }
        }
        else {
            /* 退出菜单框(x=45~195)盖住电池主体, 不重绘框内(否则先瓦片后菜单闪一帧);
               只重绘菜单拖拽中(未盖住电池)框外露出的主体(凸点已在上方补过) */
            {
                int16_t mb = s_exit_drag_y + MENU_H;   /* 菜单框底部y */
                sok_redraw_battery_tiles(45, mb, 195, 161);
            }
            menu_draw_nrow_frame_only(s_exit_drag_y, sok_row_types, 5);
        }
        g_sok_dirty = 0;
    }

    if (s_exit_menu_visible) goto process_exit_menu;
    game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 0);

    /* ---- 二级菜单 / 撤销界面: 独立处理, 本帧到此为止 ---- */
    if (s_lvl2_visible) {
        uint8_t r = sok_lvl2_update();
        return r ? r : 1;
    }
    if (s_undo_visible) {
        sok_undo_ui_update();
        return 1;
    }

    /* ---- 菜单可见时持续绘制(关卡编号UI) ---- */
    if (s_menu_visible) {
        sok_level_draw(s_level_num, LCD_RED);
    }

    /* ---- 通关状态 ---- */
    if (s_won) {
        /* 通关后延时0.5s再显示菜单(胜利动画+按钮) */
        if (s_won == 1) {
            Delay_Ms(500);
            s_menu_visible = 1;
            sok_redraw_nav_group();
            s_won = 2;
        }

        /* 已通关时处理按钮触摸 —— 按下即切第一关, 长按在menu_visible无缝继续 */
        TouchState ts;
        Touch_Scan(&ts);

        if (!s_prev_pressed && ts.pressed) {
            interacted = 1;
            s_hold_us = 0;
            if (ts.x < SOK_BTN1_X + SOK_BTN_W && ts.y >= SOK_BTN_HIT_Y0 && ts.y < SOK_BTN_HIT_Y1) {
                if (s_current_level > 0) {
                    sokoban_init_level(s_current_level - 1);
                    s_hold_button = 1;
                    s_drag_btn = 1;   /* 让menu_visible保持长按跟踪 */
                    s_won_transition = 1;  /* 标记来自WON, menu_visible首次松手不切关 */
                }
            }
            else if (ts.x >= SOK_BTN2_X && ts.x < SOK_BTN2_X + SOK_BTN_W && ts.y >= SOK_BTN_HIT_Y0 && ts.y < SOK_BTN_HIT_Y1) {
                sokoban_restart_current_level();
            }
            else if (ts.x >= SOK_BTN3_X && ts.y >= SOK_BTN_HIT_Y0 && ts.y < SOK_BTN_HIT_Y1) {
                if (s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                    sokoban_init_level(s_current_level + 1);
                    s_hold_button = 3;
                    s_drag_btn = 3;   /* 让menu_visible保持长按跟踪 */
                    s_won_transition = 1;  /* 标记来自WON, menu_visible首次松手不切关 */
                }
            }
            /* 记录本次触摸起点用于后续滑动检测 */
            s_touch_start_x = ts.x;
            s_touch_start_y = ts.y;
            s_swipe_origin_y = ts.y;
            s_btn_max_y = ts.y;
            s_prev_pressed = 1;
        }
        /* 下滑进入退出菜单 (未命中非restart按钮时生效) */
        if (ts.pressed && s_won) {
            Touch_UpdateCoords();
            Touch_Scan(&ts);
            if (sok_try_open_exit_menu(ts.y)) {
                s_hold_button = 0;
                return interacted;
            }
        }
        /* 松手下滑提交退出菜单 */
        if (!s_prev_pressed) {
            int16_t exit_dy =
                (int16_t)ts.y - (int16_t)s_swipe_origin_y;
            if (exit_dy >= MENU_SWIPE_COMMIT_DIST
                && s_swipe_origin_y < MENU_SWIPE_START_Y
                && s_menu_cooldown == 0) {
                s_exit_menu_visible = 1;
                s_exit_dragging = 0;
                s_exit_sliding = 1;
                s_exit_drag_y = -MENU_H;
                s_prev_pressed = 0;
                return interacted;
            }
        }
        if (!ts.pressed) {
            s_hold_button = 0;
            s_hold_us = 0;
        }

        s_prev_pressed = ts.pressed;
        return interacted;
    }

    /* ---- 菜单可见状态 (拖拽追踪: 按下记录→拖拽更新→松手执行) ---- */
    if (s_menu_visible) {
        TouchState ts;
        Touch_Scan(&ts);

        /* 由上划进入菜单: 屏蔽按钮交互, 等手指完全松开再恢复 */
        if (s_menu_from_swipe) {
            if (ts.pressed) {
                s_prev_pressed = 1;
                return interacted;
            }
            /* 手指松开, 清除保护, 下一帧正常交互 */
            s_menu_from_swipe = 0;
            s_prev_pressed = 0;
            s_drag_btn = 0;
            s_hold_us = 0;
            return interacted;
        }

        /* 按下: 记录命中按钮 */
        if (!s_prev_pressed && ts.pressed) {
            interacted = 1;
            s_drag_btn = sok_btn_hit_test(ts.x, ts.y);
            s_hold_us = 0;
            s_hold_button = 0;
            s_prev_pressed = 1;
            s_touch_start_x = ts.x;
            s_touch_start_y = ts.y;
            s_swipe_origin_y = ts.y;
            s_btn_max_y = ts.y;
            return interacted;
        }

        /* 按住中: 拖拽追踪 + 长按连跳(btn1/btn3有效时) */
        if (ts.pressed) {
            /* 下划打断按钮菜单, 打开退出菜单 */
            if (s_swipe_origin_y < MENU_SWIPE_START_Y
                && s_menu_cooldown == 0) {
                Touch_UpdateCoords();
                Touch_Scan(&ts);
                if (sok_try_open_exit_menu(ts.y)) {
                    s_menu_visible = 0;
                    s_hold_button = 0;
                    s_hold_us = 0;
                    /* 清除按钮区域: 先黑底再重绘游戏格子, 防边缘残留 */
                    LCD_Fill(SOK_BTN1_X, SOK_BTN_Y, SOK_BTN1_X + SOK_BTN_W, SOK_BTN_Y + SOK_BTN_H + 2, LCD_BLACK);
                    LCD_Fill(SOK_BTN2_X, SOK_BTN_Y, SOK_BTN2_X + SOK_BTN_W, SOK_BTN_Y + SOK_BTN_H + 2, LCD_BLACK);
                    LCD_Fill(SOK_BTN3_X, SOK_BTN_Y, SOK_BTN3_X + SOK_BTN_W, SOK_BTN_Y + SOK_BTN_H + 2, LCD_BLACK);
                    sok_redraw_grid_below(SOK_BTN_Y);
                    return interacted;
                }
            }

            /* 二次上划: 按钮面板已显示时, 再次上划打开二级菜单(撤销/设置/返回)
             * 起手位置与一级按钮一致(最底部窄带 Y > SOK_SWIPE_Y), 距离也用同一个 5px */
            if (s_menu_cooldown == 0
                && (int16_t)s_swipe_origin_y - (int16_t)ts.y > MENU_SWIPE_TRIGGER_DIST) {
                int16_t up_move;
                Touch_UpdateCoords();
                Touch_Scan(&ts);
                up_move = (int16_t)s_swipe_origin_y - (int16_t)ts.y;
                if (up_move > MENU_SWIPE_TRIGGER_DIST) {
                    if (s_swipe_origin_y > SOK_SWIPE_Y) {
                        s_hold_button = 0;
                        s_hold_us = 0;
                        s_prev_pressed = 1;    /* 按住态, 防下一帧误触 */
                        /* 局部清除按钮面板区(与按钮面板退出一致): 只清文字窄带 + 按钮带两个小区域,
                           不全宽/不全屏重绘(会闪)。 */
                        sok_close_nav_panel();
                        sok_panel_drag_start(1, up_move);   /* 二级菜单跟手滑入 */
                        return interacted;
                    }
                    /* 按钮区起手划动>20px: 本手势作废(4) —— 不连跳, 松手也不点击 */
                    if (up_move > MENU_SWIPE_COMMIT_DIST) {
                        s_drag_btn = 4;
                        s_hold_button = 0;   /* 已开始锁定的长按计时也要停, 否则1s后仍会连跳 */
                        s_hold_us = 0;
                        return interacted;
                    }
                }
            }
            sokoban_hold_update();
        }

        /* 松手: 执行拖拽目标按钮 */
        if (s_prev_pressed && !ts.pressed) {
            if (s_won_transition) {  /* WON切关后首次松手, 拦截不切, 清除标记 */
                s_won_transition = 0;
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_prev_pressed = 0;
                return interacted;
            }
            if (s_menu_cooldown > 0) {  /* 长按自动切关后的冷却期内松手 */
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_prev_pressed = 0;
                return interacted;
            }
            if (s_hold_button != 0 && s_drag_btn == 0 && s_hold_us > 0) {  /* 长按自动切关已发生(drag_btn被清零), 松手不做额外操作 */
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_prev_pressed = 0;
                return interacted;
            }
            /*  按钮栏下滑>5px取消 */
            if (s_btn_max_y - s_touch_start_y > 5) {
                /* 窄带清除: 文字带+按钮带, 避免整块黑闪 */
                sok_close_nav_panel();
                s_drag_btn = 0;
                s_hold_button = 0;
                s_hold_us = 0;
                s_menu_cooldown = 15;
                s_prev_pressed = 0;
                return interacted;
            }
            if (s_drag_btn == 1 && s_current_level > 0) {
                sokoban_init_level(s_current_level - 1);
                s_menu_visible = 1;
            }
            else if (s_drag_btn == 2) {
                sokoban_restart_current_level();
            }
            else if (s_drag_btn == 3
                && s_current_level < SOKOBAN_LEVEL_COUNT - 1) {
                sokoban_init_level(s_current_level + 1);
                s_menu_visible = 1;
            }
            else if (s_drag_btn == 0 && !s_menu_from_swipe) {
                /* 非上划的菜单: 点击空白区关闭菜单开始游戏 */
                sok_close_nav_panel();
                s_menu_cooldown =
                    (s_swipe_origin_y < MENU_SWIPE_START_Y) ? 0 : 15;
            }
            s_drag_btn = 0;
            s_hold_button = 0;
            s_hold_us = 0;
        }

        s_prev_pressed = ts.pressed;
        return interacted;
    }

    /* ---- 跟手上划: 拖拽模式 (限速, 跟随手指) ---- */
    /* 面板滑动/动画中: 新的下划手势可直接打断, 改开退出菜单(与静止态一致) */
    if (s_menu_sliding || s_menu_slide_anim) {
        TouchState ts_c;
        Touch_Scan(&ts_c);
        if (!ts_c.pressed) s_prev_pressed = 0;
        else if (!s_prev_pressed) {
            s_prev_pressed = 1;
            s_swipe_origin_y = ts_c.y;
        }
        else if (sok_try_open_exit_menu(ts_c.y)) {
            s_menu_sliding = 0;
            s_menu_slide_anim = 0;
            sok_panel_clear_from(s_menu_slide_y);
            sok_redraw_grid_below(s_menu_slide_y);
            s_menu_slide_y = 240;
            return interacted;
        }
    }

    if (s_menu_sliding) {
        TouchState ts_s;
        Touch_UpdateCoords();  /* 实时读取触摸坐标, 保证跟手流畅 */
        Touch_Scan(&ts_s);

        if (ts_s.pressed) {
            int16_t total_dy = (int16_t)s_swipe_origin_y - (int16_t)ts_s.y;
            if (total_dy > s_slide_max_dy) s_slide_max_dy = total_dy;
            int16_t target = 240 - total_dy;
            if (target < s_panel_rest) target = s_panel_rest;
            if (target > 240) target = 240;
            /* 限速: 每帧最多移动 SOK_SLIDE_MAX_STEP, 防跳变 */
            if (target < s_menu_slide_y) {
                int16_t step = s_menu_slide_y - target;
                if (step > SOK_SLIDE_MAX_STEP) step = SOK_SLIDE_MAX_STEP;
                s_menu_slide_y -= step;
            }
        }

        if (!ts_s.pressed) {
            /* 松手: 从最远位移回退>2px则取消, 否则自动滑入 */
            int16_t total_dy = (int16_t)s_swipe_origin_y - (int16_t)ts_s.y;
            int16_t back_dist = s_slide_max_dy - total_dy;
            if (back_dist > 2) {
                s_menu_sliding = 0;
                int16_t clip_y = s_menu_slide_y;
                if (clip_y > 239) clip_y = 239;
                s_menu_slide_y = 240;
                s_prev_pressed = 0;  /* 释放触摸态, 防止下一帧误触发 */
                s_menu_cooldown = 15; /* 冷却, 防立即重触发上划 */
                sok_panel_clear_from(clip_y);
                sok_redraw_grid_below(clip_y);
            }
            else {
                s_menu_sliding = 0;
                s_menu_slide_anim = 1;
            }
            return 0;
        }

        /* 绘制 (仅按住时到达) */
        if (s_menu_slide_y <= s_panel_rest) {
            /* 拖到底: 切换为面板可见模式 */
            s_menu_sliding = 0;
            s_prev_pressed = 1;      /* 按住态, 防止下一帧误触 */
            s_menu_from_swipe = 1;   /* 防止首帧误触关闭面板 */
            if (s_panel_kind == 1) {
                s_lvl2_visible = 1;  /* 二级菜单: 后面交给 sok_lvl2_update 处理触摸 */
            } else {
                s_menu_visible = 1;
                s_drag_btn = sok_btn_hit_test(ts_s.x, ts_s.y);
                sok_level_draw(s_level_num, LCD_RED);
            }
            sok_panel_draw_at(s_panel_rest);
        }
        else {
            sok_panel_draw_at(s_menu_slide_y);
        }
        return 0;
    }

    /* ---- 跟手上划: 自动滑动动画 (松手后滑到最终位置) ---- */
    if (s_menu_slide_anim) {
        s_menu_slide_y -= SOK_SLIDE_SPEED;
        if (s_menu_slide_y <= s_panel_rest) {
            s_menu_slide_y = s_panel_rest;
            s_menu_slide_anim = 0;
            s_menu_from_swipe = 1;  /* 防止首帧误触关闭面板 */
            s_prev_pressed = 1;     /* 吸收动画后触摸抖动 */
            if (s_panel_kind == 1) {
                s_lvl2_visible = 1;
            } else {
                s_menu_visible = 1;
                s_drag_btn = 0;     /* 清除旧拖拽状态 */
                sok_level_draw(s_level_num, LCD_RED);
            }
            sok_panel_draw_at(s_menu_slide_y);
        }
        else {
            sok_panel_draw_at(s_menu_slide_y);
        }
        return 0;
    }

process_exit_menu:
    /* ---- 退出菜单不刷新电池图标，其他菜单保持计时器运行 ---- */
    if (!s_exit_menu_visible) {
        game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 0);
    }
    /* ---- 退出菜单处理 (拖拽模式) ---- */
    if (s_exit_menu_visible) {
        /* 拖拽模式处理 */
        if (s_exit_dragging) {
            /* 上划打断退出菜单拖拽 */
            if (sok_exit_swipe_cancel()) return 0;

            TouchState ts_d;
            Touch_Scan(&ts_d);
            uint8_t poll_ret = game_menu_drag_poll(&s_exit_drag_y, &s_exit_drag_start_y,
                &s_exit_dragging, &ts_d, sok_row_types);
            if (poll_ret == 2) {
                /* 上划回退>10px松手取消: 清可见区域并补画格子 */
                menu_clear_exit_area_to(s_exit_drag_y);
                s_exit_menu_visible = 0;
                s_exit_sliding = 0;
                s_exit_dragging = 0;
                sok_redraw_grid_below((s_exit_drag_y < 0) ? 0 : s_exit_drag_y);
                s_prev_pressed = 1;
                s_menu_cooldown = 15;
            }
            else if (poll_ret == 1) {
                s_exit_sliding = 1;
            }
            return 0;
        }

        /* 滑动动画模式: 自动滑至目标, 逐帧刷新 */
        if (s_exit_sliding) {

            /* 上划打断退出菜单滑动动画 */
            if (sok_exit_swipe_cancel()) return 0;

            s_exit_drag_y += MENU_SLIDE_SPEED;
            if (s_exit_drag_y >= MENU_Y) {
                s_exit_drag_y = MENU_Y;
                s_exit_sliding = 0;
                s_prev_pressed = 0;  /* 滑入结束重置按下态, 防拖拽松手被误判为点菜单外关闭 */
            }
            menu_draw_nrow_frame_only(s_exit_drag_y, sok_row_types, 5);
            return 0;
        }

        TouchState ts;

        /* 上划打断退出菜单(静止态) */
        if (sok_exit_swipe_cancel()) return 0;
        /* (底部/中间区域上划都在上一行的调用里统一处理) */
        uint8_t ret = game_exit_menu_poll(sokoban_exit_map, &s_prev_pressed, &ts);
        if (ret == EXIT_RET_BACK) {
            /* "返回" 键退出菜单回到游戏, 擦除菜单区域, 补画地图 */
            s_exit_menu_visible = 0;
            s_exit_sliding = 0;
            s_exit_dragging = 0;
            s_menu_visible = 0;
            LCD_CLEAR_MENU_AREA();
            sok_redraw_grid_below(MENU_Y);
            if (s_won) {
                s_menu_visible = 1;
                sok_win_draw(65, 120);
                sok_redraw_nav_group();
            }
            game_battery_icon_update(SOK_BATTERY_X, SOK_BATTERY_Y, 1);
            s_prev_pressed = 1;
            s_touch_start_x = ts.x;
            s_touch_start_y = ts.y;
            s_swipe_origin_y = ts.y;
            s_menu_cooldown = 15;
            return 0;
        }
        if (ret != EXIT_RET_NONE) {
            s_exit_menu_visible = 0;
            return ret;
        }
        return 0;
    }

    /* ---- 游戏进行中 ---- */
    TouchState ts;
    Touch_Scan(&ts);

    if (!s_prev_pressed && ts.pressed) {
        interacted = 1;
        s_touch_start_x = ts.x;
        s_touch_start_y = ts.y;
        s_swipe_origin_y = ts.y;   // 记录触摸起始Y(用于退出菜单手势)
    }

    if (ts.pressed) {
        Touch_UpdateCoords();
        Touch_Scan(&ts);

        /* 检测退出菜单手势: 上方区域 + 向下滑动 > 5px 则可拖出退出菜单 */
        if (sok_try_open_exit_menu(ts.y)) return 0;

        /* 底部上划: 跟下滑菜单一样, 移动>5px即启动跟手滑动 */
        if (s_swipe_origin_y > SOK_SWIPE_Y
            && s_menu_cooldown == 0) {
            int16_t swipe_dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
            if (swipe_dy < 0 && -swipe_dy > MENU_SWIPE_TRIGGER_DIST) {
                if (s_undo_group) {
                    /* 撤销模式: 上划显示撤销按钮组(固定位置, 不跟手);
                       本次手势的松手要吸收掉, 否则会落在按钮上误触发 */
                    s_undo_visible = 1;
                    s_menu_from_swipe = 1;
                    sok_draw_undo_buttons();
                    s_prev_pressed = 1;
                    return interacted;
                }
                sok_panel_drag_start(0, -swipe_dy);   /* 导航按钮组跟手滑入 */
                s_drag_btn = sok_btn_hit_test(ts.x, ts.y);
                s_prev_pressed = 1;
                return 0;
            }
        }

        int16_t dx = (int16_t)ts.x - (int16_t)s_touch_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_touch_start_y;
        int16_t adx = (dx < 0) ? -dx : dx;
        int16_t ady = (dy < 0) ? -dy : dy;

        SokobanDir dir = SOK_DIR_NONE;
        if (adx > ady && adx > SOK_HOLD_THRESHOLD) {
            dir = (dx > 0) ? SOK_DIR_RIGHT : SOK_DIR_LEFT;
        }
        else if (ady > SOK_HOLD_THRESHOLD) {
            dir = (dy > 0) ? SOK_DIR_DOWN : SOK_DIR_UP;
        }

        if (dir != SOK_DIR_NONE) {
            /* 过滤误触: 顶部区域不触发上滑, 底部区域按下滑 */
            if ((dir == SOK_DIR_UP && s_swipe_origin_y > SOK_SWIPE_Y)
                || (dir == SOK_DIR_DOWN
                    && s_swipe_origin_y < MENU_SWIPE_START_Y)) {
                s_touch_start_x = ts.x;
                s_touch_start_y = ts.y;
            }
            else {
                sokoban_move(dir);
                if (sokoban_is_won()) {
                    sok_win_draw(65, 120);
                    s_won = 1;
                }
                s_touch_start_x = ts.x;
                s_touch_start_y = ts.y;
            }
        }
    }

    if (s_prev_pressed && !ts.pressed) {
        int16_t dx = (int16_t)ts.x - (int16_t)s_touch_start_x;
        int16_t dy = (int16_t)ts.y - (int16_t)s_touch_start_y;
        int16_t adx = (dx < 0) ? -dx : dx;
        int16_t ady = (dy < 0) ? -dy : dy;

        /* 释放检测: 退出菜单手势优先于移动方向解析 */
        int16_t exit_dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
        if (exit_dy >= MENU_SWIPE_COMMIT_DIST
            && s_swipe_origin_y < MENU_SWIPE_START_Y
            && s_menu_cooldown == 0) {
            s_exit_menu_visible = 1;
            s_exit_sliding = 1;
            s_exit_drag_y = -MENU_H;
            s_prev_pressed = 0;
            return interacted;
        }

        /* 显示菜单手势: 底部区域上滑(长距离滑动生效) */
        int16_t swipe_dy = (int16_t)ts.y - (int16_t)s_swipe_origin_y;
        int16_t swipe_ady = (swipe_dy < 0) ? -swipe_dy : swipe_dy;

        /* 底部区域向上滑动超过阈值时, 显示按钮组:
           撤销模式显示"上一步/返回/下一步", 否则显示导航(上一关+重新开始+下一关);
           起手位置都在最底部那条窄带内(Y > SOK_SWIPE_Y), 免得在按钮区随手划一下就弹 */
        if (swipe_ady > MENU_SWIPE_TRIGGER_DIST
            && swipe_dy < 0 && s_swipe_origin_y > SOK_SWIPE_Y && s_menu_cooldown == 0) {
            if (s_undo_group) {
                s_undo_visible = 1;
                sok_draw_undo_buttons();
            } else {
                sok_draw_nav_buttons_at(SOK_BTN_Y);   /* 按语言选中/英文那套, 并自带首末关判断 */
                s_menu_visible = 1;
            }
            s_prev_pressed = 0;
            return interacted;
        }

        SokobanDir dir = SOK_DIR_NONE;
        if (adx > ady && adx > SOK_RELEASE_THRESHOLD) {
            dir = (dx > 0) ? SOK_DIR_RIGHT : SOK_DIR_LEFT;
        }
        else if (ady > SOK_RELEASE_THRESHOLD) {
            dir = (dy > 0) ? SOK_DIR_DOWN : SOK_DIR_UP;
        }

        if (dir != SOK_DIR_NONE
            && !(s_swipe_origin_y < MENU_SWIPE_START_Y
                && dir == SOK_DIR_DOWN)) {
            sokoban_move(dir);
            if (sokoban_is_won()) {
                sok_win_draw(65, 120);
                s_won = 1;
                s_undo_group = 0;   /* 通关后回到普通导航按钮组(可点"下一关") */
            }
        }
    }

    s_prev_pressed = ts.pressed;
    return interacted;
}
#ifdef SIM_BUILD
/* ==================================================================
 *  sim-only -- never compiled into the device image (wasm/build.ps1 adds
 *  -DSIM_BUILD).  The simulator's touch-region overlay asks whichever module
 *  owns the screen to report its regions, so it can never drift from the real
 *  hit-test -- sokoban's panel state is file-static here, so only this file
 *  can answer it.
 * ================================================================== */
int sokoban_sim_touch_rects(void)
{
    if (s_exit_menu_visible) {                  /* exit menu: 5 rows, MENU geometry */
        sim_touch_add_rows(MENU_X, MENU_W, MENU_Y, MENU_Y + MENU_H - 1, 5, SIM_TOUCH_TAP);
        return 1;
    }
    if (s_lvl2_visible) {                       /* level-2 menu: 3 rows, bottom aligned */
        sim_touch_add_rows(RESTART_MENU_X, RESTART_MENU_W,
                           RESTART_MENU_Y, 239, 3, SIM_TOUCH_TAP);
        return 1;
    }

    /* playing: swipe anywhere to move + the two edge gesture bands */
    sim_touch_add(0, 0, 240, 240, SIM_TOUCH_SWIPE);
    sim_touch_add(0, 0, 240, MENU_SWIPE_START_Y, SIM_TOUCH_BAND);
    sim_touch_add(0, SOK_SWIPE_Y + 1, 240, 240, SIM_TOUCH_BAND);

    if (s_undo_visible) {                       /* undo / back / redo */
        sim_touch_add(0, SOK_UNDO_HIT_Y0, SOK_UNDO_HIT_SPLIT1, SOK_UNDO_HIT_Y1, SIM_TOUCH_TAP);
        sim_touch_add(SOK_UNDO_HIT_SPLIT1, SOK_UNDO_HIT_Y0, SOK_UNDO_HIT_SPLIT2, SOK_UNDO_HIT_Y1, SIM_TOUCH_TAP);
        sim_touch_add(SOK_UNDO_HIT_SPLIT2, SOK_UNDO_HIT_Y0, 240, SOK_UNDO_HIT_Y1, SIM_TOUCH_TAP);
    }
    else if (s_menu_visible) {                  /* prev level / restart / next level */
        sim_touch_add(0, SOK_BTN_HIT_Y0, SOK_BTN_HIT_SPLIT1, SOK_BTN_HIT_Y1, SIM_TOUCH_TAP);
        sim_touch_add(SOK_BTN_HIT_SPLIT1, SOK_BTN_HIT_Y0, SOK_BTN_HIT_SPLIT2, SOK_BTN_HIT_Y1, SIM_TOUCH_TAP);
        sim_touch_add(SOK_BTN_HIT_SPLIT2, SOK_BTN_HIT_Y0, 240, SOK_BTN_HIT_Y1, SIM_TOUCH_TAP);
    }
    return 1;
}
#endif
#ifdef SIM_BUILD
/* ---- sim-only: 长按诊断探针 (which: 0=累计ms 1=hold按钮 2=drag按钮 3=菜单冷却 4=面板位) ---- */
int sokoban_sim_hold_info(int which)
{
    switch (which) {
    case 0:  return (int)(s_hold_us / 1000);
    case 1:  return (int)s_hold_button;
    case 2:  return (int)s_drag_btn;
    case 3:  return (int)s_menu_cooldown;
    case 4:  return (int)(s_menu_visible | (s_undo_visible << 1) | (s_lvl2_visible << 2));
    case 5:  return (int)s_exit_menu_visible;
    default: return -1;
    }
}
#endif