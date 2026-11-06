# -*- coding: utf-8 -*-
"""把 工具/推箱子求解/solutions.txt 转成固件用的演示数据(C 数组)。

输出两段:
  sok_demo_off[51]  —— 每关在位流里的起始步号(未解关长度 0)
  sok_demo_bits[]   —— 每步 2bit(上=0 下=1 左=2 右=3, 高位在前), 共 839 字节

用法: python gen_demo_data.py            # 打印 C 代码
      python gen_demo_data.py --check    # 只做长度/字符校验
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOL = os.path.join(HERE, 'solutions.txt')
N = 50


def load():
    lens = [0] * N
    steps = [[] for _ in range(N)]
    for line in open(SOL, encoding='utf-8'):
        parts = line.split()
        if len(parts) < 3:
            continue
        lv = int(parts[0]) - 1
        path = parts[2]
        if path == '-':
            continue
        assert all(c in 'UDLR' for c in path), line
        assert len(path) == int(parts[1]), line
        lens[lv] = len(path)
        steps[lv] = ['UDLR'.index(c) for c in path]
    return lens, steps


def main():
    lens, steps = load()
    off = [0]
    for l in lens:
        off.append(off[-1] + l)
    total = off[-1]
    bits = []
    data = []
    for lv in range(N):
        bits = []
        for d in steps[lv]:
            bits.append((d >> 1) & 1)
            bits.append(d & 1)
        while len(bits) % 8:      # 每关单独按字节对齐(固件按关读取这一片)
            bits.append(0)
        data += [int(''.join(str(b) for b in bits[i:i + 8]), 2) for i in range(0, len(bits), 8)]

    if '--check' in sys.argv:
        print("已解关卡 %d/%d, 总步数 %d, 数据 %d 字节" % (sum(1 for l in lens if l), N, total, len(data)))
        return

    print("/* 每关解法起始步号(步数 = off[i+1]-off[i]; 0=未解, 该关无演示) */")
    print("const uint16_t sok_demo_off[SOKOBAN_LEVEL_COUNT + 1] = {")
    for i in range(0, N + 1, 10):
        print("    " + ", ".join("%d" % v for v in off[i:i + 10]) + ",")
    print("};")
    print()
    print("/* 每步 2bit(上0/下1/左2/右3, 高位在前), 每关按字节对齐, 共 %d 字节 */" % len(data))
    print("const uint8_t sok_demo_bits[] = {")
    for i in range(0, len(data), 16):
        print("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    print("};")


if __name__ == '__main__':
    main()
