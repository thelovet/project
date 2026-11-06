#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把项目里的 50 关写进 mkornreich/sokoban 的 solver.cpp, 并打好 3 处补丁。
   用法: python patch_solver.py <path/to/solver.cpp>
   地图来源: 64MB触摸屏程序/SPI/SPI_LCD/Game/font_library/sokoban_maps.c (BCD, 每字节 2 格)"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
MAPS_C = os.path.join(ROOT, '64MB触摸屏程序', 'SPI', 'SPI_LCD', 'Game',
                      'font_library', 'sokoban_maps.c')

# 我们的 tile -> 求解器数字: 0空 1墙 2目标 3玩家 4箱子 5箱在目标 6外界 7玩家在目标
CONV = {0: '0', 1: '1', 2: '4', 3: '3', 4: '2', 5: '5', 6: '1', 7: '6'}


def load_levels():
    txt = open(MAPS_C, encoding='utf-8', errors='ignore').read()
    m = re.search(r'sokoban_levels\[[^\]]*\]\s*=\s*\{(.*?)\};', txt, re.S)
    levels = [(int(a), int(b)) for a, b in re.findall(r'\{\s*(\d+)\s*,\s*(\d+)\s*\}', m.group(1))]
    maps = {}
    for mm in re.finditer(r'const\s+uint8_t\s+sok_map(\d+)\s*\[\s*\]\s*=\s*\{(.*?)\};', txt, re.S):
        body = re.sub(r'/\*.*?\*/', '', mm.group(2), flags=re.S)
        maps[int(mm.group(1))] = [int(x) for x in re.findall(r'\d+', body)]
    return levels, maps


def boards():
    levels, maps = load_levels()
    out = []
    for i, (rows, cols) in enumerate(levels):
        cells = []
        for b in maps[i + 1]:
            cells.append(b // 10)
            cells.append(b % 10)
        cells = cells[:rows * cols]
        rs = [''.join(CONV[cells[r * cols + c]] for c in range(cols)) for r in range(rows)]
        out.append('  {"p%d", {%s}},' % (i + 1, ','.join('"%s"' % r for r in rs)))
    return 'static unordered_map<string, vector<string>> BOARDS = {\n' + '\n'.join(out) + '\n};'


def main():
    cpp = sys.argv[1]
    src = open(cpp, encoding='utf-8', errors='ignore').read()

    # 补丁1: Windows 对齐分配
    if '_aligned_malloc' not in src:
        src = src.replace(
            '  void* p = aligned_alloc(ALIGN, bytes);',
            '#if defined(_WIN32)\n  void* p = _aligned_malloc(bytes, ALIGN);\n#else\n'
            '  void* p = aligned_alloc(ALIGN, bytes);\n#endif')

    # 补丁2: 箱子全到位即通关(不要求人站目标点)
    src = re.sub(r'if \(\(mask & goalBoxMask\) == mask\) \{.*?\n        \}',
                 'if ((mask & goalBoxMask) == mask) {\n'
                 '          uint32_t cand = g; uint32_t cb = best.load(memory_order_relaxed);'
                 ' while (cand < cb && !best.compare_exchange_weak(cb, cand, memory_order_relaxed));\n'
                 '        }', src, count=1, flags=re.S)
    src = re.sub(r'vector<uint32_t> dist = keeperDist\(mask, keeper\);\s*'
                 r'uint32_t fw = NONE, fg = NONE;\s*'
                 r'for \(uint32_t gc : goalCells\) if \(!boxAt\(mask, gc\) && dist\[gc\] < fw\) \{ fw = dist\[gc\]; fg = gc; \}\s*'
                 r'if \(fw != NONE && g \+ fw == bestv\) \{ maskF = mask; keeperF = keeper; freeGoalF = fg; break; \}',
                 'if (g == bestv) { maskF = mask; keeperF = keeper; break; }', src, count=1)
    src = src.replace('string finish = keeperPathMoves(maskF, keeperF, freeGoalF);',
                      'string finish;')

    # 补丁3: 宽表上限(大箱子关卡)
    src = re.sub(r'long double cap = \(long double\)\(\(size_t\)1 << \d+\);',
                 'long double cap = (long double)((size_t)1 << 29);', src, count=1)

    # 注入 50 关
    src = re.sub(r'static unordered_map<string, vector<string>> BOARDS = \{.*?\n\};',
                 boards(), src, count=1, flags=re.S)

    open(cpp, 'w', encoding='utf-8').write(src)
    print('已打补丁并注入 50 关: %s' % cpp)


if __name__ == '__main__':
    main()
