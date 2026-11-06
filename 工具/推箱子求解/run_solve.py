#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跑还没解的关卡: python run_solve.py <solver.exe> [单关超时秒=900]
   新解出的结果先回放校验, 通过后追加进 solutions.txt。"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch_solver as P

HERE = os.path.dirname(os.path.abspath(__file__))
SOL = os.path.join(HERE, 'solutions.txt')
LEVELS, MAPS = P.load_levels()


def decode(i):
    rows, cols = LEVELS[i]
    cells = []
    for b in MAPS[i + 1]:
        cells.append(b // 10)
        cells.append(b % 10)
    cells = cells[:rows * cols]
    walls, goals, boxes, player = set(), set(), set(), None
    for k, v in enumerate(cells):
        rc = (k // cols, k % cols)
        if v in (1, 6):
            walls.add(rc)
        elif v == 2:
            goals.add(rc)
        elif v == 3:
            player = rc
        elif v == 4:
            boxes.add(rc)
        elif v == 5:
            boxes.add(rc); goals.add(rc)
        elif v == 7:
            player = rc; goals.add(rc)
    return rows, cols, walls, goals, boxes, player


def replay(i, seq):
    rows, cols, walls, goals, boxes, cur = decode(i)
    boxes = set(boxes)
    for ch in seq:
        dr, dc = {'U': (-1, 0), 'D': (1, 0), 'L': (0, -1), 'R': (0, 1)}[ch]
        nr, nc = cur[0] + dr, cur[1] + dc
        if not (0 <= nr < rows and 0 <= nc < cols):
            return False
        n = (nr, nc)
        if n in walls:
            return False
        if n in boxes:
            b = (nr + dr, nc + dc)
            if not (0 <= b[0] < rows and 0 <= b[1] < cols) or b in walls or b in boxes:
                return False
            boxes.remove(n); boxes.add(b)
        cur = n
    return boxes <= goals


def load_sol():
    got = {}
    if os.path.exists(SOL):
        for line in open(SOL, encoding='utf-8-sig'):
            f = line.split()
            if len(f) >= 3 and f[1] != '0':
                got[int(f[0]) - 1] = f[2]
    return got


def main():
    exe = sys.argv[1]
    timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 900
    got = load_sol()
    todo = [i for i in range(50) if i not in got]
    print('剩余 %d 关: %s' % (len(todo), [x + 1 for x in todo]))
    for i in todo:
        no = i + 1
        print('--- p%d ---' % no, flush=True)
        try:
            out = subprocess.run([exe, 'p%d' % no, str(timeout), '--solution'],
                                 capture_output=True, text=True, timeout=timeout + 120).stdout
        except subprocess.TimeoutExpired:
            print('  超时', flush=True)
            continue
        m = re.search(r'SOLUTION p\d+ moves=(\d+) (\S+)', out)
        if not m:
            reason = 'no solution'
            r = re.search(r'RESULT p\d+ optimal=(\S+)', out)
            if r:
                reason = r.group(1)
            print('  失败: ' + reason, flush=True)
            continue
        seq = m.group(2).upper()
        if not replay(i, seq):
            print('  解出来但回放不通(跳过)', flush=True)
            continue
        got[i] = seq
        with open(SOL, 'a', encoding='utf-8') as fh:   # 追加, 旧的先清掉该行太麻烦, 统一最后重写
            pass
        print('  OK %d 步' % len(seq), flush=True)
    # 统一重写
    lines = []
    for i in range(50):
        if i in got:
            lines.append('%02d %d %s' % (i + 1, len(got[i]), got[i]))
        else:
            lines.append('%02d 0 -' % (i + 1))
    open(SOL, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('完成: %d/50' % len(got))


if __name__ == '__main__':
    main()
