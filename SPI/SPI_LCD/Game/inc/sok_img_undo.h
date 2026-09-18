#ifndef __SOK_IMG_UNDO_H__
#define __SOK_IMG_UNDO_H__

#include "ch32v20x.h"   /* uint8_t */

/* 撤销界面 3 个按钮图(上一步/返回/下一步, 中英两套)
   自动生成, 不要手改: 改图请改 上一关图片.txt 后重跑 gen_btn_undo.py
   每张 30x31 RGB565 小端 = 1860 字节(源 30x30 + 第 31 行纯黑, 运行期擦底边用);
   一套 3 张连在一起, 布局 = 屏幕顺序 [上一步][返回][下一步] = 5580 字节 */
#define UNDO_IMG_W    30
#define UNDO_IMG_H    31
#define UNDO_IMG_ONE  1860
#define UNDO_IMG_SIZE 5580

/* 3 张图在一套里的偏移(与 flash_store.h 的 BTN_IMG_UNDO_* 一致) */
#define UNDO_IMG_OFF_PREV  0
#define UNDO_IMG_OFF_BACK  UNDO_IMG_ONE
#define UNDO_IMG_OFF_NEXT  (UNDO_IMG_ONE * 2)

extern const uint8_t g_btn_img_undo_cn[UNDO_IMG_SIZE];   /* 中文套 */
extern const uint8_t g_btn_img_undo_en[UNDO_IMG_SIZE];   /* 英文套 */

#endif
