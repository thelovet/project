# -*- coding: utf-8 -*-
"""回读 sokoban_maps.c 里的 sok_demo_off/sok_demo_bits, 与 solutions.txt 逐关比对。
   sok_demo_bits 每关按字节对齐, 所以按关累加 ceil(步数/4) 得到该关的起始字节。"""
import os
import re

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(ROOT, '..', '..', '64MB触摸屏程序', 'SPI', 'SPI_LCD',
                                    'Game', 'font_library', 'sokoban_maps.c'))
txt = open(SRC, encoding='utf-8', errors='ignore').read()

off = [int(x) for x in re.findall(r'\d+', re.search(r'sok_demo_off\[.*?\]\s*=\s*\{(.*?)\};', txt, re.S).group(1))]
bits = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', re.search(r'sok_demo_bits\[\]\s*=\s*\{(.*?)\};', txt, re.S).group(1))]
assert len(off) == 51, len(off)

sol = {}
for line in open(os.path.join(ROOT, 'solutions.txt'), encoding='utf-8'):
    p = line.split()
    if len(p) >= 3:
        sol[int(p[0]) - 1] = '' if p[2] == '-' else p[2]

bad = 0
acc = 0
for lv in range(50):
    n = off[lv + 1] - off[lv]
    got = ''
    for i in range(n):
        b = bits[acc + (i >> 2)]
        got += 'UDLR'[(b >> (6 - (i & 3) * 2)) & 3]
    if got != sol[lv]:
        bad += 1
        print('关卡 %d 不一致 (len %d vs %d)' % (lv + 1, len(got), len(sol[lv])))
    acc += (n + 3) >> 2
print('总步数 %d, 数据 %d 字节, 不一致 %d 关' % (off[-1], len(bits), bad))
assert acc == len(bits), (acc, len(bits))
