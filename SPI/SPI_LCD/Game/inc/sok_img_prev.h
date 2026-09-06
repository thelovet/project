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

#endif
