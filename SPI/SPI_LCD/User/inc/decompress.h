#include "debug.h"
#include "config.h"

#ifndef __decompress_h
#define __decompress_h 			   

u16 flash_decompress_and_show(u8 *location4, u16 *location3, u16 *location2, u16 *location1);
void Set_display_range();
void Set_display_range_DMA();

extern uint8_t first_frame;//是否是第一帧
extern uint8_t end_flag;//是否结束
extern uint8_t lcd_blink_flag;//lCD闪烁标志位
extern uint8_t key_response;//触摸菜单输入响应标志
extern uint8_t blink_touched; //闪烁期间触摸标志，main.c用后清零

//当前视频序号
extern uint8_t current_video_index;
//最大视频数量
extern uint8_t max_video_count;
// 视频段数量(数组第一维, 与decompress.c定义一致, 修改时只需改此宏)
#define VIDEO_SEGMENT_COUNT  1
// 视频段地址范围表：每段{起始地址4字节, 结束地址4字节}
extern const uint8_t video_ranges[VIDEO_SEGMENT_COUNT][8];

/* 加载当前视频地址到 next_bank/higvh/high/low */
void LoadVideoAddress(void);

#endif
