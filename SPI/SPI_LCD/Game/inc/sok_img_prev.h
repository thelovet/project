#ifndef __SOK_IMG_PREV_H__
#define __SOK_IMG_PREV_H__

#include <stdint.h>

// 图像尺寸: 30x30 像素
// 颜色格式: RGB565 (16位输出)
// 字节序: 小端序
#define PREV_IMG_W  30
#define PREV_IMG_H  31

/**
 * @brief  绘制上一关图标(跳过白色背景, 叠加显示)
 * @param  x  左上角X坐标
 * @param  y  左上角Y坐标
 */
extern const uint16_t sok_img_prev[PREV_IMG_W * PREV_IMG_H];

void sok_prev_draw(uint16_t x, uint16_t y);
void sok_nav_draw_rgb565(uint16_t x, uint16_t y, const uint16_t *image);

/**
 * @brief  绘制按钮图并指定整数缩放倍率
 * @param  scale  1=30x31(撤销界面按钮), 2=60x62(一级按钮组), 0 视作 1
 */
void sok_nav_draw_rgb565_scaled(uint16_t x, uint16_t y, const uint16_t *image,
                                uint8_t scale);

/* 英文模式下的"上一关"按钮(内容不同, 尺寸与中文那套完全相同)
   资源 ID: FLASH_STORE_ID_IMG_PREV_EN */
extern const uint16_t sok_img_prev_en[PREV_IMG_W * PREV_IMG_H];

#ifdef BTN_IMG_STORE_MODE
/* 三张英文按钮图(上一关/下一关/重新开始), 每张 30x31 RGB565 小端(见 flash_store.c
   的 BtnImgStore_BurnAll; 保留区地址/布局见 flash_store.h 的 BTN_IMG_EN_*) */
extern const uint8_t g_btn_img_en[PREV_IMG_W * PREV_IMG_H * 2 * 3];
#endif

#endif
