#ifndef __VIDEO_CHUNK_H__
#define __VIDEO_CHUNK_H__

#include <stdint.h>

/* 由 gen_video_chunk.py 自动生成, 请勿手动编辑 */
/* 本块参数放在 g_chunk_meta[] 里(而不是直接写成宏), 因为 burn_all.ps1 的快速模式要按地址打补丁;
   下面的宏都改成读这个数组 —— extern const volatile 保证编译器不会跨编译单元把它折叠成常量。 */
#define VIDEO_CHUNK_MAX_SIZE 53248UL      /* 数据数组固定宽度(所有块一样, 末块补 0xFF) */

extern const volatile uint32_t g_chunk_meta[6];   /* [0]偏移 [1]长度 [2]块号 [3]总字节 [4]总块数 [5]CRC16 */

#define VIDEO_CHUNK_OFFSET   (g_chunk_meta[0])
#define VIDEO_CHUNK_SIZE     (g_chunk_meta[1])
#define VIDEO_CHUNK_INDEX    (g_chunk_meta[2])
#define VIDEO_TOTAL_SIZE     (g_chunk_meta[3])
#define VIDEO_BLOCK_COUNT    (g_chunk_meta[4])

extern const uint8_t g_video_chunk[VIDEO_CHUNK_MAX_SIZE];

#endif /* __VIDEO_CHUNK_H__ */
