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
/* ★ 视频段数: 同时决定 video_ranges 的行数, 必须等于表里"有值的行数"(当前 8 行 = 压缩数据1~8)。
   填错的后果: ①填 1 时 `(idx+1) % max_video_count` 恒为 0 -> 点击变成"重播当前段"、切不了视频;
               ②填多了多出全 0 行 -> seg_end=0, 播到帧末立刻重播(看着像卡住)。
   以后启用 decompress.c 表里那些注释掉的段时, 记得把这个数字一起加。 */
#define VIDEO_SEGMENT_COUNT  4
// 视频段地址范围表：每段{起始地址4字节, 结束地址4字节}
extern const uint8_t video_ranges[VIDEO_SEGMENT_COUNT][8];

/* 加载当前视频地址到 next_bank/higvh/high/low */
void LoadVideoAddress(void);

#endif
