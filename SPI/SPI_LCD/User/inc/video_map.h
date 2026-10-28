/* 视频段地址表 —— 由烧录面板(导入视频 bin 时)自动生成, 不要手改。
   每段 8 字节 = 起始地址(4 字节, 大端) + 结束地址(4 字节, 大端)。
   段数 = 导入的 bin 个数; 地址按"多个 bin 依序首尾相连烧到 Flash 地址 0"计算。
   生成时间: 2026-10-28 18:44:46 */
/* 段1: 娜娜莉.bin                   7979038 字节  0x00000000-0x0079C01D */
#ifndef VIDEO_MAP_H
#define VIDEO_MAP_H

#define VIDEO_SEGMENT_COUNT  1

#define VIDEO_RANGES_INIT \
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0xC0, 0x1D }

#endif /* VIDEO_MAP_H */
