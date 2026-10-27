/**
 * @file    menu.c
 * @brief   菜单公共函数 (统一边框绘制/图标/碰撞检测)
 */

#include "menu.h"           /* 菜单状态机/行类型/共享触摸全局 */
#include "sok_img_win.h"    /* 返回/重开等图片资源 */
#include "chinese_fonts.h"  /* 中文字库 ID */
#include "flash_store.h"    /* 菜单字库从外部 Flash 读取 */
#include "settings.h"       /* 视频/游戏设置页 */
#include "decompress.h"     /* 视频块加载 */
#include "sokoban.h"        /* 推箱子重置触摸 */
#include "d2048.h"          /* 2048 重置触摸 */
#include "fish.h"           /* 大鱼吃小鱼重置触摸 */
#include "tetris.h"         /* 俄罗斯方块重置触摸/游戏返回码 */
#include "klotski.h"        /* 华容道重置触摸 */
#include "adc.h"            /* ADC 采样(电池/充电电压) */
#include "battery.h"        /* 电池电量/图标 */
#include "timer.h"          /* GetTimerCount 计时(微秒) */

 /* ---- 视频上滑菜单参数 (集中置顶) ---- */
#define VIDEO_MENU_DRAG_MAX_STEP 4       /* 拖拽单步最大移动 */
/* VIDEO_SWIPE_START_Y 已上移到 menu.h (设置页避让区间 / 模拟器触摸区域叠加层也要用) */
#define VIDEO_SWIPE_TRIGGER   5          /* 上划触发距离 */

/* ---- 菜单字体缓存/行类型常量 (集中置顶) ---- */
/* 菜单24x24字库: 从外部Flash整体读入RAM缓存(条带渲染需逐行随机访问)
   槽位 0~21 = 资源ID 0~21(常用汉字); 槽位 22/23 = "语/言"(资源ID 115/116, 见 MENU_FONT_SLOT) */
#define MENU_FONT_SLOTS  45   /* 0~23 原有(含语/言) + 24~44 菜单追加的 21 个字(外部保留区) */
static uint8_t s_menu_fonts[MENU_FONT_SLOTS * 72];
static uint8_t s_menu_fonts_ok = 0;
static uint8_t s_fonts_lang = 0xFF;   /* 缓存里装的是哪套字: 0=中文, 1=英文 */

/* 资源ID → 缓存槽位:
   0~21 原样; 语(115)/言(116) 顺延到 22/23;
   追加的 21 个字(虚ID 128~148, 字模存外部Flash保留区) 放 24~44 */
#define MENU_EXT_SLOT     24
#define MENU_FONT_SLOT(id)  ((uint8_t)(((id) >= MENU_EXT_ID_BASE) \
                            ? (MENU_EXT_SLOT + ((id) - MENU_EXT_ID_BASE)) \
                            : (((id) >= FLASH_STORE_ID_24_YU2) \
                               ? (22u + ((id) - FLASH_STORE_ID_24_YU2)) : (id))))

/* 菜单行用到的汉字ID表 (替代原字体指针数组, 数据存外部Flash) */
static const uint8_t s_fonts_tui_id[3] = { FLASH_STORE_ID_24_TUI, FLASH_STORE_ID_24_XIANG, FLASH_STORE_ID_24_ZI };
static const uint8_t s_fonts_sp_id[2] = { FLASH_STORE_ID_24_SHI, FLASH_STORE_ID_24_PIN };
static const uint8_t s_fonts_fh_id[2] = { FLASH_STORE_ID_24_FAN, FLASH_STORE_ID_24_HUI };
static const uint8_t s_fonts_cx_id[4] = { FLASH_STORE_ID_24_CHONG, FLASH_STORE_ID_24_XIN, FLASH_STORE_ID_24_KAI, FLASH_STORE_ID_24_SHI3 };
static const uint8_t s_fonts_ld_id[4] = { FLASH_STORE_ID_24_LIANG, FLASH_STORE_ID_24_DU, FLASH_STORE_ID_24_SHE, FLASH_STORE_ID_24_ZHI };
static const uint8_t s_fonts_xp_id[4] = { FLASH_STORE_ID_24_XI2, FLASH_STORE_ID_24_PING, FLASH_STORE_ID_24_SHI2, FLASH_STORE_ID_24_JIAN };
static const uint8_t s_fonts_pm_id[4] = { FLASH_STORE_ID_24_PING, FLASH_STORE_ID_24_MU, FLASH_STORE_ID_24_FAN2, FLASH_STORE_ID_24_XIANG2 };
static const uint8_t s_fonts_yy_id[4] = { FLASH_STORE_ID_24_YU2, FLASH_STORE_ID_24_YAN, FLASH_STORE_ID_24_SHE, FLASH_STORE_ID_24_ZHI };

/* 三个游戏名(下滑菜单/主菜单里那三行): 字模存外部Flash保留区, 虚ID 128~148 */
static const uint8_t s_fonts_els_id[4]  = { MENU_EXT_ID_E, MENU_EXT_ID_LUO, MENU_EXT_ID_SI, MENU_EXT_ID_KUAI };            /* 俄罗斯块 */
static const uint8_t s_fonts_hrd_id[3]  = { MENU_EXT_ID_HUA, MENU_EXT_ID_RONG, MENU_EXT_ID_DAO };                         /* 华容道 */
static const uint8_t s_fonts_dyxy_id[4] = { MENU_EXT_ID_DA, MENU_EXT_ID_YU, MENU_EXT_ID_XIAO, MENU_EXT_ID_YU };           /* 大鱼小鱼 */
static const uint8_t s_fonts_szxr_id[4] = { MENU_EXT_ID_SHU, MENU_EXT_ID_ZI, MENU_EXT_ID_XIANG, MENU_EXT_ID_RONG2 };       /* 数字相融 */
static const uint8_t s_fonts_cx2_id[2]  = { MENU_EXT_ID_CHE,  MENU_EXT_ID_XIAO3 };                                          /* 撤销(推箱子二级菜单) */

/* 双语行文字表: 中文=字模ID数组(24宽全宽字), 英文=ASCII串(16宽半宽字, 见 chinese_fonts.h)
   英文为 0 的行表示语言无关(2048数字行/图标行), 不在此表里 */
typedef struct {
    const uint8_t* cn;      /* 中文字模ID数组 */
    const char*    en;      /* 英文串 (可为0) */
    uint8_t        cn_cnt;
} MenuRowText;

static const MenuRowText s_row_text[MENU_ROW_MAX + 1] = {
    [MENU_ROW_TUI]      = { s_fonts_tui_id,      "Sokoban",  3 },
    [MENU_ROW_SHIPIN]   = { s_fonts_sp_id,       "Video",    2 },
    [MENU_ROW_CHONGXIN] = { s_fonts_cx_id,       "Restart",  4 },
    [MENU_ROW_SHEZHI]   = { s_fonts_ld_id + 2,   "Settings", 2 },
    [MENU_ROW_LDSZ]     = { s_fonts_ld_id,       "Bright",   4 },   /* 行内可用宽度只有 130px */
    [MENU_ROW_XPSJ]     = { s_fonts_xp_id,       "Sleep",    4 },
    [MENU_ROW_PMFX]     = { s_fonts_pm_id,       "Rotation", 4 },
    [MENU_ROW_YYSH]     = { s_fonts_yy_id,       "Language", 4 },
    [MENU_ROW_FANHUI]   = { s_fonts_fh_id,       "Back",     2 },
    /* 三个游戏名: 中文用追加字模(俄罗斯块/华容道/大鱼小鱼), 英文用半宽拉丁字;
       2048 行中英文都改成文字了(数字相融 / Merge), 不再画 2048 数字 */
    [MENU_ROW_2048]     = { s_fonts_szxr_id,     "Merge",    4 },
    [MENU_ROW_TETRIS]   = { s_fonts_els_id,      "Tetris",   4 },
    [MENU_ROW_KLOTSKI]  = { s_fonts_hrd_id,      "Klotski",  3 },
    [MENU_ROW_FISH]     = { s_fonts_dyxy_id,     "Big Fish", 4 },
    [MENU_ROW_CHEXIAO]  = { s_fonts_cx2_id,      "Step Back", 2 },   /* 撤销 (推箱子二级菜单) */
};
/* 主菜单行类型 (供 main_menu_draw / main_menu_draw_frame_only 共用, 布局一致) */
static const uint8_t g_main_menu_row_types[5] = {
    MENU_ROW_TUI, MENU_ROW_2048, MENU_ROW_TETRIS, MENU_ROW_KLOTSKI, MENU_ROW_FISH
};

/* 游戏共用的退出菜单变量 (在 menu.c 中定义, menu.h 中 extern 声明) */
uint8_t  g_touch_down;                  /* 触摸按下标志(当前是否按住) */
uint16_t g_touch_start_y;               /* 触摸按下起点 Y */
uint8_t  g_menu_cooldown;               /* 菜单操作冷却计数(防立即重触发) */
uint8_t  g_exit_menu_visible;           /* 退出菜单是否可见 */
uint8_t  g_exit_sliding;                /* 退出菜单滑动动画中 */
uint8_t  g_exit_dragging;               /* 退出菜单拖拽跟手中 */
uint8_t  g_state_before_menu = 0;     /* 进入菜单前的游戏状态 */
int16_t  g_exit_drag_y = -MENU_H;     /* 退出菜单拖拽Y偏移 */
uint16_t g_exit_drag_start_y = 0;     /* 退出菜单拖拽起始触摸Y */
uint8_t  g_restart_menu_visible = 0;   /* 重新开始菜单可见 */
uint8_t  g_restart_sliding = 0;        /* 重新开始菜单滑动中 */
uint8_t  g_restart_dragging = 0;       /* 重新开始菜单拖拽中 */
int16_t  g_restart_slide_y = 240;      /* 重新开始菜单滑动Y位置 */
uint16_t g_menu_clear_color = LCD_BLACK;  /* 菜单/重开区域清除色(游戏可设为背景色) */
uint8_t  g_settings_from_game = MAIN_MODE_VIDEO;  /* 从游戏进入设置的来源模式 */
uint8_t  g_game_enter_exit_menu = 0;  /* 设置菜单中顶部下滑→退出菜单标志 */
uint8_t  g_prev_pressed;              /* 上一帧触摸按下(各游戏共用, 供 game_menu_command/退出菜单轮询) */
uint16_t g_touch_start_x;             /* 触摸起点X (与 g_touch_start_y 一致提升为共享全局) */
uint8_t  g_game_consume_release;      /* 反向滑关闭后的松手吸收标志: 只吸收本次闭合手势的松手, 不误触移动 */


/* 重新开始菜单行类型 (2行: "重新开始" + "返回") */
const uint8_t g_restart_row_types[3] = { MENU_ROW_CHONGXIN, MENU_ROW_SHEZHI, MENU_ROW_FANHUI };

/* 视频上划设置菜单行类型 (5行) */
const uint8_t g_video_menu_row_types[5] = { MENU_ROW_LDSZ, MENU_ROW_XPSJ, MENU_ROW_PMFX, MENU_ROW_YYSH, MENU_ROW_FANHUI };

#define GAME_BATTERY_SAMPLE_US   3000000UL  /* 3秒采样一次，休眠10秒前可完成3次降格确认 */
#define GAME_BATTERY_REDRAW_US   1000000UL
#define GAME_BATTERY_BODY_W            25U
#define GAME_BATTERY_BODY_H            12U
#define GAME_BATTERY_CAP_W              3U

/* 游戏电池图标的缓存状态，避免每帧执行耗时的 ADC 采样 */
static BatteryLevel s_game_battery_level;       /* 当前缓存的电量等级 */
static uint32_t s_game_battery_sample_time;     /* 上次电压采样时间，单位为 us */
static uint32_t s_game_battery_draw_time;       /* 上次图标绘制时间，单位为 us */
static uint16_t s_game_battery_x;               /* 上次绘制的左上角 X 坐标 */
static uint16_t s_game_battery_y;               /* 上次绘制的左上角 Y 坐标 */
static uint8_t s_game_battery_valid;            /* 电量缓存有效标志 */

/* game_menu_drag_poll 私有静态状态 (暴露通过 game_menu_drag_reset 重置) */
static uint8_t  gdp_rel_cnt = 0;    /* 防抖: 连丢2帧才确认释放 */
static int16_t  gdp_start_y = 0;   /* 拖拽起始菜单Y */
static uint16_t gdp_start_ty = 0;  /* 拖拽起始触摸Y */
static uint8_t  gdp_tracking = 0;  /* 已记录起始位置 */
static uint16_t gdp_max_ty = 0;    /* 拖拽中最远触摸Y (用于松手取消判定) */
/* 充电状态 */
static uint8_t  s_charge_flash = 0;         /* 是否处于充电动画(闪烁)状态 */
static uint32_t s_charge_flash_time = 0;    /* 上次动画帧切换时间(us) */
static uint32_t s_charge_anim_frame = 0;    /* 充电动画当前帧(填格数 0~5) */

/**
 * @brief  启用或禁用游戏电池图标的充电显示
 * @param  enable  非零启用充电显示(绿色+充电档位阈值)，0恢复正常电量显示
 * @note   充电动画与休眠充电屏一致：从电压档位n依次填满5格→循环；
 *         BAT_FULL(PB5拉低)时5格常亮。
 */
void game_battery_set_charge_flash(uint8_t enable) {
    s_charge_flash = enable;
    if (enable) {
        s_charge_anim_frame = 0;
        s_charge_flash_time = 0;
    }
}

/**
 * @brief  绘制游戏电池图标 (外壳+电量格)
 * @param  x      左上角X
 * @param  y      左上角Y
 * @param  level  电量等级 BAT_LEVEL_0~5
 * @note   充电时外壳和填充均为绿色；非充电时外壳白/红，填充按电量红/黄/绿
 */
static void game_battery_icon_draw(uint16_t x, uint16_t y,
    BatteryLevel level) {
    uint16_t outline, fill;
    uint8_t i;

    if (s_charge_flash) {
        outline = LCD_GREEN;
        fill = LCD_GREEN;
    }
    else {
        outline = (level == BAT_LEVEL_0) ? LCD_RED : LCD_WHITE;
        if (level <= BAT_LEVEL_1) {
            fill = LCD_RED;
        }
        else if (level == BAT_LEVEL_2) {
            fill = LCD_YELLOW;
        }
        else {
            fill = LCD_GREEN;
        }
    }

    LCD_Fill(x, y,
        x + GAME_BATTERY_BODY_W + GAME_BATTERY_CAP_W,
        y + GAME_BATTERY_BODY_H, LCD_BLACK);
    LCD_Fill(x, y, x + GAME_BATTERY_BODY_W, y + 2, outline);
    LCD_Fill(x, y + GAME_BATTERY_BODY_H - 2,
        x + GAME_BATTERY_BODY_W, y + GAME_BATTERY_BODY_H, outline);
    LCD_Fill(x, y + 2, x + 2, y + GAME_BATTERY_BODY_H - 2, outline);
    LCD_Fill(x + GAME_BATTERY_BODY_W - 2, y + 2,
        x + GAME_BATTERY_BODY_W, y + GAME_BATTERY_BODY_H - 2, outline);
    LCD_Fill(x + GAME_BATTERY_BODY_W, y + 4,
        x + GAME_BATTERY_BODY_W + GAME_BATTERY_CAP_W, y + 8, outline);

    for (i = 0; i < (uint8_t)level; i++) {
        LCD_Fill(x + 4 + (uint16_t)i * 4, y + 3,
            x + 6 + (uint16_t)i * 4, y + 9, fill);
    }
}

/**
 * @brief  清除游戏电池图标的采样和绘制缓存
 * @note   切换游戏后调用，下一次更新会立即重新采样并绘制
 */
void game_battery_icon_reset(void) {
    s_game_battery_valid = 0;
    s_game_battery_sample_time = 0;
    s_game_battery_draw_time = 0;
}

/**
 * @brief  按设定周期采样电压，并按需更新游戏电池图标
 * @param  x             图标左上角 X 坐标
 * @param  y             图标左上角 Y 坐标
 * @param  force_redraw  非零时忽略绘制周期并立即重绘
 * @note   无符号时间差计算可正确处理 32 位微秒计数器回绕
 */
void game_battery_icon_update(uint16_t x, uint16_t y, uint8_t force_redraw) {
    uint32_t now = GetTimerCount();

    /* 充电时每500ms推进一帧动画 */
    if (s_charge_flash
        && (uint32_t)(now - s_charge_flash_time) >= 500000UL) {
        s_charge_anim_frame++;
        s_charge_flash_time = now;
        force_redraw = 1;
    }

    if (!s_game_battery_valid
        || (uint32_t)(now - s_game_battery_sample_time)
        >= GAME_BATTERY_SAMPLE_US) {
        s_game_battery_level =
            Battery_GameVoltageToLevel(BAT_ADC_ReadVoltage());
        now = GetTimerCount();
        s_game_battery_sample_time = now;
        s_game_battery_valid = 1;
        force_redraw = 1;
    }

    if (x != s_game_battery_x || y != s_game_battery_y) {
        s_game_battery_x = x;
        s_game_battery_y = y;
        force_redraw = 1;
    }

    if (force_redraw
        || (uint32_t)(now - s_game_battery_draw_time)
        >= GAME_BATTERY_REDRAW_US) {
        BatteryLevel draw_level;
        if (s_charge_flash) {
            if (Battery_GetStatus() == BAT_FULL) {
                draw_level = BAT_LEVEL_5;
            }
            else {
                /* 充电动画以游戏滤波等级为基值封顶4格，保底cycle_len≥2 */
                uint8_t n = (uint8_t)s_game_battery_level;
                if (n > 4) n = 4;
                uint8_t cycle_len = (n > 0) ? (6 - n) : 6;
                uint8_t lit = n + s_charge_anim_frame % cycle_len;
                draw_level = (lit > 5)
                    ? (BatteryLevel)5 : (BatteryLevel)lit;
            }
        }
        else {
            draw_level = s_game_battery_level;
        }
        game_battery_icon_draw(x, y, draw_level);
        s_game_battery_draw_time = now;
    }
}

/**
 * @brief  获取当前缓存的游戏电量等级
 * @return 当前电量等级 (BAT_LEVEL_0 ~ BAT_LEVEL_4)，
 *         缓存无效时返回 BAT_LEVEL_4
 */
BatteryLevel game_battery_icon_get_level(void) {
    if (!s_game_battery_valid) {
        s_game_battery_level =
            Battery_GameVoltageToLevel(BAT_ADC_ReadVoltage());
        s_game_battery_sample_time = GetTimerCount();
        s_game_battery_valid = 1;
    }
    return s_game_battery_level;
}

/**
 * @brief  使用xorshift32生成游戏所需的轻量伪随机数
 * @return 生成的32位伪随机数
 */
uint32_t game_rand(void) {
    static uint32_t state = 0xA341316CUL;

    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/**
 * @brief  绘制华容道图标 (4x4网格, 右下角留空表示空格)
 */
void menu_draw_klotski_icon(uint16_t y) {
    uint8_t cell = 6, gap = 1;
    uint8_t gs = cell + gap;
    uint16_t iw = 4 * cell + 3 * gap;  /* 27px */
    uint16_t ix = MENU_X + (MENU_W - iw) / 2;
    uint16_t iy = y + (32 - iw) / 2;
    uint8_t r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            if (r == 3 && c == 3) continue;
            LCD_Fill(ix + c * gs, iy + r * gs,
                ix + c * gs + cell - 1, iy + r * gs + cell - 1,
                MENU_TEXT_COLOR);
        }
    }
}

/**
 * @brief  绘制俄罗斯方块 T 形图标
 */
void menu_draw_tetris_icon(uint16_t y) {
    uint16_t cell = 14, g2 = 2;
    uint16_t iw = 3 * cell;
    uint16_t ix = MENU_X + (MENU_W - iw) / 2;
    uint16_t iy = y + (32 - 2 * cell) / 2;
    uint16_t x0 = ix, x1 = ix + cell, x2 = ix + 2 * cell;
    uint16_t y0 = iy, y1 = iy + cell;
    LCD_Fill(x1 + g2, y0 + g2, x1 + cell - g2, y0 + cell - g2, MENU_TEXT_COLOR);
    LCD_Fill(x0 + g2, y1 + g2, x0 + cell - g2, y1 + cell - g2, MENU_TEXT_COLOR);
    LCD_Fill(x1 + g2, y1 + g2, x1 + cell - g2, y1 + cell - g2, MENU_TEXT_COLOR);
    LCD_Fill(x2 + g2, y1 + g2, x2 + cell - g2, y1 + cell - g2, MENU_TEXT_COLOR);
}

/**
 * @brief  绘制大鱼吃小鱼图标 (中文"鱼"字, 24x24)
 * @note   直接使用固件内字模 font_yu 绘制, 不经外部Flash(免重新烧录)
 */
void menu_draw_fish_icon(uint16_t y) {
    uint16_t iw = 24;  /* 字宽 */
    uint16_t ih = 24;  /* 字高 */
    uint16_t ix = MENU_X + (MENU_W - iw) / 2;
    uint16_t iy = y + (32 - ih) / 2;
    sok_draw_char_24(ix, iy, font_yu, MENU_TEXT_COLOR);
}



/**
 * @brief  绘制菜单外框 (边框 + 纯色背景)
 */
void menu_draw_frame(void) {
    LCD_Fill(MENU_X, MENU_Y, MENU_X + MENU_W - 1, MENU_Y + MENU_BDR - 1, MENU_BORDER_COLOR);
    LCD_Fill(MENU_X, MENU_Y + MENU_H - MENU_BDR, MENU_X + MENU_W - 1, MENU_Y + MENU_H - 1, MENU_BORDER_COLOR);
    LCD_Fill(MENU_X, MENU_Y, MENU_X + MENU_BDR - 1, MENU_Y + MENU_H - 1, MENU_BORDER_COLOR);
    LCD_Fill(MENU_X + MENU_W - MENU_BDR, MENU_Y, MENU_X + MENU_W - 1, MENU_Y + MENU_H - 1, MENU_BORDER_COLOR);
    LCD_Fill(MENU_X + MENU_BDR, MENU_Y + MENU_BDR,
        MENU_X + MENU_W - MENU_BDR - 1, MENU_Y + MENU_H - MENU_BDR - 1, MENU_BG_COLOR);
}

/**
 * @brief  按"视觉行中线"把触摸 Y 归属到菜单某一行 (触摸区无缝隙, 显示保留行间距)
 * @param  y_top   菜单框顶部Y
 * @param  y       触摸Y
 * @param  n_rows  行数 (2~6)
 * @return 行索引 (0-based); -1 = 点在菜单框上方(视为点空白)
 * @note   行几何与 menu_draw_nrow_frame_only 用同一公式: row_gap=(mh-2*mb-n*32)/(n+1),
 *         第 i 行文字顶 = y_top + mb + gap + i*(32+gap)。触摸区分界取相邻两行文字之间
 *         那段间隙的中线(即 base = mb + gap/2, 步距 = 32+gap), 所以:
 *           1) 触摸区连续无缝隙, 点在两行之间一定归属最近的某一行, 不会掉空;
 *           2) 分界线与"眼睛看到的行中心"对齐(旧写法按内高平均分, 5行时最后一条分界线
 *              比视觉中线低 12px → 点"返回"上半截会被算成上一行/点倒数第二行下沿被算成
 *              下一行, 这就是"点设置却跳到返回"的根因);
 *           3) 框内上下边框带、以及框下沿到屏幕最下(y=239)都夹到最近行。
 */
int8_t menu_row_from_y(uint16_t y_top, uint16_t y, uint8_t n_rows) {
    uint16_t mh, mb, gap, base, rel, idx;

    if (n_rows < 2 || n_rows > 6) return -1;
    if (n_rows == 3)      { mh = VIDEO_MENU_H;  mb = VIDEO_MENU_BDR; }  /* 游戏"重开/设置/返回" */
    else if (n_rows == 4) { mh = VIDEO_MENU_H4; mb = VIDEO_MENU_BDR; }
    else if (n_rows == 2) { mh = RESTART_MENU_H; mb = RESTART_MENU_BDR; }
    else                  { mh = MENU_H;        mb = MENU_BDR; }        /* 5/6 行 */

    if (y < y_top) return -1;   /* 菜单框上方才算未命中(点空白关闭) */

    gap  = (uint16_t)((mh - 2 * mb - (uint16_t)n_rows * 32) / (n_rows + 1));
    base = (uint16_t)(mb + gap / 2);
    rel  = (uint16_t)(y - y_top);
    if (rel <= base) return 0;
    idx = (uint16_t)((rel - base - 1u) / (uint16_t)(32u + gap));
    if (idx >= n_rows) return (int8_t)(n_rows - 1);
    return (int8_t)idx;
}

/**
 * @brief  菜单选项碰撞检测
 * @param  x, y        触摸坐标
 * @param  item_count  菜单项数量 (5行主菜单/退出菜单)
 * @return 被点击的菜单项索引 (0-based), -1 表示点在菜单框上方或左右外侧
 */
int8_t menu_hit_test(uint16_t x, uint16_t y, uint8_t item_count) {
    if (x < MENU_X || x >= MENU_X + MENU_W) return -1;
    return menu_row_from_y(MENU_Y, y, item_count);
}

#if TOUCH_DEBUG_SHOW
/* debug overlay: menu hit-box + row dividers (green) and touch cross (magenta/yellow).
   Wait for the pending menu DMA first: menu text is pushed by SPI2_Write_DMA which
   returns before the transfer finishes, so without waiting it would paint OVER the cross. */
void Menu_DebugDrawRegions(void)
{
    TouchState ts;
    uint16_t xl = MENU_X;
    uint16_t yt = MENU_Y;
    uint16_t xr = (uint16_t)(MENU_X + MENU_W);
    uint16_t yb = (uint16_t)(MENU_Y + MENU_H);
    uint16_t gap  = (uint16_t)((MENU_H - 2 * MENU_BDR - 5u * 32u) / 6u);
    uint16_t base = (uint16_t)(MENU_Y + MENU_BDR + gap / 2);
    uint8_t  k;

    Touch_Scan(&ts);
    SPI2_WaitIdle();

    LCD_CS_CLR;
    LCD_Fill(xl, yt, xr, yt + 1, LCD_GREEN);
    LCD_Fill(xl, yb - 1, xr, yb, LCD_GREEN);
    LCD_Fill(xl, yt, xl + 1, yb, LCD_GREEN);
    LCD_Fill(xr - 1, yt, xr, yb, LCD_GREEN);
    for (k = 1; k < 5; k++) {
        uint16_t yy = (uint16_t)(MENU_Y + base + 1u + (uint16_t)k * (32u + gap));
        if (yy > yt && yy < yb) LCD_Fill(xl, yy, xr, yy + 1, LCD_GREEN);
    }
    {
        uint16_t col = ts.pressed ? LCD_MAGENTA : LCD_YELLOW;
        uint16_t px = ts.x;
        uint16_t py = ts.y;
        uint16_t hx0 = (px > 12) ? (uint16_t)(px - 12) : 0;
        uint16_t hx1 = (px + 13u > 240u) ? 240u : (uint16_t)(px + 13u);
        uint16_t hy0 = (py > 12) ? (uint16_t)(py - 12) : 0;
        uint16_t hy1 = (py + 13u > 240u) ? 240u : (uint16_t)(py + 13u);
        uint16_t cx0 = (px > 1) ? (uint16_t)(px - 1) : 0;
        uint16_t cx1 = (px + 2u > 240u) ? 240u : (uint16_t)(px + 2u);
        uint16_t cy0 = (py > 1) ? (uint16_t)(py - 1) : 0;
        uint16_t cy1 = (py + 2u > 240u) ? 240u : (uint16_t)(py + 2u);
        LCD_Fill(hx0, cy0, hx1, cy1, col);
        LCD_Fill(cx0, hy0, cx1, hy1, col);
    }
    LCD_CS_SET;
}
#endif

/**
 * @brief  加载菜单字体缓存(从外部 Flash 按需载入字库块)。
 */
/**
 * @brief  让菜单字体缓存失效: 下次绘制时按当前语言重新加载。
 * @note   设置页切换语言后必须调用, 否则菜单还在用上一套字模(SW缓存是二选一的)。
 */
void Menu_FontCacheInvalidate(void) {
    s_menu_fonts_ok = 0;
    s_fonts_lang    = 0xFF;
}

static void menu_font_cache_load(void) {
    uint8_t i;
    uint8_t want = (uint8_t)(g_language_index ? 1u : 0u);   /* 0=中文, 1=英文 */
    if (s_menu_fonts_ok && s_fonts_lang == want) return;

    if (want == 1) {
        /* 英文: 一次读入整组 27 个 16x24 半宽字(27*48=1296B, 缓冲 1728B 够用) */
        s_menu_fonts_ok = 0;
        if (FlashStore_ResRead(FLASH_STORE_ID_LATIN24, 0, s_menu_fonts,
                LATIN24_COUNT * LATIN24_CHAR) == LATIN24_COUNT * LATIN24_CHAR) {
            s_menu_fonts_ok = 1;
            s_fonts_lang    = 1;
        }
    } else {
        for (i = 0; i < 22; i++) {
            if (FlashStore_ResRead((FlashStoreId)i, 0, s_menu_fonts + (uint16_t)i * 72, 72) != 72) {
                break;   /* 任一读取失败即放弃缓存(外部Flash未烧录) */
            }
        }
        if (i == 22) {
            /* 后加的 语/言(资源ID 115/116) 存到槽位 22/23; 读不到(旧字库)只影响"语言设置"行显示 */
            uint8_t k;
            for (k = 0; k < 2; k++) {
                uint8_t slot = (uint8_t)(22u + k);
                FlashStore_ResRead((FlashStoreId)(FLASH_STORE_ID_24_YU2 + k), 0,
                    s_menu_fonts + (uint16_t)slot * 72, 72);
            }
            /* 游戏名追加的 10 个字(方块消行/华容道/大鱼小鱼): 字模不在资源表里, 存在
               外部Flash的"字库数据区紧前面"那一小块(与字库同一个区, 但不在字库擦除
               范围内), 由 BTN_IMG_STORE_MODE 单独烧一次; 没烧这块只是这 3 行不显示字 */
            FlashStore_MenuGlyphRead(s_menu_fonts + (uint16_t)MENU_EXT_SLOT * 72);
            s_menu_fonts_ok = 1;
            s_fonts_lang    = 0;
        }
    }
}

/**
 * @brief  绘制主菜单(5行: 视频/各游戏), 用 n 行框架统一绘制。
 */
void main_menu_draw(void) {
    /* 统一走条带渲染: 边框+分隔线+各图标/文字, 与 main_menu_draw_frame_only 布局一致 */
    menu_draw_nrow_frame_only(MENU_Y, g_main_menu_row_types, 5);
}

/**
 * @brief  主菜单动画 (5行: 推箱子/2048/俄罗斯方块/华容道/大鱼吃小鱼)
 */
void main_menu_draw_frame_only(int16_t y_top) {
    menu_draw_nrow_frame_only(y_top, g_main_menu_row_types, 5);
}

/**
 * @brief  把低电电池图标像素写进菜单条带缓冲(与菜单一次 DMA 一起绘制)。
 * @param  row 条带缓冲
 * @param  ay  条带所在屏幕 Y
 * @param  mx  菜单左缘 X
 * @param  mw  菜单宽度
 * @note   仅图标亮段(g_lowbatt_showing)且条带位于电池区时叠加, 避免拖拽时菜单与电池来回闪。
 */
static void lmbat_strip(uint8_t* row, int16_t ay, int16_t mx, uint16_t mw) {
    int16_t bx = 45, by = 79, bw = 150, bh = 82;           /* 电池主体(2px边): x=45~195, y=79~161 */
    int16_t ex0 = 111, ex1 = 129;                       /* 感叹号竖条 x=111~129 */
    int16_t ey0 = 92, ey1 = 128, edy0 = 137, edy1 = 155;  /* 感叹号竖条 y=92~128, 点 y=137~155 */
    int16_t a, b, x, rx;
    /* 电池框边(2px) */
    if ((ay >= by && ay < by + 2) || (ay >= by + bh - 2 && ay < by + bh)) {
        a = bx; b = bx + bw;
        for (x = a; x < b; x++) {
            rx = x - mx;
            if (rx >= 0 && rx < (int16_t)mw) { row[rx * 2] = 0xF8; row[rx * 2 + 1] = 0x00; }
        }
    }
    else if (ay >= by && ay < by + bh) {
        a = bx; b = bx + 2;
        for (x = a; x < b; x++) { rx = x - mx; if (rx >= 0 && rx < (int16_t)mw) { row[rx * 2] = 0xF8; row[rx * 2 + 1] = 0x00; } }
        a = bx + bw - 2; b = bx + bw;
        for (x = a; x < b; x++) { rx = x - mx; if (rx >= 0 && rx < (int16_t)mw) { row[rx * 2] = 0xF8; row[rx * 2 + 1] = 0x00; } }
    }
    /* 感叹号 */
    if ((ay >= ey0 && ay <= ey1) || (ay >= edy0 && ay <= edy1)) {
        a = ex0; b = ex1;
        for (x = a; x < b; x++) {
            rx = x - mx;
            if (rx >= 0 && rx < (int16_t)mw) { row[rx * 2] = 0xF8; row[rx * 2 + 1] = 0x00; }
        }
    }
}

/**
 * @brief 画一行英文菜单文字(半宽字, 逐字从 RAM 缓存的整组拉丁字里取)
 * @param row  条带缓冲(每像素2字节)
 * @param mw   菜单宽度
 * @param cr   行内像素行(0..23, 已去掉上下留白)
 * @param str  ASCII 串
 * @param hi/lo 文字颜色高低字节
 * @note  供 menu_draw_nrow_frame_only 的文字行 与"鱼"那行(英文模式)复用
 */
static void menu_blit_en_row(uint8_t* row, uint16_t mw, uint16_t cr,
                             const char* str, uint8_t hi, uint8_t lo)
{
    uint16_t cx, x;
    uint8_t ci, slen = 0;
    if (!str) return;
    while (str[slen]) slen++;
    cx = (uint16_t)((mw - (uint16_t)slen * LATIN24_W) / 2);
    for (ci = 0; ci < slen; ci++) {
        uint8_t gi = g_latin24_idx[(uint8_t)str[ci]];
        if (gi != LATIN_INDEX_NONE) {
            const uint8_t* fp = &s_menu_fonts[((uint16_t)gi * 24u + cr) * 2u];
            /* 字模只有 14 列宽: 第 1 字节覆盖列 0~7, 第 2 字节覆盖列 8~15 但低 2 位恒 0。
               两级省循环: ① 整字节为 0 直接跳过(空格、窄字母 i/l 常整字节空, 原来白跑 8 次);
               ② 第 2 字节只扫高 6 位。英文菜单一行 33 个字符, 每字符原本固定 16 次位判断 */
            for (uint8_t b = 0; b < 2; b++) {
                uint8_t bits = fp[b];
                if (!bits) continue;
                int8_t bt_lo = b ? 2 : 0;   /* b=1 时 bit1/bit0 对应列 14/15, 恒 0 */
                for (int8_t bt = 7; bt >= bt_lo; bt--) {
                    if (bits & (1 << bt)) {
                        x = (uint16_t)(cx + b * 8 + (7 - (uint8_t)bt));
                        if (x < mw) { row[x * 2] = hi; row[x * 2 + 1] = lo; }
                    }
                }
            }
        }
        cx += LATIN24_W;
    }
}

/**
 * @brief  绘制 n 行菜单框架(逐条带乒乓双缓冲, 一次 DMA 发送)。
 * @param  y_top     菜单框顶 Y
 * @param  row_types 各行人类型数组
 * @param  n_rows    行数
 * @note   条带复用 shared_buffer 前后两半, 不额外占 RAM; 低电亮段时电池区条带叠加图标。
 */
void menu_draw_nrow_frame_only(int16_t y_top, const uint8_t* row_types,
    uint8_t n_rows) {
    /* 条带高度: MENU_W=150 → 行宽300B, 每条带8行=2400B,
       双缓冲分块(shared_buffer 5120 = 前置2400 + 后置2400), 0额外RAM恢复流水线并行 */
#define STRIP_H   8

#define TXT_SPAN  96  /* 文本边框内容区宽度(中文): 4*24 = 3*32 = 96 */
#define TXT_SPAN_EN 112 /* 英文: 最长单词 8 字 x 14(LATIN24_W) = 112, 框按语言放宽 */

       /* 根据行数选择菜单尺寸 */
    uint16_t mw, mh, mx, mb;
    if (n_rows == 2) {
        mw = RESTART_MENU_W; mh = RESTART_MENU_H;
        mx = RESTART_MENU_X; mb = RESTART_MENU_BDR;
    }
    else if (n_rows == 3) {
        mw = VIDEO_MENU_W; mh = VIDEO_MENU_H;
        mx = VIDEO_MENU_X; mb = VIDEO_MENU_BDR;
    }
    else if (n_rows == 4) {
        mw = VIDEO_MENU_W; mh = VIDEO_MENU_H4;
        mx = VIDEO_MENU_X; mb = VIDEO_MENU_BDR;
    }
    else {
        mw = MENU_W; mh = MENU_H;
        mx = MENU_X; mb = MENU_BDR;
    }

#define ROW_BYTES ((uint16_t)mw * 2)

    /* 颜色常量 */
#define BK_H 0x00
#define BK_L 0x00
#define GN_H 0x07
#define GN_L 0xE0
#define CY_H 0x7F
#define CY_L 0xFF
#define BL_H 0x00
#define BL_L 0x1F
#define TX_H 0xF8
#define TX_L 0x1F

/* 计算行Y坐标: row_y[i] = mb + gap + i*(32+gap) */
    uint8_t  nr = n_rows;
    uint16_t row_gap = (uint16_t)((mh - 2 * mb - nr * 32) / (nr + 1));
    uint16_t row_y[6];  /* 最大6行 */
    uint16_t i;
    for (i = 0; i < nr; i++)
        row_y[i] = (uint16_t)(mb + row_gap + i * (32 + row_gap));

    /* 文本边框水平范围 (英文单词比中文宽: 8字x14=112, 所以英文时把框放宽) */
    uint16_t txt_span = (uint16_t)((g_language_index == 1) ? TXT_SPAN_EN : TXT_SPAN);
    uint16_t txt_l = (mw - txt_span) / 2 - MENU_TEXT_BDR_PAD;
    uint16_t txt_r = (uint16_t)(txt_l + txt_span + 2 * MENU_TEXT_BDR_PAD - 1);
    uint16_t blu_l = (uint16_t)(txt_l + MENU_TEXT_BDR_W);
    uint16_t blu_r = (uint16_t)(txt_r - MENU_TEXT_BDR_W);

    /* 文本边框垂直范围 */
    int16_t txt_y_top = (int16_t)(row_y[0] - MENU_TEXT_BDR_PAD);
    int16_t txt_y_bot = (int16_t)(row_y[nr - 1] + 31 + MENU_TEXT_BDR_PAD);
    int16_t bhw = (int16_t)MENU_TEXT_BDR_W;

    /* 分隔线Y */
    int16_t sep_y[5];  /* 最大5条分隔线 */
    for (i = 0; i < nr - 1; i++)
        sep_y[i] = (int16_t)((row_y[i] + 31 + row_y[i + 1]) / 2);

    int16_t y0 = y_top, y1 = y_top + (int16_t)mh;
    if (y1 <= 0 || y0 >= 240) return;
    if (y0 < 0) y0 = 0;
    if (y1 > 240) y1 = 240;

    /* 乒乓双缓冲: 复用 shared_buffer 前后两半 (0额外RAM恢复画/发流水线并行)
       STRIP_H=8 × ROW_BYTES(mw*2≤300)=2400B, 两块共4800B ≤ FILL_BUFFER_SIZE(5120) */
#define STRIP_BYTES ((uint16_t)(STRIP_H * ROW_BYTES))
    u8* strip_buf[2];
    strip_buf[0] = shared_buffer;
    strip_buf[1] = shared_buffer + STRIP_BYTES;
    uint8_t active = 0;
    uint8_t first = 1;

    menu_font_cache_load();  /* 确保菜单字库缓存已从外部Flash加载 */

    LCD_CS_CLR;
    LCD_Address_Set(mx, (uint16_t)y0,
        mx + mw - 1, (uint16_t)(y1 - 1));
    while (SPI2_DMA_TX_CH->CNTR > 0);
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
    LCD_DC_SET;

    for (int16_t sy = 0; sy < (y1 - y0); sy += STRIP_H) {
        int16_t h = STRIP_H;
        if ((int16_t)(sy + h) > (y1 - y0)) h = (int16_t)(y1 - y0 - sy);
        int16_t abs_y0 = y0 + sy;
        uint16_t bytes = (uint16_t)h * ROW_BYTES;
        uint8_t* buf = strip_buf[active];

        FastFill_16bit_Repeat(buf, bytes, BK_H, BK_L);

        for (int16_t r = 0; r < h; r++) {
            int16_t rel_y = (abs_y0 + r) - y_top;
            uint8_t* row = buf + r * ROW_BYTES;
            uint16_t x;

            if (rel_y < 0 || rel_y >= (int16_t)mh) continue;

            /* ---- 绿色顶/底整行边框 ---- */
            if (rel_y < (int16_t)mb || rel_y >= (int16_t)(mh - mb)) {
                for (x = 0; x < mw; x++) {
                    row[x * 2] = GN_H; row[x * 2 + 1] = GN_L;
                }
            }
            else {
                /* ---- 内饰行: 绿竖框 + 青底 ---- */
                for (x = 0; x < mb; x++) {
                    row[x * 2] = GN_H; row[x * 2 + 1] = GN_L;
                }
                for (x = mw - mb; x < mw; x++) {
                    row[x * 2] = GN_H; row[x * 2 + 1] = GN_L;
                }
                for (x = mb; x < mw - mb; x++) {
                    row[x * 2] = CY_H; row[x * 2 + 1] = CY_L;
                }
            }

            /* ---- 蓝色文字边框 (覆盖所有行) ---- */
            if (rel_y >= txt_y_top && rel_y <= txt_y_bot) {
                uint8_t is_hline = 0;

                if ((rel_y >= txt_y_top && rel_y < txt_y_top + bhw) ||
                    (rel_y > txt_y_bot - bhw && rel_y <= txt_y_bot))
                    is_hline = 1;
                else {
                    for (i = 0; i < nr - 1; i++) {
                        if (rel_y >= sep_y[i] - bhw / 2 &&
                            rel_y < sep_y[i] + (bhw + 1) / 2) {
                            is_hline = 1;
                            break;
                        }
                    }
                }

                if (is_hline) {
                    for (x = txt_l; x <= txt_r; x++) {
                        row[x * 2] = BL_H; row[x * 2 + 1] = BL_L;
                    }
                }
                else {
                    /* 文字区的青色底不用在这里重填: 上面"内饰行"分支已经把 mb..mw-mb 整行刷成青色,
                       而文字框 txt_l..txt_r 一定落在该范围内
                       (txt_l = (mw-txt_span)/2 - PAD, txt_r = txt_l + txt_span + 2*PAD - 1;
                        各尺寸菜单都是 mw=150 / mb=5, 且 txt_y_top~txt_y_bot 也都在内饰行里)。
                       原来"先整片填青、再拿蓝框覆盖一遍"等于白写一遍文字区宽度, 每帧要跑
                       约 190 行 × (txt_span+20) 次; 而英文的 txt_span(112) 比中文(96) 宽,
                       白写的量还更多 —— 这就是"英文菜单比中文慢一点"的那点差距来源。
                       这里只画左右两条蓝竖框。 */
                    for (x = txt_l; x < blu_l; x++) {
                        row[x * 2] = BL_H; row[x * 2 + 1] = BL_L;
                    }
                    for (x = blu_r + 1; x <= txt_r; x++) {
                        row[x * 2] = BL_H; row[x * 2 + 1] = BL_L;
                    }
                }
            }

            /* ---- 逐行渲染文字/图标 ---- */
            for (i = 0; i < nr; i++) {
                if (rel_y < (int16_t)row_y[i] ||
                    rel_y >(int16_t)(row_y[i] + 31)) continue;

                uint8_t cr = (uint8_t)(rel_y - row_y[i]);
                uint16_t cx;
                uint8_t ci, cl, co;

                switch (row_types[i]) {
                case MENU_ROW_2048:        /* 2048行: 中文"方块合并" / 英文"Merge" */
                case MENU_ROW_CHONGXIN:
                case MENU_ROW_LDSZ:
                case MENU_ROW_XPSJ:
                case MENU_ROW_PMFX:
                case MENU_ROW_YYSH:
                case MENU_ROW_TUI:
                case MENU_ROW_SHIPIN:
                case MENU_ROW_SHEZHI:
                case MENU_ROW_FANHUI:
                case MENU_ROW_TETRIS:      /* 这几个行(俄罗斯方块/华容道/鱼)改成中英文字, */
                case MENU_ROW_KLOTSKI:     /* 不再画图标: 中文用追加字模(外部保留区), */
                case MENU_ROW_FISH:        /* 英文用半宽拉丁字, 与 Sokoban/Video 一致 */
                case MENU_ROW_CHEXIAO: {
                    const MenuRowText* rt = (row_types[i] <= MENU_ROW_MAX)
                        ? &s_row_text[row_types[i]] : 0;
                    if (!rt) break;
                    if (cr < 4 || cr >= 28) break;   /* 24 高的字模画在 32 高行槽的中部 */
                    cr -= 4;

                    if (g_language_index == 1) {
                        /* 英文行: 半宽字, 每字占 LATIN24_W 列 (空格字模全零, 自然留白) */
                        if (!rt->en) break;
                        menu_blit_en_row(row, mw, cr, rt->en, TX_H, TX_L);
                    } else {
                        /* 中文行: 24 宽全宽字 */
                        const uint8_t* fnt = rt->cn;
                        uint8_t cnt = rt->cn_cnt;
                        if (!fnt) break;
                        cx = (mw - cnt * 24) / 2;
                        cl = cr >> 2; co = (cr & 3) * 3;
                        for (ci = 0; ci < cnt; ci++) {
                            const uint8_t* fp = &s_menu_fonts[(uint16_t)MENU_FONT_SLOT(fnt[ci]) * 72 + cl * 12 + co];
                            for (uint8_t b = 0; b < 3; b++) {
                                uint8_t bits = fp[b];
                                for (int8_t bt = 7; bt >= 0; bt--) {
                                    if (bits & (1 << bt)) {
                                        x = cx + b * 8 + (7 - (uint8_t)bt);
                                        if (x < mw) { row[x * 2] = TX_H; row[x * 2 + 1] = TX_L; }
                                    }
                                }
                            }
                            cx += 24;
                        }
                    }
                } break;
                }
            }
            /* 低电图标显示(亮段)时, 把电池图标该行像素写进 buf(与菜单一起绘制) */
            if (g_lowbatt_showing)
                lmbat_strip(row, (int16_t)(abs_y0 + r), mx, mw);
        }

        __asm__ volatile("" ::: "memory");
        if (!first) {
            /* 发送前等上一次(另一块) DMA 完毕, 再复用该块 */
            while (SPI2_DMA_TX_CH->CNTR > 0);
            while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
        }
        SPI2_Write_DMA(buf, bytes);
        active ^= 1;
        first = 0;
    }

    while (SPI2_DMA_TX_CH->CNTR > 0);
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
    LCD_CS_SET;

#undef STRIP_H
#undef STRIP_BYTES
#undef TXT_SPAN_EN
#undef TXT_SPAN
#undef ROW_BYTES
#undef BK_H
#undef BK_L
#undef GN_H
#undef GN_L
#undef CY_H
#undef CY_L
#undef BL_H
#undef BL_L
#undef TX_H
#undef TX_L
}

/**
 * @brief  游戏菜单中点击"设置"按钮, 打开视频设置菜单
 */
void menu_enter_game_settings(void) {
    g_settings_from_game = 1;
    s_video_menu_visible = 1;
    s_video_menu_slide_y = VIDEO_MENU_Y5;
    vm_touch_down = 0;
}


/**
 * @brief  重置 game_menu_drag_poll 内部状态, 确保下次调用从干净状态启动
 */
void game_menu_drag_reset(void) {
    gdp_rel_cnt = 0;
    gdp_start_y = 0;
    gdp_start_ty = 0;
    gdp_tracking = 0;
    gdp_max_ty = 0;
}

/**
 * @brief  游戏退出菜单跟手拖拽轮询 (4个游戏共用)
 *         单向: 只跟手下拉, 不跟手上移; 松手上划>10px返回2取消
 * @return 0=拖拽中, 1=已释放-提交, 2=已释放-取消
 */
uint8_t game_menu_drag_poll(int16_t* drag_y, uint16_t* drag_touch_y,
    uint8_t* dragging, const TouchState* ts,
    const uint8_t row_types[5]) {
    if (ts->pressed) {
        gdp_rel_cnt = 0;
        if (!gdp_tracking) {
            gdp_start_y = *drag_y;
            gdp_start_ty = ts->y;
            gdp_max_ty = ts->y;
            gdp_tracking = 1;
        }
        /* 根据手指总位移算目标，逐帧追过去 */
        int16_t total_dy = (int16_t)ts->y - (int16_t)gdp_start_ty;
        int16_t target_y = gdp_start_y + total_dy;
        /* 拖动上限要留出 MENU_RELEASE_GAP, 不能改成 MENU_Y(试过, 已回退):
           ① 留一段余量, 松手后由自动滑动补完, 用于区分"拖到一半松手取消";
           ② 更关键: 若真拖到底, 手指会跟到屏幕下方(>220), 而"上划重新开始菜单"的
              起手判定区就在那里 → 会意外弹出上划菜单。 */
        if (target_y > MENU_Y - MENU_RELEASE_GAP)
            target_y = MENU_Y - MENU_RELEASE_GAP;
        if (target_y > *drag_y) {
            int16_t diff = target_y - *drag_y;
            /* 只做单帧位移上限: 菜单拖起来是"逐像素慢慢追上来"的从容感。
               曾加过"最大滞后"(超出部分直接补上, 让快滑更跟手), 但菜单观感变差,
               已按需回退(鱼拖动/亮度条仍保留该做法, 见 §24)。 */
            if (diff > MENU_DRAG_MAX_STEP)
                diff = MENU_DRAG_MAX_STEP;
            *drag_y += diff;
        }
        if (ts->y > gdp_max_ty) gdp_max_ty = ts->y;
        *drag_touch_y = ts->y;
    }
    else {
        gdp_rel_cnt++;
        if (gdp_rel_cnt >= 2) gdp_tracking = 0;  /* 释放后重置起始记录 */
    }
    menu_draw_nrow_frame_only(*drag_y, row_types, 5);
    if (!ts->pressed && gdp_rel_cnt >= 2) {
        *dragging = 0;
        gdp_rel_cnt = 0;
        if ((int16_t)gdp_max_ty - (int16_t)ts->y > 10)
            return 2;
        return 1;
    }
    return 0;
}

/**
 * @brief  重新开始菜单跟手上划轮询
 *         单向: 只跟手上划, 不跟手下移
 *         松手时根据回退距离判断: max_dy - cur_dy > 5 → 取消, 否则提交
 *         (与视频菜单 video_menu_drag_poll 逻辑一致)
 * @return 0=拖拽中, 1=已释放-提交, 2=已释放-取消
 */
uint8_t game_restart_menu_drag_poll(int16_t* slide_y, uint16_t drag_start_y,
    uint8_t* dragging, const TouchState* ts) {
    static uint8_t rel_cnt = 0;
    static int16_t max_dy = 0;

    if (ts->pressed) {
        int16_t dy = (int16_t)drag_start_y - (int16_t)ts->y;
        int16_t new_y = RESTART_MENU_Y + RESTART_MENU_H - dy;
        rel_cnt = 0;
        if (dy > max_dy) max_dy = dy;
        if (new_y < RESTART_MENU_Y) new_y = RESTART_MENU_Y;
        if (new_y > RESTART_MENU_Y + RESTART_MENU_H)
            new_y = RESTART_MENU_Y + RESTART_MENU_H;
        if (new_y < *slide_y - RESTART_MENU_DRAG_MAX_STEP)
            new_y = *slide_y - RESTART_MENU_DRAG_MAX_STEP;
        if (new_y <= *slide_y)
            *slide_y = new_y;
    }
    else {
        rel_cnt++;
    }

    menu_draw_nrow_frame_only(*slide_y, g_restart_row_types, 3);
    if (!ts->pressed && rel_cnt >= 2) {
        *dragging = 0;
        rel_cnt = 0;
        int16_t cur_dy = (int16_t)drag_start_y - (int16_t)ts->y;
        int16_t ret = (max_dy - cur_dy > 5) ? 2 : 1;
        max_dy = 0;
        return ret;
    }
    return 0;
}

/**
 * @brief  检测重新开始菜单中的顶部下滑手势
 * @return 1 表示检测到有效下滑，0 表示尚未触发
 */
uint8_t game_restart_menu_down_poll(const TouchState* ts, uint8_t* touch_down,
    uint16_t* start_x, uint16_t* start_y) {
    if (!ts->pressed) {
        *touch_down = 0;
        return 0;
    }
    if (!*touch_down) {
        *touch_down = 1;
        *start_x = ts->x;
        *start_y = ts->y;
    }
    return *start_y < MENU_SWIPE_START_Y && (int16_t)ts->y - (int16_t)*start_y > 5;
}

/**
 * @brief  视频设置菜单拖拽跟随轮询。
 * @param  slide_y       滑动位置
 * @param              drag_start_y 拖拽起点 Y
 * @param  dragging      拖拽中标志
 * @param  ts            触摸状态
 */
uint8_t video_menu_drag_poll(int16_t* slide_y, uint16_t drag_start_y,
    uint8_t* dragging, const TouchState* ts) {
    static uint8_t rel_cnt = 0;
    static int16_t max_dy = 0;

    if (ts->pressed) {
        int16_t dy = drag_start_y - ts->y;
        int16_t new_y = (int16_t)(VIDEO_MENU_Y5 + VIDEO_MENU_H5) - dy;
        rel_cnt = 0;
        if (dy > max_dy) max_dy = dy;
        if (new_y > VIDEO_MENU_Y5 + VIDEO_MENU_H5)
            new_y = VIDEO_MENU_Y5 + VIDEO_MENU_H5;
        if (new_y < VIDEO_MENU_Y5) new_y = VIDEO_MENU_Y5;
        if (new_y < *slide_y - VIDEO_MENU_DRAG_MAX_STEP)
            new_y = *slide_y - VIDEO_MENU_DRAG_MAX_STEP;
        if (new_y <= *slide_y) *slide_y = new_y;
    }
    else {
        rel_cnt++;
    }

    menu_draw_nrow_frame_only(*slide_y, g_video_menu_row_types, 5);
    if (!ts->pressed && rel_cnt >= 2) {
        *dragging = 0;
        rel_cnt = 0;
        int16_t cur_dy = drag_start_y - ts->y;
        int16_t ret = (max_dy - cur_dy > 5) ? 2 : 1;
        max_dy = 0;
        return ret;
    }
    return 0;
}

/**
 * @brief  视频设置菜单上划触发轮询。
 * @param  ts         触摸状态
 * @param  touch_down 按下标志
 * @param  start_x    触摸起点 X
 * @param  start_y    触摸起点 Y
 */
uint8_t video_menu_swipe_up_poll(const TouchState* ts, uint8_t* touch_down,
    uint16_t* start_x, uint16_t* start_y) {
    if (!ts->pressed) {
        *touch_down = 0;
        return 0;
    }
    if (!*touch_down) {
        *touch_down = 1;
        *start_x = ts->x;
        *start_y = ts->y;
    }
    return *start_y > VIDEO_SWIPE_START_Y
        && (int16_t)*start_y - (int16_t)ts->y > VIDEO_SWIPE_TRIGGER;
}

/**
 * @brief  主菜单触摸处理 + 模式切换
 *         封装 hit检测 + LCD清屏 + 模式切换 + 返回视频
 * @param  x, y        触摸坐标
 * @param  mode        s_main_mode 指针
 * @param  menu_drawn  s_menu_drawn 指针
 * @param  game_inited s_game_inited 指针
 * @param  touch_down  s_touch_down 指针
 * @param  key_resp    key_response 指针
 * @return 1 = 已处理, 0 = 未处理
 */
static void menu_start_game_prep(uint8_t* game_inited);  /* 前向声明 */
static uint8_t main_menu_hit_test(uint16_t x, uint16_t y);  /* 前向声明 */
/**
 * @brief  主菜单命中检测与点击处理(返回值切换模式)。
 * @param  x           命中 X
 * @param  y           命中 Y
 * @param  mode        输出目标模式
 * @param  menu_drawn  菜单已绘制标志
 * @param  game_inited 游戏已初始化标志
 * @param  touch_down  触摸按下标志
 * @param  key_resp    按键响应
 */
uint8_t main_menu_handle(uint16_t x, uint16_t y,
    MainMode* mode, uint8_t* menu_drawn,
    uint8_t* game_inited, uint8_t* touch_down,
    uint8_t* key_resp) {
    uint8_t sel = main_menu_hit_test(x, y);
    if (sel == MAIN_MENU_ACT_NONE) return 0;
    game_battery_icon_reset();

    /* 游戏模式切换：清屏 + 设置背光 + 重置初始化标志 */
    if (sel == MAIN_MENU_ACT_D2048) {
        menu_start_game_prep(game_inited);
        *mode = MAIN_MODE_D2048;
    }
    else if (sel == MAIN_MENU_ACT_SOKOBAN) {
        menu_start_game_prep(game_inited);
        *mode = MAIN_MODE_SOKOBAN;
    }
    else if (sel == MAIN_MENU_ACT_TETRIS) {
        menu_start_game_prep(game_inited);
        *mode = MAIN_MODE_TETRIS;
    }
    else if (sel == MAIN_MENU_ACT_KLOTSKI) {
        menu_start_game_prep(game_inited);
        *mode = MAIN_MODE_KLOTSKI;
    }
    else if (sel == MAIN_MENU_ACT_FISH) {
        menu_start_game_prep(game_inited);
        *mode = MAIN_MODE_FISH;
    }
    else if (sel == MAIN_MENU_ACT_VIDEO) {
        /* 返回视频: 不黑屏, main.c视频循环直接渲染第一帧 */
        LoadVideoAddress();
        *key_resp = 1;
        *mode = MAIN_MODE_VIDEO;
        *game_inited = 0;
    }

    *menu_drawn = 0;
    return 1;
}

/**
 * @brief  主菜单触摸检测 (5行菜单: 推箱子/2048/俄罗斯方块/华容道/返回)
 *         封装 menu_hit_test + hit→动作码映射
 * @param  x, y  触摸坐标
 * @return MAIN_MENU_ACT_* 动作码
 */
uint8_t main_menu_hit_test(uint16_t x, uint16_t y) {
    /* 菜单框内(含上下边框带)由 menu_hit_test 夹到最近的行, 只有框外才返回 -1 → 返回视频 */
    int8_t hit = menu_hit_test(x, y, 5);
    if (hit < 0 || hit > 4) return MAIN_MENU_ACT_VIDEO;  /* 菜单外→返回视频 */
    /* 行0=推箱子, 1=2048, 2=俄罗斯方块, 3=华容道, 4=大鱼吃小鱼 (无返回行) */
    static const uint8_t map[5] = {
        MAIN_MENU_ACT_SOKOBAN, MAIN_MENU_ACT_D2048,
        MAIN_MENU_ACT_TETRIS, MAIN_MENU_ACT_KLOTSKI,
        MAIN_MENU_ACT_FISH
    };
    return map[hit];
}

/* 进入各游戏前共用的 3 步(清屏/亮背光/标记未初始化): 收成一个函数省 Flash */
static void menu_start_game_prep(uint8_t* game_inited) {
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    LCD_Backlight_PWM_SetDuty(g_brightness);
    *game_inited = 0;
}

/* ---- 内部辅助：ret→MainMode ---- */
static MainMode game_ret_to_mode(uint8_t ret) {
    switch (ret) {
    case EXIT_RET_D2048:   return MAIN_MODE_D2048;
    case EXIT_RET_SOKOBAN: return MAIN_MODE_SOKOBAN;
    case EXIT_RET_TETRIS:  return MAIN_MODE_TETRIS;
    case EXIT_RET_KLOTSKI: return MAIN_MODE_KLOTSKI;
    case EXIT_RET_FISH:    return MAIN_MODE_FISH;
    default:               return MAIN_MODE_VIDEO;
    }
}

/**
 * @brief  在格子里居中绘制数字(2048 与华容道共用)
 * @note   两份原实现(各约40行)的框架完全一致: val==0 直接返回 → uint_to_digits 分解 →
 *         LCD_CS 包住 → 二选一的字号分支 → 逐位居中绘制。差异只有三处, 已参数化:
 *         1) 字号档位: 2048 是"4位及以上才缩放", 华容道是"3位及以上才缩放"(big_first);
 *         2) 取色: 2048 按 val<=32 取黑/白, 华容道恒白 —— 由调用方传入 text_color;
 *         3) 两位数的间距: 2048 固定 16, 华容道用 (tile_size-16)/2 —— 见下面的 gap 计算。
 */
void game_grid_draw_number(uint16_t tx, uint16_t ty, uint16_t tile_size,
                           uint16_t val, uint16_t text_color,
                           uint8_t digit_offy, uint8_t big_first)
{
    uint8_t digits[8];
    uint8_t dc;
    uint8_t i;

    if (val == 0) return;

    dc = uint_to_digits(val, digits);

    LCD_CS_CLR;
    if (big_first ? (dc <= 2) : (dc <= 3)) {
        /* 16x32 大字号: 两位数间距按 tile_size 算(2048 位宽固定 16) */
        uint8_t gap;
        uint16_t total_w, sx, y;

        if (dc == 1)      gap = 0;
        else if (dc == 2) gap = big_first ? (uint8_t)((tile_size - 16) / 2) : 16;
        else              gap = (uint8_t)((tile_size - 16) / 2);

        total_w = (uint16_t)(16 + (dc - 1) * gap);
        sx = (uint16_t)(tx + (tile_size - total_w) / 2);
        y  = (uint16_t)(ty + digit_offy);
        for (i = 0; i < dc; i++) {
            sok_draw_digit_16x32(sx + i * gap, y, digits[dc - 1 - i], text_color);
        }
    }
    else {
        /* 缩放字号: 位数越多字越小, 总宽压在格子内 */
        uint8_t tw, th, gap;
        uint16_t total_w, sx, y;

        if (big_first) {
            tw = 10; th = 20; gap = 1;          /* 华容道 3 位 */
        }
        else if (dc == 4) { tw = 9; th = 18; gap = 1; }
        else if (dc == 5) { tw = 7; th = 14; gap = 1; }
        else              { tw = 6; th = 12; gap = 0; }

        total_w = (uint16_t)(dc * tw + (dc - 1) * gap);
        sx = (uint16_t)(tx + (tile_size - total_w) / 2);
        y  = (uint16_t)(ty + (tile_size - th) / 2);
        for (i = 0; i < dc; i++) {
            sok_draw_digit_scaled(sx + i * (tw + gap), y,
                                  digits[dc - 1 - i], text_color, tw, th);
        }
    }
    LCD_CS_SET;
}

/**
 * @brief  复位触摸状态(操作的全是 menu.h 里的全局: g_prev_pressed / g_touch_down /
 *         g_touch_start_x / g_touch_start_y / g_menu_cooldown)。
 * @note   d2048 与 klotski 的 reset_touch 逐字节相同, 合并成这一份;
 *         sokoban(用 s_* 私有变量) / fish(g_prev_pressed=0 + 拖动起点) /
 *         tetris(多置 g_wait_touch_release) 的实现各不相同, 仍各自保留。
 */
void game_reset_touch_common(void)
{
    g_prev_pressed = 1;      /* 设为1防止进入时触发释放, 待首帧清零 */
    g_touch_down = 0;
    g_touch_start_x = 0;
    g_touch_start_y = 0;
    g_menu_cooldown = 0;
}

/**
 * @brief  模式切换时重置触摸状态并标记等待松手。
 * @param  ret 切换返回命令
 */
static void game_reset_touch(uint8_t ret) {
    switch (ret) {
    case EXIT_RET_D2048:
    case EXIT_RET_KLOTSKI: game_reset_touch_common(); break;
    case EXIT_RET_SOKOBAN: sokoban_reset_touch(); break;
    case EXIT_RET_TETRIS:  tetris_reset_touch();  break;
    case EXIT_RET_FISH:    fish_reset_touch();    break;
    }
}

/**
 * @brief  统一游戏返回值处理 (4个游戏共用)
 *         根据 ret 码执行模式切换或返回视频
 * @param  ret        游戏 update 返回值 (GAME_RET_*)
 * @param  mode       s_main_mode 指针
 * @param  game_inited s_game_inited 指针
 * @param  touch_down s_touch_down 指针
 * @param  key_resp   key_response 指针
 * @return ret 原值 (便于调用方判断)
 */
uint8_t game_handle_ret(uint8_t ret, MainMode* mode, uint8_t* game_inited,
    uint8_t* touch_down, uint8_t* key_resp) {
    if (ret == EXIT_RET_NONE || ret == EXIT_RET_BACK) return ret;
    if (ret == GAME_RET_INTERACT) { *key_resp = 1; return ret; }
    if (ret == EXIT_RET_SETTINGS) { *key_resp = 1; return ret; }
    game_battery_icon_reset();
    g_menu_clear_color = LCD_BLACK;

    /* 模式切换只在松手后发生，同时清除主循环触摸状态 */
    *game_inited = 0;
    *touch_down = 0;
    g_game_consume_release = 0;  /* 模式切换清吸收标志, 防跨游戏残留 */

    if (ret == EXIT_RET_VIDEO) {
        /* 返回视频: 不黑屏, main.c视频循环直接渲染第一帧 */
        LoadVideoAddress();
        *key_resp = 1;
        *mode = MAIN_MODE_VIDEO;
    }
    else {
        LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
        *mode = game_ret_to_mode(ret);
        game_reset_touch(ret);
    }
    return ret;
}

/**
 * @brief  退出菜单触摸轮询 (4个游戏共用)
 *         按下时记录菜单行，松手仍在同一行才触发
 * @param  ret_map       hit索引(0-3)到返回码的映射表
 * @param  prev_pressed  上一帧触摸状态指针
 * @param  out_ts        输出触摸坐标
 * @return EXIT_RET_NONE(0) / EXIT_RET_BACK(0xFF) / EXIT_RET_*
 */
uint8_t game_exit_menu_poll(const uint8_t ret_map[5], uint8_t* prev_pressed,
    TouchState* out_ts) {
    static int8_t pressed_hit = -1;
    static uint16_t pressed_y = 0;
    static uint16_t pressed_x = 0;
    TouchState ts;
    Touch_Scan(&ts);
    if (out_ts) *out_ts = ts;

    if (!(*prev_pressed) && ts.pressed) {
        pressed_hit = menu_hit_test(ts.x, ts.y, 5);
        pressed_y = ts.y;
        pressed_x = ts.x;
    }
    else if (*prev_pressed && !ts.pressed) {
        int8_t released_hit = menu_hit_test(ts.x, ts.y, 5);
        uint8_t ret = EXIT_RET_NONE;

        /* 抬手坐标向下漂出菜单框外时, 以按下那一行为准。
           最后一行(如各游戏退出菜单的"华容道/大鱼吃小鱼")紧贴屏幕下沿, 抬手瞬间接触面
           向下滑出, 松手报的坐标常落到框外 → released_hit=-1, 与按下行不一致被判成
           "没点中", 现象就是"最下面那个选项怎么点都没反应, 点它下面反而关掉菜单"
           (点下面时按下即在框外, 才是走 EXIT_RET_BACK 关闭菜单)。
           限定只吸收"X 仍在菜单宽度内、Y 只是向下漂出框外"这一种抬手漂移,
           拖到菜单左/右侧外松手仍按原语义取消; 松手落在别的行也算滑走取消。 */
        if (released_hit < 0 && pressed_hit >= 0 && pressed_hit < 5
            && ts.x >= MENU_X && ts.x < MENU_X + MENU_W
            && ts.y >= MENU_Y + MENU_H)
            released_hit = pressed_hit;

        if (released_hit == pressed_hit
            && (int16_t)((int16_t)ts.y - (int16_t)pressed_y) <= MENU_SWIPE_COMMIT_DIST
            && (int16_t)((int16_t)pressed_y - (int16_t)ts.y) <= MENU_SWIPE_COMMIT_DIST) {
            if (released_hit >= 0 && released_hit < 5)
                ret = ret_map[released_hit];
            else
                ret = EXIT_RET_BACK;  /* hit==-1(菜单外)→关闭菜单; 无返回行 */
        }
        else if (((ts.x >= pressed_x ? ts.x - pressed_x : pressed_x - ts.x) <= MENU_SWIPE_COMMIT_DIST)
              && ((ts.y >= pressed_y ? ts.y - pressed_y : pressed_y - ts.y) <= MENU_SWIPE_COMMIT_DIST)) {
            /* pressed/released verdicts differ but the finger barely moved => it is a tap.
               Use the press-position verdict so a tap right on the outside/inside boundary
               (x == MENU_X) is not swallowed as a swipe (which would do nothing at all). */
            if (pressed_hit >= 0 && pressed_hit < 5)
                ret = ret_map[pressed_hit];
            else
                ret = EXIT_RET_BACK;
        }
        pressed_hit = -1;
        *prev_pressed = 0;
        return ret;
    }

    if (!ts.pressed) pressed_hit = -1;
    *prev_pressed = ts.pressed;
    return EXIT_RET_NONE;
}

/**
 * @brief  检测底部上划打断退出菜单 (d2048/tetris/klotski 共用)
 *         维护 g_touch_down/g_touch_start_y，在拖拽/释放时检测上划
 *         检测到时自动清理退出菜单(g_exit_*清零+LCD_Fill清屏)
 * @param  y_threshold 触摸起点Y阈值  @param  release_thresh 释放上划距离阈值
 * @param  dy_out       释放路径的输出滑动距离
 * @return 0=无动作, 1=拖拽中断(已清屏, 调用方切换状态+设置拖动脏位)
 *              2=释放中断(已清屏, 调用方切换状态+设置滑动脏位)
 */
uint8_t game_menu_swipe_up_poll(uint16_t y_threshold, uint8_t release_thresh,
    int16_t* dy_out) {
    TouchState ts_up;
    Touch_Scan(&ts_up);
    uint8_t prev_up = g_touch_down;
    if (ts_up.pressed) {
        if (!g_touch_down) { g_touch_down = 1; g_touch_start_y = ts_up.y; }
    }
    else { g_touch_down = 0; }

    /* 退出菜单滑入期间不认"按住上划": 手指刚下滑打开菜单后常还停在底部,
       按住路径只要 5px 就会误触发(见 game_menu_command 里 exit 分支的注释);
       滑入期间由"松手路径"(阈值 release_thresh, 通常 20px)负责打断 */
    if (ts_up.pressed && prev_up && !g_exit_sliding && g_touch_start_y > y_threshold
        && g_menu_cooldown == 0) {
        int16_t dy_move = (int16_t)ts_up.y - (int16_t)g_touch_start_y;
        if (dy_move < -5) {
            g_exit_dragging = 0;
            g_exit_sliding = 0;
            g_exit_menu_visible = 0;
            LCD_CLEAR_MENU_AREA();
            return 1;
        }
    }
    if (!ts_up.pressed && prev_up && g_touch_start_y > y_threshold
        && g_menu_cooldown == 0) {
        int16_t dy = (int16_t)ts_up.y - (int16_t)g_touch_start_y;
        if (dy < -(int16_t)release_thresh) {
            g_exit_dragging = 0;
            g_exit_sliding = 0;
            g_exit_menu_visible = 0;
            LCD_CLEAR_MENU_AREA();  /* 释放中断同样清退出菜单区, 否则调回重开菜单时退出菜单大块残留 */
            *dy_out = dy;
            return 2;
        }
    }
    return 0;
}

/**
 * @brief  退出菜单已显示(静止)时, 检测中间区域上划手势 关闭菜单返回游戏
 *         按起点分流: 底部(y>y_threshold)上划由 game_menu_swipe_up_poll 保留给切重菜,
 *         中间区域(起点 y 在 [10, y_threshold))上划才由本函数关闭菜单。推箱子不用本函数。
 * @param  y_threshold   底部切重菜起点阈值(起点<此值才属中间区域)
 * @param  release_thresh 释放上划距离阈值
 * @return 1=已检测到中间区域上划(调用方执行关闭菜单), 0=未触发
 */
uint8_t game_menu_swipe_mid_up_close(uint16_t y_threshold, uint8_t release_thresh) {
    static uint8_t  mu_dn = 0;      /* 中间区域上划追踪 */
    static uint16_t mu_sy = 0;      /* 起点Y */
    static uint8_t  mu_rel = 0;     /* 释放防抖 */
    TouchState ts;
    Touch_Scan(&ts);

    if (!ts.pressed) {
        if (mu_dn && ++mu_rel < 2) return 0;
        mu_dn = 0;
        mu_rel = 0;
        return 0;
    }
    mu_rel = 0;
    if (!mu_dn) { mu_dn = 1; mu_sy = ts.y; return 0; }
    /* 起点在中间区域且上划超过阈值 关闭菜单 */
    if (g_menu_cooldown == 0
        && mu_sy >= 10 && mu_sy < y_threshold
        && (int16_t)ts.y - (int16_t)mu_sy < -(int16_t)release_thresh) {
        mu_dn = 0;
        mu_rel = 0;
        g_menu_cooldown = 15;
        return 1;
    }
    return 0;
}

/**
 * @brief  检测中间区域下滑关闭上划菜单 (视频上滑菜单/游戏二级设置菜单共用)
 *         按起点分流: 顶部(起点<MENU_SWIPE_START_Y)下滑由调用方保留给"切换退出菜单",
 *         中间/底部区域(起点在[MENU_SWIPE_START_Y, 240))下滑才由本函数关闭菜单。
 * @param  start_min     中间区域起点Y下限(起点>=此值才属可关闭区域)
 * @param  release_thresh 下滑距离阈值
 * @return 1=已检测到中间区域下滑(调用方执行关闭菜单), 0=未触发
 * @note   与 game_menu_swipe_mid_up_close 对称, 独立 static 状态不复用, 防互抢。
 */
uint8_t game_menu_swipe_mid_down_close(uint16_t start_min, uint8_t release_thresh) {
    static uint8_t  md_dn = 0;      /* 中间区域下滑追踪 */
    static uint16_t md_sy = 0;      /* 起点Y */
    static uint8_t  md_rel = 0;     /* 释放防抖 */
    TouchState ts;
    Touch_Scan(&ts);

    if (!ts.pressed) {
        if (md_dn && ++md_rel < 2) return 0;
        md_dn = 0;
        md_rel = 0;
        return 0;
    }
    md_rel = 0;
    if (!md_dn) { md_dn = 1; md_sy = ts.y; return 0; }
    /* 起点在中间/底部区域且下滑超过阈值 关闭菜单 */
    if (g_menu_cooldown == 0
        && md_sy >= start_min
        && (int16_t)ts.y - (int16_t)md_sy > release_thresh) {
        md_dn = 0;
        md_rel = 0;
        g_menu_cooldown = 15;
        return 1;
    }
    return 0;
}

/**
 * @brief  反向滑关闭菜单后, 修正游戏共享触摸状态, 使松手不传入游戏
 * @note   反向滑(上划关闭下滑菜单/下滑关闭上划菜单)触发关闭时手指仍按住,
 *         游戏恢复后会重录按下起点并把"按下区"的冷却清掉, 导致松手被误判为
 *         一次滑动/拖动。此处把 g_touch_down 置 1 (视为已按下, 防重录/清冷却),
 *         并把 g_touch_start_* 记为当前手指位置, 使随后松手的位移≈0。
 */
void game_menu_fix_release_after_close(void) {
    TouchState t;
    Touch_Scan(&t);
    g_touch_down = t.pressed ? 1 : 0;  /* 手指仍按住则视为已按下(防重录起/清冷却), 已抬起则保持0 */
    g_touch_start_x = t.x;
    g_touch_start_y = t.y;
    g_game_consume_release = t.pressed ? 1 : 0;  /* 仅手指按住(反向滑)才标记吸收本次松手, 点击关闭不标记 */
}

/**
 * @brief  顶部下滑拖拽中触发退出菜单 (3个游戏共用)
 * @return 1=已进入退出菜单拖拽模式, 0=未触发
 */
uint8_t game_menu_enter_exit_drag(const TouchState* ts, uint8_t* state,
    uint8_t playing_state, uint8_t exit_state) {
    if (!ts->pressed || !g_touch_down || *state != playing_state
        || g_touch_start_y >= MENU_SWIPE_START_Y
        || g_menu_cooldown != 0)
        return 0;

    int16_t dy_move = (int16_t)ts->y - (int16_t)g_touch_start_y;
    if (dy_move <= MENU_SWIPE_TRIGGER_DIST)
        return 0;

    g_state_before_menu = *state;
    *state = exit_state;
    g_exit_menu_visible = 1;
    g_exit_dragging = 1;
    g_exit_drag_start_y = ts->y;
    if (dy_move > MENU_DRAG_MAX_STEP)
        dy_move = MENU_DRAG_MAX_STEP;
    g_exit_drag_y = -MENU_H + dy_move;
    return 1;
}

/**
 * @brief  底部上划拖拽中触发重新开始菜单 (3个游戏共用)
 * @return 1=已进入重新开始菜单拖拽模式, 0=未触发
 */
uint8_t game_menu_enter_restart_drag(const TouchState* ts, uint8_t* state,
    uint8_t playing_state, uint8_t restart_state,
    uint16_t y_threshold) {
    if (!ts->pressed || !g_touch_down || *state != playing_state
        || g_touch_start_y <= y_threshold
        || g_menu_cooldown != 0)
        return 0;

    int16_t dy_move = (int16_t)ts->y - (int16_t)g_touch_start_y;
    if (dy_move >= -5)
        return 0;

    g_state_before_menu = *state;
    *state = restart_state;
    g_restart_menu_visible = 1;
    g_restart_dragging = 1;
    g_restart_sliding = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;

    /* 用当前触摸状态立即启动跟手拖拽 */
    TouchState ts_copy;
    Touch_Scan(&ts_copy);
    game_restart_menu_drag_poll(&g_restart_slide_y, g_touch_start_y,
        &g_restart_dragging, &ts_copy);
    return 1;
}

/**
 * @brief  松手时提交退出菜单 (3个游戏共用)
 * @return 1=已提交退出菜单滑动, 0=未触发
 */
uint8_t game_menu_commit_exit(int16_t dy, uint8_t* state, uint8_t exit_state) {
    if (g_touch_start_y >= MENU_SWIPE_START_Y
        || dy < MENU_SWIPE_COMMIT_DIST
        || g_menu_cooldown != 0)
        return 0;

    g_state_before_menu = *state;
    *state = exit_state;
    g_exit_menu_visible = 1;
    g_exit_sliding = 1;
    g_exit_drag_y = -MENU_H;
    return 1;
}

/**
 * @brief  松手时提交重新开始菜单 (3个游戏共用)
 * @return 1=已提交重新开始菜单滑动, 0=未触发
 */
uint8_t game_menu_commit_restart(int16_t dy, uint8_t* state, uint8_t restart_state,
    uint16_t y_threshold, uint8_t swipe_dist) {
    if (g_touch_start_y <= y_threshold
        || dy >= -(int16_t)swipe_dist
        || g_menu_cooldown != 0)
        return 0;

    g_state_before_menu = *state;
    *state = restart_state;
    g_restart_menu_visible = 1;
    g_restart_sliding = 1;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H + dy;
    if (g_restart_slide_y < RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP)
        g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP;
    return 1;
}


/**
 * @brief  重开菜单清理: 复位重开菜单状态并置脏场景。
 */
void menu_restart_cleanup(void) {
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
    LCD_CLEAR_RESTART_AREA();
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
    g_touch_down = 0;
    g_restart_menu_visible = 0;
    g_restart_dragging = 0;
}

/**
 * @brief  重开菜单下滑打断进退出菜单(先清重菜区再进退出菜单)。
 * @param  state      当前状态指针
 * @param  exit_state 退出菜单状态
 * @param  touch_y    触摸 Y
 */
void game_menu_restart_to_exit(uint8_t* state, uint8_t exit_state,
    uint16_t touch_y) {
    g_touch_down = 0;  /* 重置触摸状态，让退出菜单的上划检测用当前位置重新建立参考点，防止立即振荡切回 */
    LCD_CLEAR_RESTART_AREA();
    *state = exit_state;
    g_exit_menu_visible = 1;
    g_exit_dragging = 1;
    g_exit_drag_start_y = touch_y;
    g_exit_drag_y = -MENU_H + MENU_DRAG_MAX_STEP;
}


/**
 * @brief  上划切换到重开菜单处理(按起点分流)。
 * @param  y_threshold   起点阈值
 * @param  release_thresh 松手阈值
 * @return 1=已触发, 0=未触发
 */
uint8_t menu_swipe_to_restart_handler(uint16_t y_threshold, uint8_t release_thresh) {
    int16_t dy = 0;
    uint8_t sw = game_menu_swipe_up_poll(y_threshold, release_thresh, &dy);
    if (!sw) return 0;
    g_restart_menu_visible = 1;
    if (sw == 1) {
        g_restart_dragging = 1;
        g_restart_sliding = 0;
        g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
        return 1;
    }
    else {
        g_restart_sliding = 1;
        g_restart_dragging = 0;
        g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H + dy;
        if (g_restart_slide_y < RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP)
            g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP;
        return 2;
    }
}


/**
 * @brief  把无符号整数拆成十进制数字串。
 * @param  val    待转换值
 * @param  digits 输出数字数组(每字节一位)
 * @return 数字位数
 */
uint8_t uint_to_digits(uint32_t val, uint8_t* digits) {
    uint8_t dc = 0;
    if (val == 0) { digits[dc++] = 0; }
    else { while (val > 0) { digits[dc++] = (uint8_t)(val % 10); val /= 10; } }
    return dc;
}


/**
 * @brief  退出菜单返回清理(复位退出菜单状态并置脏场景)。
 */
void menu_exit_back_cleanup(void) {
    g_exit_menu_visible = 0;
    g_exit_sliding = 0;
    g_exit_dragging = 0;
    LCD_CLEAR_MENU_AREA();
    g_touch_down = 0;
}

/**
 * @brief  退出菜单取消时按拖拽实际位置清除可见区域
 * @param  y  取消瞬间退出菜单的顶部Y坐标 (可能为负)
 */
void menu_clear_exit_area_to(int16_t y) {
    int16_t bottom = y + MENU_H;
    if (y < 0) y = 0;
    LCD_Fill(MENU_X, (uint16_t)y, MENU_X + MENU_W, (uint16_t)bottom, g_menu_clear_color);
}

/**
 * @brief  配置驱动游戏退出/重新开始菜单统一状态机 (d2048/klotski/tetris/fish 共用)
 *         把各游戏 update() 里重复的 EXIT_MENU/RESTART_MENU 状态处理收敛到一处:
 *         仅通过 GameMenuCfg 传入状态枚举/行类型/映射/阈值/背景重绘回调等差异点.
 * @return GAME_MENU_CMD_*; 调用方按命令执行 init/设置/切换等游戏侧动作
 */
uint8_t game_menu_command(const GameMenuCfg* cfg, uint8_t* state, uint8_t* out_ret) {
    *out_ret = 0;

    /* ---- 退出菜单 ---- */
    if (*state == cfg->exit) {
        /* 背景重绘(依据游戏脏标记), 非拖拽/滑动时再画菜单框 */
        if (cfg->bg_redraw() && !g_exit_dragging && !g_exit_sliding)
            menu_draw_nrow_frame_only(g_exit_drag_y, cfg->exit_row_types, 5);

        /* 底部上划打断 → 切换到重新开始菜单
           静止时: 按住/松手两条路径都判;
           滑入期间: 只认松手路径(game_menu_swipe_up_poll 里已按 g_exit_sliding 屏蔽按住路径),
           这样"下滑打开退出菜单"那一下的手指抖动不会误触发, 而真正想上划切菜单的手势仍能打断;
           拖拽期间不判(那时手势归拖拽所有) */
        if (!g_exit_dragging) {
            uint8_t r = menu_swipe_to_restart_handler(cfg->y_threshold, cfg->release_thresh);
            if (r) {
                cfg->mark_dirty();
                *state = cfg->restart;
                return (r == 2) ? GAME_MENU_CMD_INTERACT : GAME_MENU_CMD_NONE;
            }
        }

        /* 中间区域上划 → 关闭退出菜单返回游戏 (仅静止可见时, 起点在[10, y_threshold)才判;
           拖拽/滑入期间不判, 避免下滑刚进入菜单时被误关闭) */
        if (!g_exit_dragging && !g_exit_sliding
            && game_menu_swipe_mid_up_close(cfg->y_threshold, cfg->release_thresh)) {
            menu_exit_back_cleanup();
            game_menu_fix_release_after_close();  /* 上划关闭, 松手不传入游戏(须在cleanup后, 防其清touche状态) */
            *state = g_state_before_menu;
            cfg->mark_dirty();
            (void)cfg->bg_redraw();   /* 立即重绘场景, 防菜单清黑残留 */
            return GAME_MENU_CMD_NONE;
        }

        /* 拖拽 */
        if (g_exit_dragging) {
            TouchState ts_d;
            Touch_Scan(&ts_d);
            uint8_t poll_ret = game_menu_drag_poll(&g_exit_drag_y, &g_exit_drag_start_y,
                &g_exit_dragging, &ts_d, cfg->exit_row_types);
            if (poll_ret == 1) {
                g_exit_sliding = 1;
            }
            else if (poll_ret == 2) {
                menu_clear_exit_area_to(g_exit_drag_y);
                g_exit_menu_visible = 0;
                *state = g_state_before_menu;
                cfg->mark_dirty();
                (void)cfg->bg_redraw();   /* 上划取消退出菜单后立即重绘场景, 防清黑残留 */
                g_touch_down = 0;
                g_menu_cooldown = 15;
            }
            return GAME_MENU_CMD_NONE;
        }

        /* 滑入动画 */
        if (g_exit_sliding) {
            g_exit_drag_y += MENU_SLIDE_SPEED;
            if (g_exit_drag_y >= MENU_Y) {
                g_exit_drag_y = MENU_Y;
                g_exit_sliding = 0;
                g_touch_down = 0;
                g_prev_pressed = 0;  /* 滑入结束重置按下态, 防拖拽松手被误判为点菜单外关闭 */
                /* 菜单刚落定: 若此时手指已重新按住(通常就停在屏幕下方的菜单行上),
                   上划检测会把起点重录在那片底部区域 → 之后几像素上漂就会弹出上划菜单。
                   补一小段冷却把这次刚落下的触摸吸收掉。 */
                {
                    TouchState ts_now;
                    Touch_Scan(&ts_now);
                    if (ts_now.pressed) g_menu_cooldown = 15;
                }
            }
            menu_draw_nrow_frame_only(g_exit_drag_y, cfg->exit_row_types, 5);
            return GAME_MENU_CMD_NONE;
        }

        /* 点选 */
        {
            TouchState ts;
            uint8_t ret = game_exit_menu_poll(cfg->exit_map, &g_prev_pressed, &ts);
            if (ret == EXIT_RET_BACK) {
                menu_exit_back_cleanup();
                *state = g_state_before_menu;
                cfg->mark_dirty();
                (void)cfg->bg_redraw();   /* 立即重绘场景, 防菜单清黑残留到下一帧 */
                return GAME_MENU_CMD_NONE;
            }
            if (ret != EXIT_RET_NONE) {
                g_exit_menu_visible = 0;
                *state = cfg->playing;
                *out_ret = ret;
                return GAME_MENU_CMD_SWITCH;
            }
            return GAME_MENU_CMD_NONE;
        }
    }

    /* ---- 重新开始菜单 ---- */
    if (*state == cfg->restart) {
        uint8_t close_cmd = GAME_MENU_CMD_RETURN;   /* 默认点空白/取消 → 关闭回原状态 */
        uint8_t bg_dirty = cfg->bg_redraw();   /* 依据脏标记重绘场景; 返回脏标志, 供菜单框仅脏时重绘(同退出菜单) */

        /* 静止时: 场景脏才重绘菜单框, 避免每帧重绘与中央低电图标来回覆盖闪;
           右上角电池图标内部有节流, 每帧调用无害 */
        if (!g_restart_dragging && !g_restart_sliding) {
            if (cfg->bat_x != GAME_MENU_BAT_NONE)
                game_battery_icon_update(cfg->bat_x, cfg->bat_y, 0);
            if (bg_dirty)
                menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
        }
        TouchState ts;

        /* 拖拽 */
        if (g_restart_dragging) {
            Touch_Scan(&ts);
            uint8_t poll_ret = game_restart_menu_drag_poll(&g_restart_slide_y, g_touch_start_y,
                &g_restart_dragging, &ts);
            if (poll_ret == 1) {
                g_restart_sliding = 1;
                g_touch_down = 0;   /* 松手复位触摸, 保证下滑打断识别 */
                g_prev_pressed = 0;
            }
            else if (poll_ret == 2) {
                goto restart_close;
            }
            else if (poll_ret == 3) {
                goto restart_to_exit;
            }
            return GAME_MENU_CMD_NONE;
        }

        /* 滑入动画 */
        if (g_restart_sliding) {
            /* 菜单加高(130→160)后滑入距离变长, 曾按高度比例补偿成"平均 2.5px/帧", 但用户
               实测偏快 → 保持原速 2px/帧(宁可慢一点, 手感与别处一致) */
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
                    goto restart_to_exit;
            }
            return GAME_MENU_CMD_NONE;
        }

        Touch_Scan(&ts);

        if (game_restart_menu_down_poll(&ts, &g_touch_down,
            &g_touch_start_x, &g_touch_start_y))
            goto restart_to_exit;

        /* 中间区域下滑 → 关闭重菜返回游戏 (起点>=MENU_SWIPE_START_Y(40), 顶部下滑留给切退菜) */
        {
            static uint8_t  rd_dn = 0;
            static uint16_t rd_sy = 0;
            if (!ts.pressed) {
                rd_dn = 0;
            }
            else {
                if (!rd_dn) { rd_dn = 1; rd_sy = ts.y; }
                else if (rd_sy >= MENU_SWIPE_START_Y
                    && (int16_t)ts.y - (int16_t)rd_sy > MENU_SWIPE_TRIGGER_DIST
                    && g_menu_cooldown == 0) {
                    rd_dn = 0;
                    g_menu_cooldown = 15;
                    goto restart_close;
                }
            }
        }

        /* 点选重菜 (行归属与绘制用同一套行几何: 触摸区无缝隙, 点在两行之间的空隙按
           视觉中线归属, 不会掉空, 也不会把"设置"下半截算成"返回") */
        {
            static int8_t ph = -1;
            static uint16_t ph_y = 0;
            int8_t ch = -1;
            if (ts.x >= RESTART_MENU_X && ts.x < RESTART_MENU_X + RESTART_MENU_W)
                ch = menu_row_from_y(RESTART_MENU_Y, ts.y, 3);
            if (!g_prev_pressed && ts.pressed) {
                ph = ch;
                ph_y = ts.y;
            }
            else if (g_prev_pressed && !ts.pressed && ch == ph
                && (int16_t)((int16_t)ts.y - (int16_t)ph_y) <= MENU_SWIPE_COMMIT_DIST
                && (int16_t)((int16_t)ph_y - (int16_t)ts.y) <= MENU_SWIPE_COMMIT_DIST) {
                if (ph < 0) {
                    /* 点击菜单外 → 关闭 */
                    goto restart_close;
                }
                if (ph == 0) {
                    /* 重新开始 */
                    menu_restart_cleanup();
                    *state = cfg->playing;
                    g_prev_pressed = 0;
                    ph = -1;
                    return GAME_MENU_CMD_RESTART;
                }
                /* 设置 或 返回 */
                if (ph == 1)      close_cmd = GAME_MENU_CMD_SETTINGS;
                else              close_cmd = GAME_MENU_CMD_INTERACT;
                ph = -1;
                g_prev_pressed = 0;
                goto restart_close;
            }
            if (!ts.pressed) ph = -1;
            g_prev_pressed = ts.pressed;
            return GAME_MENU_CMD_NONE;
        }

    restart_to_exit:
        game_menu_restart_to_exit((uint8_t*)state, cfg->exit, ts.y);
        if (cfg->on_to_exit) cfg->on_to_exit();
        return GAME_MENU_CMD_NONE;

    restart_close:
        g_prev_pressed = ts.pressed;
        menu_restart_cleanup();
        game_menu_fix_release_after_close();  /* 下滑关闭/点选关闭, 松手不传入游戏(须在cleanup后) */
        *state = g_state_before_menu;
        cfg->mark_dirty();
        (void)cfg->bg_redraw();   /* 立即重绘场景, 防菜单清黑残留到下一帧 */
        return close_cmd;
    }

    return GAME_MENU_CMD_NONE;
}

#ifdef SIM_BUILD
/* ---- sim-only: the "settings options" strip (5 rows).  Used by the video mode
   and by the strip a game shows when it enters settings.  Returns 1 when it
   owns the screen. */
int menu_sim_video_touch_rects(void)
{
    if (s_video_menu_visible) {
        int top = (int)s_video_menu_slide_y;
        sim_touch_add_rows(VIDEO_MENU_X, VIDEO_MENU_W, top, top + VIDEO_MENU_H5 - 1, 5, SIM_TOUCH_TAP);
        return 1;
    }
    return 0;
}

/* ---- sim-only: exit (5 rows) / restart (3 rows) menus of the four CFG-driven
   games (d2048/klotski/tetris/fish).
   ★ 不能拿它判推箱子: 那四个游戏用共享的 g_exit_menu_visible, 而推箱子有自己的
   s_exit_menu_visible、从不碰共享那个; 但 main.c 的游戏设置页下拉路径(GameSettings_Update)
   会无条件把共享标志置 1, 推箱子关菜单时又不清它 → 残留下来会让叠加层一直画菜单。 */
int menu_sim_game_touch_rects(void)
{
    if (g_exit_menu_visible) {
        sim_touch_add_rows(MENU_X, MENU_W, MENU_Y, MENU_Y + MENU_H - 1, 5, SIM_TOUCH_TAP);
        return 1;
    }
    if (g_restart_menu_visible) {
        sim_touch_add_rows(RESTART_MENU_X, RESTART_MENU_W, RESTART_MENU_Y, 239, 3, SIM_TOUCH_TAP);
        return 1;
    }
    return 0;
}
#endif