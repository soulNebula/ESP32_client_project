#!/usr/bin/env python3
# Python
# 出 Arduino 汉字点阵
# -*- coding: utf-8 -*-
"""
gen_font.py -- 生成 arduino/SmartHomeArduino/panel_font_data.h

汉字用 SimHei 17pt（和参考工程同一个字体、同一个字号）渲染成 16x16 逐列点阵；
ASCII 8x16 不重新渲染，直接从参考工程 components/ui/font_cn_data.c 里
把 panel_ascii_8x16 表原样抄过来，保证屏上的字形和 ESP-IDF 版一模一样。

点阵格式（逐列式 1bpp）：
    每字节 = 同一列上连续 8 个像素，bit0 = 该列最上面一行
    先从左到右排完所有列，再排下一页（每页 8 行）
    ASCII 8x16 : 8 列 x 2 页 = 16 字节
    汉字 16x16 : 16 列 x 2 页 = 32 字节

用法：
    python tools/gen_font.py                # 用 tools/font_charset.txt
    python tools/gen_font.py --scan         # 先扫源码刷新字符集，再生成
    python tools/gen_font.py --preview      # 顺便打印点阵图案
"""

import argparse
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

# 本脚本在 arduino/SmartHomeArduino/tools/ 下
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(PROJ))

SRC_ASCII = os.path.join(ROOT, "components", "ui", "font_cn_data.c")
# 必须和 .ino 平铺同一层：Arduino 只编根目录的源文件
OUT_FILE = os.path.join(PROJ, "panel_font_data.h")
CHARSET_FILE = os.path.join(HERE, "font_charset.txt")

TTF = "C:/Windows/Fonts/simhei.ttf"
CELL = 16
CANVAS = 64
PAD = 16
# 从 17 开始试：参考工程的表就是 17pt 出的，必须一致
EM_LIST = (17, 16, 15, 14, 13, 12, 11, 10)

SCAN_EXT = (".h", ".hpp", ".c", ".cc", ".cpp", ".ino")
# 这几个是自动生成的字模，扫了等于自己喂自己
SCAN_SKIP = ("panel_font_data.h",)
# 温度单位：不在 ASCII 表里，源码里有没有都要留着
EXTRA_SYMBOLS = "\u00B0\u2103"


def wanted(cp):
    """只留汉字和两个温度符号"""
    return 0x4E00 <= cp <= 0x9FFF or cp == 0x00B0 or cp == 0x2103


def scan_sources():
    """扫 Arduino 源码，注释也算"""
    found = set()
    bad = []
    for base, _dirs, names in os.walk(PROJ):
        for name in names:
            if not name.lower().endswith(SCAN_EXT) or name in SCAN_SKIP:
                continue
            path = os.path.join(base, name)
            try:
                text = open(path, encoding="utf-8").read()
            except UnicodeDecodeError:
                bad.append(path)
                continue
            found.update(ord(c) for c in text if wanted(ord(c)))
    for path in bad:
        print("    跳过非 UTF-8 文件: %s" % path)
    return found


def write_charset(codes):
    """写 tools/font_charset.txt，无 BOM、LF"""
    chars = "".join(chr(cp) for cp in sorted(codes))
    lines = [
        "#  字符集：Arduino 工程要显示的字，tools/gen_font.py 读它出点阵",
        "#",
        "#  (1) 本文件必须是无 BOM 的 UTF-8。行首 # 是注释，行内 # 之后也丢掉，",
        "#      剩下的字符按码点收集，空白忽略。",
        "#  (2) 只认三种：汉字 U+4E00~U+9FFF、° U+00B0、℃ U+2103。",
        "#      全角标点和别的符号不认，画出来是豆腐块，文案里一律用 ASCII 标点。",
        "#  (3) ° 和 ℃ 不管源码里有没有都留着：温度单位早晚要用。",
        "#",
        "#  本文件由 python tools/gen_font.py --scan 扫源码生成（连注释一起收），",
        "#  加字可以直接加在下面，或者改完源码再 --scan 一遍。",
        "#  共 %d 字，按码点升序：" % len(codes),
    ]
    for i in range(0, len(chars), 24):
        lines.append(chars[i:i + 24])
    with open(CHARSET_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_charset():
    """读字符集，返回升序码点"""
    if not os.path.exists(CHARSET_FILE):
        raise SystemExit("找不到 %s，先跑 --scan" % CHARSET_FILE)
    raw = set()
    with open(CHARSET_FILE, encoding="utf-8") as f:
        for line in f:
            for ch in line.split("#", 1)[0]:
                if not ch.isspace():
                    raw.add(ord(ch))
    codes = sorted(cp for cp in raw if wanted(cp))
    if not codes:
        raise SystemExit("字符集是空的")
    if len(codes) != len(raw):
        print("    丢掉 %d 个非汉字字符" % (len(raw) - len(codes)))
    return codes


def read_ascii_table():
    """抄参考工程的 ASCII 表，一个字节都不改"""
    text = open(SRC_ASCII, encoding="utf-8").read()
    # 第二维写的是宏 PANEL_FONT_ASCII_BYTES，不是数字，所以两个方括号都放宽
    m = re.search(
        r"uint8_t\s+panel_ascii_8x16\s*\[[^\]]*\]\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
        text,
        re.S,
    )
    if not m:
        raise SystemExit("没在 %s 里找到 panel_ascii_8x16" % SRC_ASCII)
    rows = []
    for body in re.findall(r"\{([^{}]*)\}", m.group(1)):
        vals = [int(v, 16) for v in re.findall(r"0[xX][0-9a-fA-F]+", body)]
        if len(vals) != 16:
            raise SystemExit("ASCII 表有一行不是 16 字节：%r" % body)
        rows.append(vals)
    if len(rows) != 95:
        raise SystemExit("ASCII 表不是 95 行，是 %d 行" % len(rows))
    return rows


def pack_cell(img, cols=CELL, rows=CELL):
    """逐列打包：先排完列再下一页，bit0 是最上面那行"""
    px = img.load()
    out = bytearray()
    for page in range((rows + 7) // 8):
        for x in range(cols):
            b = 0
            for r in range(8):
                y = page * 8 + r
                if y < rows and px[x, y] > 127:
                    b |= 1 << r
            out.append(b)
    return bytes(out)


def render_chars(codes):
    """一个字一个字渲染，量墨迹外框再居中放进 16x16，返回 (字形, 字号, 最大墨迹)"""
    if not os.path.exists(TTF):
        raise SystemExit("找不到字体 " + TTF)
    for em in EM_LIST:
        font = ImageFont.truetype(TTF, em)
        cells = {}
        max_w = max_h = 0
        for cp in codes:
            img = Image.new("L", (CANVAS, CANVAS), 0)
            d = ImageDraw.Draw(img)
            # 不抗锯齿，纯黑白，和 GDI+ 的 SingleBitPerPixelGridFit 对齐
            d.fontmode = "1"
            d.text((PAD, PAD), chr(cp), font=font, fill=255)
            mask = img.point(lambda v: 255 if v > 127 else 0)
            box = mask.getbbox()
            if box is None:
                print("    U+%04X %s 没渲染出墨迹" % (cp, chr(cp)))
                break
            w = box[2] - box[0]
            h = box[3] - box[1]
            if w > CELL or h > CELL:
                print("    U+%04X %s 墨迹 %dx%d 装不进 16x16 (em=%d)" % (cp, chr(cp), w, h, em))
                break
            max_w = max(max_w, w)
            max_h = max(max_h, h)
            cell = Image.new("L", (CELL, CELL), 0)
            # crop 已经把外框左上角当原点了，居中就是 (16-w)/2
            cell.paste(img.crop(box), ((CELL - w) // 2, (CELL - h) // 2))
            cells[cp] = pack_cell(cell)
        if len(cells) == len(codes):
            return cells, em, max_w, max_h
        print("    em=%d 不行，换小一号重来" % em)
    raise SystemExit("汉字渲染失败：没有哪个字号装得下所有字")


def build_header(ascii_rows, glyphs):
    """拼 panel_font_data.h"""
    out = [
        "// c\n",
        "// 点阵字模数据（自动生成，别手改）\n",
        "#pragma once\n",
        "#include <stdint.h>\n",
        "\n",
        "// 别处定义过就跳过\n",
        "#ifndef PANEL_GLYPH_T_DEFINED\n",
        "#define PANEL_GLYPH_T_DEFINED\n",
        "\n",
        "// 一个字模\n",
        "typedef struct {\n",
        "    uint32_t unicode;\n",
        "    // 先 page 再列\n",
        "    uint8_t data[32];\n",
        "} panel_glyph_t;\n",
        "#endif\n",
        "\n",
        "// ASCII 8x16，0x20~0x7E 共 95 字\n",
        "// 抄 IDF 的表，没重渲染\n",
        "static const uint8_t PANEL_ASCII_8X16[95][16] = {\n",
    ]
    for row in ascii_rows:
        out.append("    {" + ", ".join("0x%02X" % v for v in row) + "},\n")
    out.append("};\n")
    out.append("\n")
    out.append("// 汉字 16x16，按码点升序\n")
    out.append("// 每字 32 字节，能二分\n")
    out.append("static const panel_glyph_t PANEL_CN_GLYPHS[] = {\n")
    for cp, data in glyphs:
        out.append("    {0x%04X,\n" % cp)
        out.append("     {\n")
        out.append("      // %s\n" % chr(cp))
        for i in (0, 16):
            out.append("      " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",\n")
        out.append("     }},\n")
    out.append("};\n")
    out.append("\n")
    out.append("// 表里几个字\n")
    out.append("static const int PANEL_CN_GLYPH_COUNT = %d;\n" % len(glyphs))
    return "".join(out)


def self_check(text, want):
    """写完读回来数一遍：条数、32 字节、严格升序、别有空格子"""
    codes = []
    blank = []
    for cp_hex, body in re.findall(
        r"\{\s*0[xX]([0-9a-fA-F]{4})\s*,\s*\{(.*?)\}\s*\}", text, re.S
    ):
        vals = [int(v, 16) for v in re.findall(r"0[xX][0-9a-fA-F]{2}", body)]
        if len(vals) != 32:
            raise SystemExit("字形 0x%s 不是 32 字节" % cp_hex)
        if not any(vals):
            blank.append(cp_hex)
        codes.append(int(cp_hex, 16))
    if blank:
        raise SystemExit("这些字模全是 0，渲染坏了：%s" % " ".join(blank))
    if len(codes) != want:
        raise SystemExit("表里 %d 条，字符集 %d 条，对不上" % (len(codes), want))
    if codes != sorted(set(codes)):
        raise SystemExit("码点没有严格升序或有重复")
    rows = re.findall(r"^\s*\{0x[0-9A-F]{2}, ", text, re.M)
    if len(rows) != 95:
        raise SystemExit("ASCII 表写出来不是 95 行，是 %d 行" % len(rows))
    return len(codes)


def preview(glyphs):
    """打印点阵图案"""
    for cp, data in glyphs:
        print("    U+%04X %s" % (cp, chr(cp)))
        for y in range(16):
            line = "      "
            for x in range(16):
                line += "#" if (data[(y // 8) * 16 + x] >> (y % 8)) & 1 else "."
            print(line)


def main():
    ap = argparse.ArgumentParser(description="生成 ui/panel_font_data.h")
    ap.add_argument("--scan", action="store_true", help="先扫源码刷新 font_charset.txt")
    ap.add_argument("--preview", action="store_true", help="打印点阵图案")
    args = ap.parse_args()

    if args.scan:
        codes = sorted(scan_sources() | {ord(c) for c in EXTRA_SYMBOLS})
        if not codes:
            raise SystemExit("源码里一个汉字都没扫到")
        write_charset(codes)
        print("扫源码      : %d 字 -> %s" % (len(codes), CHARSET_FILE))

    codes = read_charset()
    ascii_rows = read_ascii_table()
    cells, em, max_w, max_h = render_chars(codes)
    glyphs = [(cp, cells[cp]) for cp in codes]
    text = build_header(ascii_rows, glyphs)

    os.makedirs(os.path.dirname(OUT_FILE), exist_ok=True)
    with open(OUT_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    n = self_check(open(OUT_FILE, encoding="utf-8").read(), len(codes))
    size = os.path.getsize(OUT_FILE)
    print("写出        : %s" % OUT_FILE)
    print("汉字        : %d 字（ASCII 95 字）" % n)
    print("字号        : em=%d，最大墨迹 %dx%d" % (em, max_w, max_h))
    print("文件大小    : %.1f KB" % (size / 1024.0))
    print("自检        : OK")

    if args.preview:
        preview(glyphs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
