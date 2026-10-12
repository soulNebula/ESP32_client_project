# Python
# 屏排版离线预览
# -*- coding: utf-8 -*-
"""
离线渲染需求③的屏幕效果预览 —— 用【真实字库点阵】复刻 display_view.c 的排版。

为什么值得做：屏上的排版宽度全是"手算 + 编译期断言"，肉眼看代码很难发现
"文字被裁掉"或"两块压在一起"。这里用 Python 把 panel.c 的字模按同一套
坐标摆一遍，任何越界都会直接在图上看出来（越界的像素会被丢出画布）。

数据来源（只读，不修改任何源文件）：
  components/ui/font_cn_data.c   panel_cn_glyphs[]   (基础表)
  components/ui/font_cn_extra.c  panel_cn_extra[]    (补充表)
  components/ui/panel_font.c     ASCII 表 panel_ascii_8x16
"""
import os
import re

# 缺 Pillow 时说人话
try:
    from PIL import Image, ImageDraw
except ImportError:
    print('[FAIL] 缺 Pillow：请用系统 Python 跑，别在 ESP-IDF 终端里跑')
    print('       （IDF 自带的 Python 环境没装 Pillow）')
    print('       装的话：pip install pillow')
    raise SystemExit(2)

# ★ 路径不写死：本脚本就放在 tools\verify\ 下，仓库根 = 上两级目录。
#   写死绝对路径的话，仓库一改名/一挪目录，脚本就静默读不到字库（表现为"字全丢"）。
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "preview_oled.png")

SCR_W, SCR_H = 128, 64
CN_W = CN_H = 16
ASCII_W, ASCII_H = 8, 16


def load_cn_table(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    table = {}
    for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{4,6})\s*,\s*\{([^}]*)\}\s*\}", src):
        cp = int(m.group(1), 16)
        nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
        if len(nums) >= 32:
            table[cp] = nums[:32]
    return table


def load_ascii_table():
    """ASCII 8x16 表：只要按顺序取 panel_ascii_8x16 声明之后的 95 个 '{...}' 块。
    ⚠ 不要依赖"以 }; 结尾"的写法 —— 这张表的收尾 '};' 和最后一个字形在同一行，
    用 `\\n};` 去匹配会静默失败（结果是 ASCII 一个都不画，预览图上只剩汉字）。"""
    path = os.path.join(ROOT, "components/ui/font_cn_data.c")
    src = open(path, encoding="utf-8", errors="replace").read()
    i = src.index("panel_ascii_8x16")
    i = src.index("{", src.index("=", i))
    table = []
    for m in re.finditer(r"\{([^{}]*)\}", src[i:]):
        nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
        if len(nums) == 16:
            table.append(nums)
        if len(table) == 95:
            break
    return table or None


CN = {}
CN.update(load_cn_table(os.path.join(ROOT, "components/ui/font_cn_data.c")))
CN.update(load_cn_table(os.path.join(ROOT, "components/ui/font_cn_extra.c")))
ASCII_TAB = load_ascii_table()


class Frame(object):
    """1bpp 显存 + 与 panel.c 一致的绘制原语（超出画布 = 裁掉，用来暴露越界）"""

    def __init__(self):
        self.px = [[0] * SCR_W for _ in range(SCR_H)]
        self.clipped = 0

    def set(self, x, y, v):
        if 0 <= x < SCR_W and 0 <= y < SCR_H:
            self.px[y][x] = v
        else:
            self.clipped += 1

    def fill(self, x, y, w, h, v):
        for py in range(y, y + h):
            for px in range(x, x + w):
                self.set(px, py, v)

    def ascii(self, x, y, ch, color=1):
        o = ord(ch)
        if ASCII_TAB is None or o < 0x20 or o > 0x7E:
            return
        g = ASCII_TAB[o - 0x20]
        for c in range(ASCII_W):
            for b in range(8):
                if g[c] & (1 << b):
                    self.set(x + c, y + b, color)

    def cn(self, x, y, cp, color=1):
        g = CN.get(cp)
        if g is None:
            for c in range(CN_W):
                for p in range(CN_H):
                    if p in (0, CN_H - 1) or c in (0, CN_W - 1):
                        self.set(x + c, y + p, color)
            return
        for c in range(CN_W):
            for half in range(2):
                bits = g[c + half * 16]
                for b in range(8):
                    if bits & (1 << b):
                        self.set(x + c, y + half * 8 + b, color)

    def text(self, x, y, s, color=1):
        """中英混排：ASCII 8px / 汉字 16px（与 panel_draw_text_cn 一致）"""
        for ch in s:
            if ord(ch) < 0x80:
                self.ascii(x, y, ch, color)
                x += ASCII_W
            else:
                self.cn(x, y, ord(ch), color)
                x += CN_W
        return x


def text_width(s):
    return sum(ASCII_W if ord(c) < 0x80 else CN_W for c in s)


def render(raining, door_open, mode_auto, temp="29.3C", hum="60%", light="L32%",
           wifi="W4", lights=(True, False, False, False)):
    f = Frame()

    # ---- 顶栏 y=0：模式反白块(0..33) + 门(36..67) + 网络档位(右对齐) ----
    f.fill(0, 0, 34, 16, 1)
    f.text(1, 0, "自动" if mode_auto else "手动", color=0)      # 反白 = 白底黑字
    dx = 34 + 2
    if door_open:
        f.fill(dx - 1, 0, 34, 16, 1)
        f.text(dx, 0, "门开", color=0)
    else:
        f.text(dx, 0, "门关")
    f.text(SCR_W - text_width(wifi), 0, wifi)

    # ---- 天气行 y=16（需求③，独占整行） ----
    txt = "天气: " + ("Raining!" if raining else "Sunny")
    x = (SCR_W - text_width(txt)) // 2
    if raining:
        f.fill(0, 16, SCR_W, 16, 1)
        f.text(x, 16, txt, color=0)
    else:
        f.text(x, 16, txt)

    # ---- 传感器行 y=32：温度 + 光照 + 湿度 ----
    f.text(0, 32, temp)
    hw = text_width(" " + hum)
    hx = SCR_W - hw
    lw = text_width(light)
    lx = max(text_width(temp) + 8, hx - 8 - lw)
    f.text(lx, 32, light)
    f.text(hx, 32, " " + hum)

    # ---- 四路灯行 y=48：4 格 × 32px，亮的整格反白 ----
    cell = SCR_W // 4
    for i, (room, on) in enumerate(zip(("客", "厨", "卧", "浴"), lights)):
        x = i * cell
        if on:
            f.fill(x, 48, cell, 16, 1)
            f.text(x + 1, 48, room, color=0)
            f.text(x + 17, 48, "开", color=0)
        else:
            f.text(x + 1, 48, room)
            f.text(x + 17, 48, "关")
    return f


def to_image(f, scale=6):
    img = Image.new("L", (SCR_W * scale, SCR_H * scale), 0)
    d = ImageDraw.Draw(img)
    for y in range(SCR_H):
        for x in range(SCR_W):
            if f.px[y][x]:
                d.rectangle([x * scale, y * scale, (x + 1) * scale - 1, (y + 1) * scale - 1],
                            fill=255)
    return img


def main():
    scenes = [
        ("(a) weather OFF  ->  天气: Sunny", dict(raining=False, door_open=False, mode_auto=True)),
        ("(b) weather ON   ->  天气: Raining! (whole row inverted)", dict(raining=True, door_open=False, mode_auto=True)),
        ("(c) ON + door open + manual mode", dict(raining=True, door_open=True, mode_auto=False)),
    ]
    # ★ 倍率压到 3：一屏 128x64 -> 384x192，三帧 + 标注正好一张图能看全，
    #   不需要在查看器里缩放（倍率太大时查看器会按比例只显示局部，反而看不出排版关系）。
    scale, gap = 3, 16
    row_tags = ["row0 mode/door/net", "row1 weather", "row2 temp/hum/light", "row3 4 lights"]
    imgs = [to_image(render(**kw), scale) for _, kw in scenes]
    W = imgs[0].width + 20 + 90          # 右侧留给行标注
    H = gap + sum(im.height + gap + 14 for im in imgs)
    canvas = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(canvas)
    y = gap
    for (title, _), im in zip(scenes, imgs):
        d.text((10, y - 13), title, fill=200)
        canvas.paste(im, (10, y))
        for i, tag in enumerate(row_tags):
            d.text((im.width + 16, y + i * 16 * scale + 16 * scale // 2 - 5), tag, fill=150)
        y += im.height + gap + 14
    canvas.save(OUT)
    print("saved:", OUT, canvas.size)
    print("cn glyphs loaded:", len(CN), "| ascii table:", "ok" if ASCII_TAB else "MISSING")
    for s in ("天气: Sunny", "天气: Raining!"):
        print("width(%r) = %d px  (screen = %d)" % (s, text_width(s), SCR_W))
    for (name, _), _im in zip(scenes, imgs):
        f = render(**{k: v for k, v in dict(
            raining=("Raining" in name), door_open=("door open" in name),
            mode_auto=("manual" not in name)).items()})
        print("clipped pixels in %-16s : %d" % (name.split()[0], f.clipped))
    # 逐行包围盒：这是比肉眼更可靠的验收（越界/压字都会在这里暴露）
    for title, kw in scenes[:2]:
        f = render(**kw)
        print("--- %s  clipped=%d" % (title.split()[0], f.clipped))
        for y0, y1, tag in ((0, 15, "topbar"), (16, 31, "weather"),
                            (32, 47, "sensors"), (48, 63, "lights")):
            xs = [x for y in range(y0, y1 + 1) for x in range(SCR_W) if f.px[y][x]]
            print("    %-8s lit_x = [%s..%s]" % (tag, min(xs) if xs else "-", max(xs) if xs else "-"))


if __name__ == "__main__":
    main()
