# -*- coding: utf-8 -*-
# Undo-button images -> Game/font_library/sok_img_undo.c + Game/inc/sok_img_undo.h
#
# Source: the "undo buttons" section of the image-export txt, 6 images =
#         (prev-step / back / next-step) x (chinese / english), each 30x30 RGB565
#         little-endian = 1800 bytes.
# Output: each image gets one extra pure-black row (row 31, used at runtime to
#         erase the button bottom edge) -> 1860 bytes each; concatenated in
#         on-screen order [prev][back][next] -> 5580 bytes per set.
#
# Usage: python gen_btn_undo.py [source txt]     (default: ./<image txt>)
#
# NOTE: this source file is intentionally ASCII-only (use \u escapes) so it works
#       no matter whether the repo stores .py as GBK or UTF-8.
import re, os, sys

ROOT  = os.path.dirname(os.path.abspath(__file__))
# "\u4e0a\u4e00\u5173\u56fe\u7247.txt" = the image export txt file name
SRC   = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "\u4e0a\u4e00\u5173\u56fe\u7247.txt")
OUT_C = os.path.join(ROOT, "SPI", "SPI_LCD", "Game", "font_library", "sok_img_undo.c")
OUT_H = os.path.join(ROOT, "SPI", "SPI_LCD", "Game", "inc", "sok_img_undo.h")

IMG_W, IMG_H = 30, 31          # source 30x30 + the padded pure-black row
ONE  = IMG_W * IMG_H * 2       # 1860
SIZE = ONE * 3                 # 5580
# \u4e0a\u4e00\u6b65 = prev-step, \u8fd4\u56de = back, \u4e0b\u4e00\u6b65 = next-step
ORDER = ("\u4e0a\u4e00\u6b65", "\u8fd4\u56de", "\u4e0b\u4e00\u6b65")
K_CN  = "\u4e2d\u6587"         # chinese
K_EN  = "\u82f1\u6587"         # english

if not os.path.exists(SRC):
    print("ERROR: source not found:", SRC); sys.exit(1)

raw = open(SRC, "rb").read()
try:
    text = raw.decode("utf-8")
except UnicodeDecodeError:
    text = raw.decode("gbk", errors="replace")

# label of each image = nearest preceding comment
blocks = []
for m in re.finditer(r"const uint16_t image_data\[\]\s*=\s*\{(.*?)\};", text, re.S):
    head = text[max(0, m.start() - 300):m.start()]
    labs = re.findall(r"//\s*([^\r\n/]+)", head)
    label = labs[-1].strip() if labs else ""
    data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))]
    blocks.append((label, data))
print("parsed %d images" % len(blocks))

want = {name: {} for name in ORDER}
for label, data in blocks:
    name = next((k for k in ORDER if k in label), None)
    lang = "cn" if K_CN in label else ("en" if K_EN in label else None)
    if name and lang:
        if lang in want[name]:
            print("ERROR: duplicate %s/%s" % (name.encode("unicode_escape"), lang)); sys.exit(1)
        want[name][lang] = data

for name in ORDER:
    for lang in ("cn", "en"):
        if lang not in want[name]:
            print("ERROR: missing %s/%s" % (name.encode("unicode_escape"), lang)); sys.exit(1)
        if len(want[name][lang]) != IMG_W * 30 * 2:
            print("ERROR: %s/%s is %d bytes, expected %d (30x30)"
                  % (name.encode("unicode_escape"), lang, len(want[name][lang]), IMG_W * 30 * 2))
            sys.exit(1)

def to_blob(data):
    return bytes(data) + b"\x00" * (IMG_W * 2)      # append row 31 (pure black)

blobs = {lang: b"".join(to_blob(want[name][lang]) for name in ORDER)
         for lang in ("cn", "en")}
for lang in ("cn", "en"):
    assert len(blobs[lang]) == SIZE, len(blobs[lang])

def emit_array(f, name, blob):
    f.write("const uint8_t %s[%d] = {\n" % (name, SIZE))
    for i in range(0, len(blob), 12):
        f.write("    " + "".join("0x%02x, " % b for b in blob[i:i + 12]).rstrip() + "\n")
    f.write("};\n\n")

hdr = (
    "#ifndef __SOK_IMG_UNDO_H__\n"
    "#define __SOK_IMG_UNDO_H__\n"
    "\n"
    "#include \"ch32v20x.h\"   /* uint8_t */\n"
    "\n"
    "/* \u64a4\u9500\u754c\u9762 3 \u4e2a\u6309\u94ae\u56fe"
    "(\u4e0a\u4e00\u6b65/\u8fd4\u56de/\u4e0b\u4e00\u6b65, \u4e2d\u82f1\u4e24\u5957)\n"
    "   \u81ea\u52a8\u751f\u6210, \u4e0d\u8981\u624b\u6539: \u6539\u56fe\u8bf7\u6539"
    " \u4e0a\u4e00\u5173\u56fe\u7247.txt \u540e\u91cd\u8dd1 gen_btn_undo.py\n"
    "   \u6bcf\u5f20 30x31 RGB565 \u5c0f\u7aef = %d \u5b57\u8282"
    "(\u6e90 30x30 + \u7b2c 31 \u884c\u7eaf\u9ed1, \u8fd0\u884c\u671f\u64e6\u5e95\u8fb9\u7528);\n"
    "   \u4e00\u5957 3 \u5f20\u8fde\u5728\u4e00\u8d77, \u5e03\u5c40 = \u5c4f\u5e55\u987a\u5e8f"
    " [\u4e0a\u4e00\u6b65][\u8fd4\u56de][\u4e0b\u4e00\u6b65] = %d \u5b57\u8282 */\n"
    "#define UNDO_IMG_W    %d\n"
    "#define UNDO_IMG_H    %d\n"
    "#define UNDO_IMG_ONE  %d\n"
    "#define UNDO_IMG_SIZE %d\n"
    "\n"
    "/* 3 \u5f20\u56fe\u5728\u4e00\u5957\u91cc\u7684\u504f\u79fb"
    "(\u4e0e flash_store.h \u7684 BTN_IMG_UNDO_* \u4e00\u81f4) */\n"
    "#define UNDO_IMG_OFF_PREV  0\n"
    "#define UNDO_IMG_OFF_BACK  UNDO_IMG_ONE\n"
    "#define UNDO_IMG_OFF_NEXT  (UNDO_IMG_ONE * 2)\n"
    "\n"
    "extern const uint8_t g_btn_img_undo_cn[UNDO_IMG_SIZE];   /* \u4e2d\u6587\u5957 */\n"
    "extern const uint8_t g_btn_img_undo_en[UNDO_IMG_SIZE];   /* \u82f1\u6587\u5957 */\n"
    "\n"
    "#endif\n"
) % (ONE, SIZE, IMG_W, IMG_H, ONE, SIZE)

c_head = (
    "/* \u64a4\u9500\u754c\u9762\u6309\u94ae\u56fe"
    "(\u4e2d\u82f1\u5404 3 \u5f20: \u4e0a\u4e00\u6b65/\u8fd4\u56de/\u4e0b\u4e00\u6b65)\n"
    "   \u81ea\u52a8\u751f\u6210, \u4e0d\u8981\u624b\u6539: \u6539\u56fe\u8bf7\u6539"
    " \u4e0a\u4e00\u5173\u56fe\u7247.txt \u540e\u91cd\u8dd1 gen_btn_undo.py\n"
    "\n"
    "   \u4ec5\u70e7\u5f55\u56fa\u4ef6(BTN_IMG_STORE_MODE)\u5f15\u7528 -> "
    "\u6e38\u620f\u56fa\u4ef6\u91cc\u88ab --gc-sections \u56de\u6536(\u4e0d\u5360\u5185\u90e8 Flash);\n"
    "   \u8fd0\u884c\u671f\u4ece\u5916\u90e8 Flash \u56fa\u5b9a\u4fdd\u7559\u533a\u6309\u7edd\u5bf9\u5730\u5740\u8bfb"
    "(\u89c1 flash_store.h \u7684 BTN_IMG_UNDO_*) */\n"
    "#include \"sok_img_undo.h\"\n"
    "#include \"lcd.h\"           /* pulls in config.h (BTN_IMG_STORE_MODE) */\n"
    "\n"
    "#ifdef BTN_IMG_STORE_MODE\n"
    "\n"
)

with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
    f.write(hdr)

with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
    f.write(c_head)
    emit_array(f, "g_btn_img_undo_cn", blobs["cn"])
    emit_array(f, "g_btn_img_undo_en", blobs["en"])
    f.write("#endif /* BTN_IMG_STORE_MODE */\n")

print("generated:")
print("  ", OUT_C)
print("  ", OUT_H)
print("   per image: %d bytes x %d = %d bytes/set" % (ONE, len(ORDER), SIZE))
