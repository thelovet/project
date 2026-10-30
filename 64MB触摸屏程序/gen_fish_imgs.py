# -*- coding: utf-8 -*-
# Convert 潩??.txt (RGB565 little-endian byte stream, white 0xFFFF transparent)
# into Game/font_library/fish_imgs.c and fish_imgs.h
#
# Usage: python gen_fish_imgs.py "<path to 潩??.txt>"
import re, os, sys

# fish table: (name, width, height) matching the txt header order
fish = [
    ("srd_bait",  16, 12),
    ("xdy",       20, 10),
    ("ys_small",  24, 16),
    ("tw_sea",    28, 19),
    ("puffer",    32, 19),
    ("ray",       36, 21),
    ("common",    40, 28),
    ("grouper",   44, 22),
    ("tiger_shark", 48, 28),
    ("white_shark", 52, 31),
]

# resolve source path: arg, or default relative "潩??.txt"
if len(sys.argv) > 1:
    SRC = sys.argv[1]
else:
    SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "鱼图片.txt")
print("SRC:", SRC)
if not os.path.exists(SRC):
    print("ERROR: source not found:", SRC)
    sys.exit(1)

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(ROOT, "SPI", "SPI_LCD", "Game", "font_library", "fish_imgs.c")
OUT_H = os.path.join(ROOT, "SPI", "SPI_LCD", "Game", "inc", "fish_imgs.h")

# read file bytes, decode as gbk then utf8 fallback
raw = open(SRC, "rb").read()
try:
    text = raw.decode("utf-8")
except UnicodeDecodeError:
    text = raw.decode("gbk", errors="replace")

blocks = re.findall(r"const uint16_t image_data\[\]\s*=\s*\{(.*?)\};", text, re.S)
print("found blocks:", len(blocks))
assert len(blocks) == 10, "expect 10 blocks, got %d" % len(blocks)

def extract_bytes(block):
    return [int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{2})", block)]

c_lines = []
c_lines.append('#include "fish_imgs.h"')
c_lines.append("")
for idx, (name, w, h) in enumerate(fish):
    byte_list = extract_bytes(blocks[idx])
    expect = w * h * 2
    if len(byte_list) < expect:
        print("  [%s] short %d bytes, padding 0" % (name, expect - len(byte_list)))
        byte_list += [0] * (expect - len(byte_list))
    elif len(byte_list) > expect:
        print("  [%s] extra %d bytes, truncate" % (name, len(byte_list) - expect))
        byte_list = byte_list[:expect]

    var = "fish_img_%d" % idx
    c_lines.append("/* %s %dx%d */" % (name, w, h))
    c_lines.append("const uint8_t %s[%d] = {" % (var, expect))
    for i in range(0, expect, 16):
        chunk = byte_list[i:i+16]
        hexes = ", ".join("0x%02x" % b for b in chunk)
        c_lines.append("    " + hexes + ",")
    c_lines.append("};")
    c_lines.append("")

h_lines = []
h_lines.append("#ifndef __FISH_IMGS_H__")
h_lines.append("#define __FISH_IMGS_H__")
h_lines.append("")
h_lines.append("#include <stdint.h>")
h_lines.append("")
h_lines.append("#define FISH_COUNT 10")
h_lines.append("")
for idx, (name, w, h) in enumerate(fish):
    h_lines.append("#define FISH_%d_W  %d" % (idx, w))
    h_lines.append("#define FISH_%d_H  %d" % (idx, h))
h_lines.append("")
h_lines.append("/* dims table: per fish {w,h} */")
h_lines.append("extern const uint16_t fish_imgs_dim[FISH_COUNT][2];")
h_lines.append("")
h_lines.append("/* per-fish RGB565 little-endian byte stream, len = w*h*2 */")
for idx, (name, w, h) in enumerate(fish):
    h_lines.append("extern const uint8_t fish_img_%d[%d*%d*2];" % (idx, w, h))
h_lines.append("")
h_lines.append("#endif")

# dim table
c_lines.append("const uint16_t fish_imgs_dim[FISH_COUNT][2] = {")
for idx, (name, w, h) in enumerate(fish):
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
