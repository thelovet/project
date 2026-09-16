#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把源视频 .bin 切一块变成 C 数组: 生成 video_chunk.c / video_chunk.h
用法: python gen_video_chunk.py <块号> [raw] [--bin <视频.bin 路径>]
      (块号 0 起; raw = 只输出原始数据+CRC, 不生成 .c/.h)
      (--bin 不填时, 依次在 脚本目录 / 工程内 数据\娜娜莉 / 外层 数据\娜娜莉 里找第一个 *.bin)
输出:
  SPI_LCD/User/src/video_chunk.c   (g_chunk_meta[] 参数表 + g_video_chunk[] 数据数组)
  SPI_LCD/User/inc/video_chunk.h   (参数表声明 + 各参数宏)
  SPI_LCD/obj/chunk_data.bin       (本块原始数据, 定宽补 0xFF; 给"固定基准镜像打补丁"用)
  SPI_LCD/obj/chunk_meta.bin       (参数表二进制 6xuint32 小端; 同上)
说明(为什么参数放在 g_chunk_meta 里而不直接写成宏):
  burn_all.ps1 的快速模式是"一次编译好基准镜像, 之后每块只把数据补丁贴进镜像再烧",
  参数若写成编译期宏就没法打补丁, 所以宏改成读这个 extern const volatile 数组
  (跨编译单元 + volatile, 编译器不会把它当常量折叠掉)。
"""
import sys, os, struct

# --- 路径: 脚本可能放在工程内, 也可能放在工程外(原位置 ...\文件数据与源代码\数据\娜娜莉\) ---
# 所以不再依赖"固定的上级目录", 而是从脚本所在目录向上找"含 SPI/SPI_LCD/User/src 的目录"当工程根。
HERE = os.path.dirname(os.path.abspath(__file__))


def find_proj(start):
    """从 start 向上找工程根(含 SPI/SPI_LCD/User/src); 找不到返回 None"""
    d = start
    for _ in range(8):
        if os.path.isdir(os.path.join(d, "SPI", "SPI_LCD", "User", "src")):
            return d
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return None


def find_proj_sibling(start):
    r"""脚本在工程外时(原位置 ...\文件数据与源代码\数据\娜娜莉\): 去 ../../源代码/ 下逐个子目录找"""
    for up in (os.path.join("..", ".."), os.path.join("..", "..", "..")):
        src_root = os.path.normpath(os.path.join(start, up, "源代码"))
        if not os.path.isdir(src_root):
            continue
        try:
            for name in sorted(os.listdir(src_root)):
                d = os.path.join(src_root, name)
                if os.path.isdir(os.path.join(d, "SPI", "SPI_LCD", "User", "src")):
                    return d
        except OSError:
            pass
    return None


def parse_args(argv):
    """块号/raw/--bin 路径 三种参数, 顺序任意"""
    idx, raw, binp = 0, False, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--bin":
            i += 1
            binp = argv[i] if i < len(argv) else None
        elif a.startswith("--bin="):
            binp = a.split("=", 1)[1]
        elif a.lower() == "raw":
            raw = True
        else:
            try:
                idx = int(a)
            except ValueError:
                pass
        i += 1
    return idx, raw, binp


def pick_bin(binp_arg, proj):
    """源视频数据: 优先 --bin, 其次脚本目录, 其次工程内/外层 数据\娜娜莉"""
    if binp_arg:
        return binp_arg if os.path.isfile(binp_arg) else None
    cands = [HERE]
    if proj:
        cands.append(os.path.join(proj, "数据", "娜娜莉"))
        outer = os.path.dirname(os.path.dirname(proj))        # ...\文件数据与源代码
        cands.append(os.path.join(outer, "数据", "娜娜莉"))
    for d in cands:
        if d and os.path.isdir(d):
            bins = sorted(f for f in os.listdir(d) if f.lower().endswith(".bin"))
            if bins:
                return os.path.join(d, bins[0])
    return None


PROJ = find_proj(HERE) or find_proj_sibling(HERE)
if not PROJ:
    print("error: 找不到工程根(应含 SPI/SPI_LCD/User/src)。请把本脚本放在工程目录内再运行。")
    sys.exit(2)
SRC_DIR = os.path.join(PROJ, "SPI", "SPI_LCD")
OUT_C = os.path.join(SRC_DIR, "User", "src", "video_chunk.c")
OUT_H = os.path.join(SRC_DIR, "User", "inc", "video_chunk.h")
OUT_META = os.path.join(SRC_DIR, "obj", "chunk_meta.bin")
OUT_DATA = os.path.join(SRC_DIR, "obj", "chunk_data.bin")

BLOCK_SIZE = 13 * 4096   # 53248 字节 = 13 个 4K 块; 数据数组固定按这个宽度生成

# --- CRC16/MODBUS(多项式0x8005反向0xA001, 初值0xFFFF): 必须与固件 flash_store.c 完全一致 ---
_CRC_TBL = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xA001 if (_c & 1) else (_c >> 1)
    _CRC_TBL.append(_c)


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc = (crc >> 8) ^ _CRC_TBL[(crc ^ b) & 0xFF]
    return crc


def main():
    idx, raw_only, binp = parse_args(sys.argv[1:])
    binfile = pick_bin(binp, PROJ)
    if not binfile:
        print("error: 找不到源视频 .bin 文件。"
              "用 --bin <路径> 指定, 或把 .bin 放到脚本目录 / 工程内 数据\\娜娜莉 / 外层 数据\\娜娜莉 下。")
        sys.exit(2)

    total = os.path.getsize(binfile)
    if not raw_only:
        print("video source: %s" % binfile)
        print("video total size: %d bytes (%.3f MB)" % (total, total / 1024 / 1024))

    nblocks = (total + BLOCK_SIZE - 1) // BLOCK_SIZE
    offset = idx * BLOCK_SIZE

    with open(binfile, "rb") as f:
        f.seek(offset)
        data = f.read(BLOCK_SIZE)
    if not data:
        print("error: block %d out of range (total %d blocks)" % (idx, nblocks))
        sys.exit(1)

    crc = crc16(data)
    # 数据数组定宽(末块补 0xFF): 保证每块生成的文件形状一致 -> 基准镜像布局固定, 补丁偏移才稳
    padded = data + b"\xFF" * (BLOCK_SIZE - len(data))

    # 原始数据 + 参数表(给 burn_all.ps1 的快速模式打补丁用)
    with open(OUT_DATA, "wb") as f:
        f.write(padded)
    with open(OUT_META, "wb") as f:
        f.write(struct.pack("<6I", offset, len(data), idx, total, nblocks, crc))

    if raw_only:
        print("block %d raw ready: offset=%d, size=%d, crc=0x%04X" % (idx, offset, len(data), crc))
        print("CRC=0x%04X" % crc)
        return

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("""#ifndef __VIDEO_CHUNK_H__
#define __VIDEO_CHUNK_H__

#include <stdint.h>

/* 由 gen_video_chunk.py 自动生成, 请勿手动编辑 */
/* 本块参数放在 g_chunk_meta[] 里(而不是直接写成宏), 因为 burn_all.ps1 的快速模式要按地址打补丁;
   下面的宏都改成读这个数组 —— extern const volatile 保证编译器不会跨编译单元把它折叠成常量。 */
#define VIDEO_CHUNK_MAX_SIZE %dUL      /* 数据数组固定宽度(所有块一样, 末块补 0xFF) */

extern const volatile uint32_t g_chunk_meta[6];   /* [0]偏移 [1]长度 [2]块号 [3]总字节 [4]总块数 [5]CRC16 */

#define VIDEO_CHUNK_OFFSET   (g_chunk_meta[0])
#define VIDEO_CHUNK_SIZE     (g_chunk_meta[1])
#define VIDEO_CHUNK_INDEX    (g_chunk_meta[2])
#define VIDEO_TOTAL_SIZE     (g_chunk_meta[3])
#define VIDEO_BLOCK_COUNT    (g_chunk_meta[4])

extern const uint8_t g_video_chunk[VIDEO_CHUNK_MAX_SIZE];

#endif /* __VIDEO_CHUNK_H__ */
""" % BLOCK_SIZE)

    lines = ["/* 由 gen_video_chunk.py 自动生成, 请勿手动编辑 */",
             '#include "video_chunk.h"',
             "",
             "/* 本块参数: 偏移/长度/块号/总字节/总块数/CRC16 (burn_all.ps1 快速模式会按地址覆盖这 24 字节) */",
             "const volatile uint32_t g_chunk_meta[6] = {",
             "    %dUL, %dUL, %dUL, %dUL, %dUL, 0x%04XUL" % (offset, len(data), idx, total, nblocks, crc),
             "};",
             "",
             "const uint8_t g_video_chunk[VIDEO_CHUNK_MAX_SIZE] = {"]
    row = []
    for b in padded:
        row.append("0x%02X" % b)
        if len(row) == 16:
            lines.append("    " + ",".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ",".join(row))
    lines.append("};")
    lines.append("")
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    remain = total - (offset + len(data))
    print("block %d ready: offset=%d, size=%d, crc=0x%04X" % (idx, offset, len(data), crc))
    print("total blocks=%d, remain bytes=%d, remain blocks=%d" % (nblocks, remain, nblocks - idx - 1))
    print("CRC=0x%04X" % crc)


if __name__ == "__main__":
    main()
