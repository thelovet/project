/* 视频段地址表 —— 由烧录面板(导入视频 bin 时)自动生成, 不要手改。
   每段 8 字节 = 起始地址(4 字节, 大端) + 结束地址(4 字节, 大端)。
   段数 = 导入的 bin 个数; 地址按"多个 bin 依序首尾相连烧到 Flash 地址 0"计算。
   生成时间: 2026-10-31 09:06:21 */
/* 段1: 压缩数据1.bin                  276061 字节  0x00000000-0x0004365C */
/* 段2: 压缩数据2.bin                  543034 字节  0x0004365D-0x000C7F96 */
/* 段3: 压缩数据3.bin                  552068 字节  0x000C7F97-0x0014EC1A */
/* 段4: 压缩数据4.bin                 1640325 字节  0x0014EC1B-0x002DF39F */
#ifndef VIDEO_MAP_H
#define VIDEO_MAP_H

#define VIDEO_SEGMENT_COUNT  4

#define VIDEO_RANGES_INIT \
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x36, 0x5C }, \
    { 0x00, 0x04, 0x36, 0x5D, 0x00, 0x0C, 0x7F, 0x96 }, \
    { 0x00, 0x0C, 0x7F, 0x97, 0x00, 0x14, 0xEC, 0x1A }, \
    { 0x00, 0x14, 0xEC, 0x1B, 0x00, 0x2D, 0xF3, 0x9F }

#endif /* VIDEO_MAP_H */
