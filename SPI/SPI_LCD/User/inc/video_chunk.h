#ifndef __VIDEO_CHUNK_H__
#define __VIDEO_CHUNK_H__

#include <stdint.h>

/* 由 gen_video_chunk.py 自动生成, 请勿手动编辑 */
#define VIDEO_CHUNK_OFFSET   8253440UL      /* 本块在外部Flash中的起始地址(从0开始) */
#define VIDEO_CHUNK_SIZE     52262UL      /* 本块数据长度(字节) */
#define VIDEO_CHUNK_INDEX    155UL      /* 块序号(0起) */
#define VIDEO_TOTAL_SIZE     8305702UL      /* 视频数据总字节数 */
#define VIDEO_BLOCK_COUNT    156UL      /* 总块数 */

extern const uint8_t g_video_chunk[VIDEO_CHUNK_SIZE];

#endif /* __VIDEO_CHUNK_H__ */
