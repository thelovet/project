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
 *        LATIN24: 25 glyphs x 1200 B (16x24, menu rows, index 0 = space)
 *        LATIN32: 18 glyphs x 1152 B (16x32, win / game-over / level text)
 *        The index tables map an ASCII code to the glyph index (0xFF = not available).
 */
#define LATIN24_CHAR     48
/* LATIN24_W: menu glyph cell width in px (14 -> tighter spacing) */
#define LATIN24_W       14
#define LATIN32_W       16
#define LATIN32_CHAR     64
#define LATIN24_COUNT    26
#define LATIN32_COUNT    18
#define LATIN_INDEX_NONE 0xFF

extern const uint8_t font_latin24[1248];
extern const uint8_t font_latin32[1152];

extern const uint8_t g_latin24_idx[128];

extern const uint8_t g_latin32_idx[128];

#endif