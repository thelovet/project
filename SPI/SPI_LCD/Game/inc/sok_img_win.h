#ifndef __SOK_IMG_WIN_H__
#define __SOK_IMG_WIN_H__

#include <stdint.h>

/* 中文字库：32x32点阵编码, 每字分4行每行16字节, 共128字节/字 */
#define FONT_CHAR_W   16
#define FONT_CHAR_L   8
#define FONT_CHAR_SIZE (FONT_CHAR_L * FONT_CHAR_W)  /* 128 */

/**
 * @brief  绘制32x32中文字符 (每行16字节MSB优先)
 * @param  x     字符左上角X坐标
 * @param  y     字符左上角Y坐标
 * @param  font  字模数据 (128字节)
 * @param  color 显示颜色 (RGB565)
 */
void sok_draw_char_32(uint16_t x, uint16_t y, const uint8_t *font, uint16_t color);
void sok_draw_char_24(uint16_t x, uint16_t y, const uint8_t *font, uint16_t color);

/**
 * @brief  绘制推箱子胜利画面"恭喜通关", 使用32x32中文字库
 * @param  x  画面左上角X坐标
 * @param  y  画面左上角Y坐标
 */
void sok_win_draw(uint16_t x, uint16_t y);

/* 中文字模数据 (32x32) */
extern const uint8_t font_ka[FONT_CHAR_SIZE];      /* 卡 */
extern const uint8_t font_gong[FONT_CHAR_SIZE];    /* 恭 */
extern const uint8_t font_xi[FONT_CHAR_SIZE];      /* 喜 */
extern const uint8_t font_tong[FONT_CHAR_SIZE];    /* 通 */
extern const uint8_t font_guan[FONT_CHAR_SIZE];    /* 关 */
extern const uint8_t font_you[FONT_CHAR_SIZE];     /* 游 */
extern const uint8_t font_xi3[FONT_CHAR_SIZE];     /* 戏 */
extern const uint8_t font_jie[FONT_CHAR_SIZE];     /* 结 */
extern const uint8_t font_shu[FONT_CHAR_SIZE];     /* 束 */

/* 16x32 数字字库 */
#define FONT_DIGIT_W   16
#define FONT_DIGIT_H   32
extern const uint8_t font_digits[640];
#define FONT_DIGIT_BYTES  64

/**
 * @brief  绘制16x32数字字符
 * @param  x     字符左上角X坐标
 * @param  y     字符左上角Y坐标
 * @param  digit 数字 (0-9)
 * @param  color 显示颜色 (RGB565)
 */
void sok_draw_digit_16x32(uint16_t x, uint16_t y, uint8_t digit, uint16_t color);

/**
 * @brief  绘制可缩放16x32数字 (等比缩放)
 * @param  x     字符左上角X
 * @param  y     字符左上角Y
 * @param  digit 数字 (0-9)
 * @param  color 显示颜色
 * @param  tw    目标宽度 (px)
 * @param  th    目标高度 (px)
 */
void sok_draw_digit_scaled(uint16_t x, uint16_t y, uint8_t digit,
                           uint16_t color, uint8_t tw, uint8_t th);

/**
 * @brief  绘制关卡号 "关卡X" (32x32中文字 + 16x32数字缩放, 红色显示)
 * @param  level 关卡编号 (1-99)
 * @param  color 文字颜色
 */
void sok_level_draw(uint8_t level, uint16_t color);

#endif
