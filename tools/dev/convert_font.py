#!/usr/bin/env python3
# Python
# 生成基础字库
# -*- coding: utf-8 -*-
"""
convert_font.py -- 把旧工程的 font_cn.c / font_cn_extra.c 合并成本工程的
                   components/ui/font_cn_data.c（干净 UTF-8 注释）

为什么需要它：
    旧工程的两个字库文件是 GBK 注释被当成 UTF-8 读进来的（乱码），
    虽然点阵数据本身完好，但注释已经无法阅读，直接抄过来会污染新工程。
    本脚本只【提取点阵数据】，重新写一份干净的文件，数据一个字节都不改。

点阵格式（与 panel_font.h 一致，逐列 1bpp）：
    · 每字节 = 同一列上连续 8 个像素，bit0 = 该列最上面一行
    · 先从左到右排完所有列，再排下一页（每页 8 行）
    · ASCII 8x16 : 8 列 × 2 页 = 16 字节
    · 中文 16x16 : 16 列 × 2 页 = 32 字节

用法：
    python tools/dev/convert_font.py                # 用下面的默认字库目录
    python tools/dev/convert_font.py <字库目录>     # 显式指定旧工程字库所在目录
"""

import os
import re
import sys

# 本脚本在 tools/dev/ 下，工程根 = 上两级目录
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# ★ 旧工程的原始字库（font_cn.c / font_cn_extra.c）是【参考工程的资产】，
#   不在本仓库里；需要重新生成基础表时把它的目录作为第一个参数传进来。
OLD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "components", "BSP", "FONT")
OUT = os.path.join(ROOT, "components", "ui", "font_cn_data.c")

# 只保留 UI 真正会用到的汉字，控制 Flash 占用。
# 如果想要全部字，把 KEEP_ONLY_USED 改成 False。
KEEP_ONLY_USED = False

USED = set(
    "温度湿度天气客厅厨房卧室浴室门开关全灯光照报警静音已切换到当前请说无雨下有多少"
    "自动手动模式高温正常检测中连接网络失败成功客厅厨房卧室浴室开关状态年月份日星期"
    "上下左右前后时间秒分小时点分度百分之一二三四五六七八九十零"
)


def parse_ascii(text):
    """提取 ASCII 点阵表；文件里没有就返回空字典（补充字文件里没有）"""
    ascii_tables = {}
    for name, width in (("font_cn_ascii6x8", 6), ("font_cn_ascii8x16", 16)):
        m = re.search(
            r"const\s+uint8_t\s+" + name + r"\s*\[[^\]]*\]\s*\[\d+\]\s*=\s*\{(.*?)\n\};",
            text,
            re.S,
        )
        if not m:
            continue
        rows = re.findall(r"\{([^{}]*)\}", m.group(1))
        data = []
        for r in rows:
            vals = [int(v, 16) for v in re.findall(r"0[xX][0-9a-fA-F]+", r)]
            if len(vals) != width:
                raise SystemExit("%s 有一行宽度不是 %d：%r" % (name, width, r))
            data.append(vals)
        ascii_tables[name] = data
    return ascii_tables


def parse_glyphs(text):
    """提取中文字形表；文件里没有就返回空字典"""
    glyphs = {}
    for name in ("font_cn_base", "font_cn_extra"):
        # 注意：条目里也有 '}'，所以不能用 \{...\n\}; 这种非贪婪匹配，
        # 必须用「行首的 };」作为结束锚点。
        m = re.search(
            r"const\s+font_cn_glyph_t\s+" + name + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)^\};",
            text,
            re.S | re.M,
        )
        if not m:
            continue
        entries = re.findall(
            r"\{\s*0[xX]([0-9a-fA-F]+)\s*,\s*\{(.*?)\}\s*\}", m.group(1), re.S
        )
        for cp_hex, body in entries:
            vals = [int(v, 16) for v in re.findall(r"0[xX][0-9a-fA-F]+", body)]
            if len(vals) != 32:
                raise SystemExit("字形 0x%s 不是 32 字节" % cp_hex)
            glyphs[int(cp_hex, 16)] = vals
    return glyphs


def fmt_bytes(vals, per_line, indent):
    out = []
    for i in range(0, len(vals), per_line):
        chunk = ", ".join("0x%02X" % v for v in vals[i:i + per_line])
        out.append(indent + chunk + ",")
    return "\n".join(out)


HEADER = '''// 点阵字库数据：ASCII 8x16 + 中文 16x16，逐列式 1bpp
// 黑体 17pt 取模；tools/dev/convert_font.py 只重排版、不改字节，不要手改
// 每字节是一列的 8 个像素，bit0 在最上面；先排完列，再排下一页

#include "panel_font.h"
#include <stddef.h>

// ASCII 8x16，0x20~0x7E 共 95 个字，每个 16 字节
// 1 bit 只表示画不画：渲染时 bit=1 涂前景、bit=0 涂背景，单色屏彩色屏通用
const uint8_t panel_ascii_8x16[95][PANEL_FONT_ASCII_BYTES] = {
'''

FOOTER_TMPL = '''
// 中文字形表：按 Unicode 码点升序，便于二分查找
// 共 %d 个字
const panel_glyph_t panel_cn_glyphs[] = {
%s
};

const int panel_cn_glyph_count = %d;

// 查表函数（panel_font_cn_lookup / panel_font_ascii_lookup）在 panel_font.c 里，
// 本文件只放数据 —— 别再往这里生成一份，否则链接期重复定义
'''


def main():
    src_base = os.path.join(OLD, "font_cn.c")
    src_extra = os.path.join(OLD, "font_cn_extra.c")
    for p in (src_base, src_extra):
        if not os.path.exists(p):
            raise SystemExit("找不到 " + p)

    ascii_tables = parse_ascii(open(src_base, encoding="utf-8").read())
    glyphs = parse_glyphs(open(src_base, encoding="utf-8").read())
    glyphs.update(parse_glyphs(open(src_extra, encoding="utf-8").read()))
    if not ascii_tables.get("font_cn_ascii8x16"):
        raise SystemExit("基础字库里没有找到 font_cn_ascii8x16")

    if KEEP_ONLY_USED:
        glyphs = {cp: d for cp, d in glyphs.items() if chr(cp) in USED}

    ordered = sorted(glyphs.items())

    parts = [HEADER]
    for idx, row in enumerate(ascii_tables["font_cn_ascii8x16"]):
        ch = chr(0x20 + idx)
        if ch in ("*", "/"):
            ch = "0x%02X" % (0x20 + idx)
        parts.append("    { " + ", ".join("0x%02X" % v for v in row) +
                     " },  /* %s */\n" % ch)
    parts.append("};\n")

    body = []
    for cp, data in ordered:
        ch = chr(cp)
        label = ch if ch.isprintable() and cp > 0x7F else ("U+%04X" % cp)
        body.append("    { 0x%04X, {  /* %s */\n%s\n    } },\n"
                    % (cp, label, fmt_bytes(data, 16, "        ")))

    parts.append(FOOTER_TMPL % (len(ordered), "".join(body), len(ordered)))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))

    print("OK  写出 %s" % OUT)
    print("    中文 %d 字，ASCII 95 字" % len(ordered))
    print("    文件大小 %.1f KB" % (os.path.getsize(OUT) / 1024.0))


if __name__ == "__main__":
    sys.exit(main())
