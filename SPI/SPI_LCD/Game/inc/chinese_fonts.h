#ifndef __CHINESE_FONTS_H__
#define __CHINESE_FONTS_H__

#include <stdint.h>

/**
 * @brief 24x24中文点阵字模 (72字节/字)
 * @note  推 箱 子 视 频 返 重 新 开 始 回
 *        亮 度 设 置 息 屏 时 间 幕 反 向
 */
extern const uint8_t font_tui[72];
extern const uint8_t font_xiang[72];
extern const uint8_t font_zi[72];
extern const uint8_t font_shi[72];
extern const uint8_t font_pin[72];
extern const uint8_t font_fan[72];
extern const uint8_t font_chong[72];
extern const uint8_t font_xin2[72];
extern const uint8_t font_kai2[72];
extern const uint8_t font_shi3[72];
extern const uint8_t font_hui[72];

extern const uint8_t font_liang[72];
extern const uint8_t font_du[72];
extern const uint8_t font_she[72];
extern const uint8_t font_zhi[72];
extern const uint8_t font_xi2[72];
extern const uint8_t font_ping[72];
extern const uint8_t font_shi2[72];
extern const uint8_t font_jian[72];
extern const uint8_t font_mu[72];
extern const uint8_t font_fan2[72];
extern const uint8_t font_xiang2[72];

extern const uint8_t font_yu[72];   /* 鱼(主菜单小鱼图标, 直接放内部Flash) */

/**
 * @brief 菜单"游戏名"追加汉字 (24x24, 72字节/字, 共 12 个字)
 *        顺序(即 g_menu_glyph_ext 里字节的排列): 方 块 消 行 | 华 容 道 | 鱼 大 小 | 合 并
 * @note  这 12 个字不进资源表也不放内部 Flash:
 *        字库固件内部 Flash 只剩几百 B 装不下(12*72=864B), 而改资源表就要重烧整个字库。
 *        所以和英文按钮图一样由 BTN_IMG_STORE_MODE 单独烧一次, 存在"字库数据区紧前面"
 *        的那一小块(MENU_GLYPH_SIZE), 游戏固件也不占空间(只在中文模式下载入字模缓存)。
 *        虚 ID 从 128 起(资源 ID 最大 127, 不会冲突), 槽位换算见 menu.c 的 MENU_FONT_SLOT。
 *        ★ 顺序不能改: 现有 10 个字的字节位置必须保持不变(加字只能往后追加)
 */
#define MENU_EXT_ID_BASE   128
#define MENU_EXT_CNT       12
#define MENU_EXT_ID_FANG   (MENU_EXT_ID_BASE + 0)   /* 方 */
#define MENU_EXT_ID_KUAI   (MENU_EXT_ID_BASE + 1)   /* 块 */
#define MENU_EXT_ID_XIAO2  (MENU_EXT_ID_BASE + 2)   /* 消 */
#define MENU_EXT_ID_XING   (MENU_EXT_ID_BASE + 3)   /* 行 */
#define MENU_EXT_ID_HUA    (MENU_EXT_ID_BASE + 4)   /* 华 */
#define MENU_EXT_ID_RONG   (MENU_EXT_ID_BASE + 5)   /* 容 */
#define MENU_EXT_ID_DAO    (MENU_EXT_ID_BASE + 6)   /* 道 */
#define MENU_EXT_ID_YU     (MENU_EXT_ID_BASE + 7)   /* 鱼 */
#define MENU_EXT_ID_DA     (MENU_EXT_ID_BASE + 8)   /* 大 */
#define MENU_EXT_ID_XIAO   (MENU_EXT_ID_BASE + 9)   /* 小 */
#define MENU_EXT_ID_HE     (MENU_EXT_ID_BASE + 10)  /* 合 */
#define MENU_EXT_ID_BING   (MENU_EXT_ID_BASE + 11)  /* 并 */

extern const uint8_t g_menu_glyph_ext[864];

/**
 * @brief 24x24 追加汉字 "语言设置"页用 (语 言) + 子页选项 (中 文)
 * @note  资源 ID 排在外部Flash资源表末尾(115~118), 不改动前面已有的 ID ——
 *        这样没重烧字库的旧板子只是这几个字显示空白, 其它功能照常
 */
extern const uint8_t font_yu2[72];    /* 语 */
extern const uint8_t font_yan[72];    /* 言 */
extern const uint8_t font_zhong[72];  /* 中 */
extern const uint8_t font_wen[72];    /* 文 */

/**
 * @brief 16x24 半宽拉丁字母 "English" 选项用 (48字节/字: 24行 x 2字节, MSB在左)
 */
extern const uint8_t font_lat_E[48];
extern const uint8_t font_lat_n[48];
extern const uint8_t font_lat_g[48];
extern const uint8_t font_lat_l[48];
extern const uint8_t font_lat_i[48];
extern const uint8_t font_lat_s[48];
extern const uint8_t font_lat_h[48];


/**
 * @brief English UI glyph sets (resources: FLASH_STORE_ID_LATIN24 / FLASH_STORE_ID_LATIN32)
 *        LATIN24: 29 glyphs x 1392 B = 1344 B (14x24, menu rows, index 0 = space)
 *        LATIN32: 18 glyphs x 1152 B (16x32, win / game-over / level text)
 *        The index tables map an ASCII code to the glyph index (0xFF = not available).
 *        ★ 加词要同时把 gen_latin2.py 的 MENU_WORDS 补上再重生成(否则缺字母不报错,
 *          只是那个字不显示、后面的字往前挪一点点, 看着像"没居中")
 */
#define LATIN24_CHAR     48
/* LATIN24_W: menu glyph cell width in px (14 -> tighter spacing) */
#define LATIN24_W       14
#define LATIN32_W       16
#define LATIN32_CHAR     64
#define LATIN24_COUNT    29
#define LATIN32_COUNT    18
#define LATIN_INDEX_NONE 0xFF

extern const uint8_t font_latin24[1392];
extern const uint8_t font_latin32[1152];

extern const uint8_t g_latin24_idx[128];

extern const uint8_t g_latin32_idx[128];

#endif