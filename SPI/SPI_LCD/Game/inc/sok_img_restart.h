#ifndef __SOK_IMG_RESTART_H__
#define __SOK_IMG_RESTART_H__

#include <stdint.h>

// 图像尺寸: 30x30 像素
// 颜色格式: RGB565 (16位输出)
// 字节序: 小端序
#define RESTART_IMG_W  30
#define RESTART_IMG_H  31

/**
 * @brief  绘制重新开始图标(跳过白色背景, 叠加显示)
 * @param  x  左上角X坐标
 * @param  y  左上角Y坐标
 */
extern const uint16_t sok_img_restart[RESTART_IMG_W * RESTART_IMG_H];

void sok_restart_draw(uint16_t x, uint16_t y);

/* 英文模式下的"重新开始"按钮(内容不同, 尺寸与中文那套完全相同)
   资源 ID: FLASH_STORE_ID_IMG_RESTART_EN */
extern const uint16_t sok_img_restart_en[RESTART_IMG_W * RESTART_IMG_H];

#endif
