# -*- coding: utf-8 -*-
# Convert Íæ¼Ò.txt (RGB565 little-endian byte stream, white 0xFFFF transparent)
# into Game/font_library/player_imgs.c and Game/inc/player_imgs.h
#
# Usage: python gen_player_imgs.py "<path to Íæ¼Ò.txt>"
import re, os, sys

# player stages: (name, width, height) matching the txt header order (9 stages)
stages = [
    ("p0_20x16",  20, 16),
    ("p1_24x19",  24, 19),
    ("p2_28x22",  28, 22),
    ("p3_32x25",  32, 25),
    ("p4_36x28",  36, 28),
    ("p5_40x32",  40, 32),
    ("p6_44x35",  44, 35),
    ("p7_48x38",  48, 38),
    ("p8_52x41",  52, 41),
]

if len(sys.argv) > 1:
    SRC = sys.argv[1]
else:
    SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wanjia.txt")
print("SRC:", SRC)
if not os.path.exists(SRC):
    print("ERROR: source not found:", SRC)
    sys.exit(1)

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(ROOT, "SPI", "SPI_LCD", "Game", "font_library", "player_imgs.c")
OUT_H = os.path.join(ROOT, "SPI", "SPI_LCD", "Game", "inc", "player_imgs.h")

raw = open(SRC, "rb").read()
try:
    text = raw.decode("gbk")
except UnicodeDecodeError:
    text = raw.decode("utf-8", errors="replace")

blocks = re.findall(r"const uint16_t image_data\[\]\s*=\s*\{(.*?)\};", text, re.S)
print("found blocks:", len(blocks))
assert len(blocks) == 9, "expect 9 blocks, got %d" % len(blocks)

def extract_bytes(block):
    return [int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{2})", block)]

c_lines = []
c_lines.append('#include "player_imgs.h"')
c_lines.append("")
for idx, (name, w, h) in enumerate(stages):
    byte_list = extract_bytes(blocks[idx])
    expect = w * h * 2
    if len(byte_list) < expect:
        print("  [%s] short %d bytes, padding 0" % (name, expect - len(byte_list)))
        byte_list += [0] * (expect - len(byte_list))
    elif len(byte_list) > expect:
        print("  [%s] extra %d bytes, truncate" % (name, len(byte_list) - expect))
        byte_list = byte_list[:expect]

    var = "player_img_%d" % idx
    c_lines.append("/* %s %dx%d */" % (name, w, h))
    c_lines.append("const uint8_t %s[%d] = {" % (var, expect))
    for i in range(0, expect, 16):
        chunk = byte_list[i:i+16]
        hexes = ", ".join("0x%02x" % b for b in chunk)
        c_lines.append("    " + hexes + ",")
    c_lines.append("};")
    c_lines.append("")

h_lines = []
h_lines.append("#ifndef __PLAYER_IMGS_H__")
h_lines.append("#define __PLAYER_IMGS_H__")
h_lines.append("")
h_lines.append("#include <stdint.h>")
h_lines.append("")
h_lines.append("#define PLAYER_COUNT 9")
h_lines.append("")
for idx, (name, w, h) in enumerate(stages):
    h_lines.append("#define PLAYER_%d_W  %d" % (idx, w))
    h_lines.append("#define PLAYER_%d_H  %d" % (idx, h))
h_lines.append("")
h_lines.append("/* dims table: per player stage {w,h} */")
h_lines.append("extern const uint16_t player_imgs_dim[PLAYER_COUNT][2];")
h_lines.append("")
h_lines.append("/* per-stage RGB565 little-endian byte stream, len = w*h*2 */")
for idx, (name, w, h) in enumerate(stages):
    h_lines.append("extern const uint8_t player_img_%d[%d*%d*2];" % (idx, w, h))
h_lines.append("")
h_lines.append("#endif")

c_lines.append("const uint16_t player_imgs_dim[PLAYER_COUNT][2] = {")
for idx, (name, w, h) in enumerate(stages):
    c_lines.append("    { %d, %d }," % (w, h))
c_lines.append("};")

os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
with open(OUT_C, "w", encoding="ascii") as f:
    f.write("\n".join(c_lines) + "\n")
with open(OUT_H, "w", encoding="ascii") as f:
    f.write("\n".join(h_lines) + "\n")
print("written:")
print("  ", OUT_C)
print("  ", OUT_H)
print("DONE")
