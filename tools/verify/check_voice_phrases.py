#!/usr/bin/env python3
# Python
# 命令词表静态校验
# -*- coding: utf-8 -*-
"""
check_voice_phrases.py -- 静态校验 voice_esp_sr.c 的命令词表

为什么需要它
---------------------------------------------------------------------------
命令词表是"拼音字符串"驱动的：
  · 音节拼错一个字母 → MultiNet 解析失败 → 现场现象是「这句怎么喊都没反应」，
    而编译期毫无提示（esp_mn_commands_add 只在运行期返回错误）；
  · 漏掉一条指令 → 那个功能永远说不出话；
  · 一个 id 挂多句同义说法（需求①的 "开启自动模式" 等）→ 表变复杂，
    更容易出现"少个分号 / 多个分号 / 句式重复"这类低级错误。

本脚本把上面三类错误在【不接硬件、不烧固件】的前提下挡在提交之前：
  1. 语法：每个表项 { "中文", "拼音(;分隔)", CMD } 是否完整、枚举名是否存在于 voice.h；
  2. 覆盖：voice_cmd_t 里 VOICE_CMD_NONE 之后的每条指令是否都出现在表里，且只出现一次；
  3. 拼音：每个音节是否在普通话合法音节集合里（拼错的音节会被点出来，比如 "shou dong mo shir"）；
  4. 去重：同一指令下的多种说法不能完全重复（重复注册会让 MultiNet 内部短语表变脏）。

用法
---------------------------------------------------------------------------
  python tools/verify/check_voice_phrases.py            # 在工程根目录下跑
  python tools/verify/check_voice_phrases.py --verbose
退出码：0 = 全部通过；1 = 有问题（打印到 stdout，可直接贴给排错的人）。
"""

import argparse
import re
import sys
from pathlib import Path

# 本脚本在 tools/verify/ 下，工程根 = 上两级目录
ROOT = Path(__file__).resolve().parent.parent.parent
SR_C = ROOT / "components" / "BSP" / "VOICE" / "voice_esp_sr.c"
HDR = ROOT / "components" / "BSP" / "VOICE" / "voice.h"

# --- 普通话合法音节（无声调） -------------------------------------------------
# 与 MultiNet7 的拼音输入约定一致：小写、空格分隔、不带声调数字。
# 列表覆盖常用音节；若某天要加一个罕见音节而它不在表里，脚本会报"未知音节"，
# 这时请人工确认后把音节补进本集合 —— 宁可多一次人工确认，也不要放过拼写错误。
INITIALS = [
    "", "b", "p", "m", "f", "d", "t", "n", "l", "g", "k", "h",
    "j", "q", "x", "zh", "ch", "sh", "r", "z", "c", "s", "y", "w",
]
FINALS = [
    "a", "o", "e", "i", "u", "v", "ai", "ei", "ui", "ao", "ou", "iu", "ie", "ve", "er",
    "an", "en", "in", "un", "vn", "ang", "eng", "ing", "ong",
    "ia", "iao", "ian", "iang", "iong", "ua", "uo", "uai", "uan", "uang", "ue", "ueng",
]
VALID_SYLLABLES = {i + f for i in INITIALS for f in FINALS} | {
    # 整体认读 / 特殊拼写
    "zhi", "chi", "shi", "ri", "zi", "ci", "si", "yi", "wu", "yu",
    "ye", "yue", "yuan", "yin", "yun", "ying", "ya", "yo", "yao", "you", "yan", "yang", "yong",
    "wa", "wo", "wai", "wei", "wan", "wen", "wang", "weng",
    "n", "ng", "hm", "hng",
}


def parse_enum(header_text: str):
    """从 voice.h 的 voice_cmd_t 里取出枚举名（按声明顺序）。"""
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*voice_cmd_t\s*;", header_text, re.S)
    if not m:
        raise SystemExit("!! 在 voice.h 里找不到 voice_cmd_t 枚举定义")
    names = []
    for line in m.group(1).splitlines():
        line = line.split("/*")[0].split("//")[0].strip()
        if not line:
            continue
        name = line.split("=")[0].strip().rstrip(",")
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            names.append(name)
    return names


def parse_table(src_text: str):
    """从 voice_esp_sr.c 里取出 s_cmds[] 的表项：{ "中文", "拼音", CMD }。

    支持跨行的字符串拼接（需求① 的多句式就是分几行写的）。"""
    m = re.search(r"static\s+const\s+voice_sr_cmd_entry_t\s+s_cmds\[\]\s*=\s*\{(.*?)\n\};",
                  src_text, re.S)
    if not m:
        raise SystemExit("!! 在 voice_esp_sr.c 里找不到 s_cmds[] 定义")

    body = m.group(1)
    entries = []
    # 逐项匹配：{ 字符串 字符串 标识符 }，字符串可以有多段相邻拼接
    item_re = re.compile(
        r'\{\s*("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*,'
        r'\s*("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*,'
        r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}',
        re.S,
    )
    for mm in item_re.finditer(body):
        cn = join_c_strings(mm.group(1))
        pinyin = join_c_strings(mm.group(2))
        cmd = mm.group(3)
        line = body[:mm.start()].count("\n") + 1
        entries.append({"cn": cn, "pinyin": pinyin, "cmd": cmd, "line": line})
    return entries


def join_c_strings(chunk: str) -> str:
    """把 C 的相邻字符串字面量拼成一个字符串，并处理少量转义。"""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', chunk, re.S)
    s = "".join(parts)
    return s.replace('\\"', '"').replace("\\\\", "\\")


def split_phrases(pinyin_field: str):
    """按 ';' 切分句式，丢掉空段 —— 与 C 侧 voice_sr_nth_phrase() 同一套规则。"""
    return [p.strip() for p in pinyin_field.split(";") if p.strip()]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true", help="打印全部句式")
    args = ap.parse_args()

    if not SR_C.exists() or not HDR.exists():
        print(f"!! 找不到源码：{SR_C} / {HDR}")
        return 1

    src_text = SR_C.read_text(encoding="utf-8")
    enums = parse_enum(HDR.read_text(encoding="utf-8"))
    entries = parse_table(src_text)

    problems = []
    print("=" * 74)
    print("命令词表静态校验（tools/verify/check_voice_phrases.py）")
    print("=" * 74)
    print(f"源码：{SR_C.relative_to(ROOT)}")
    print(f"枚举：voice_cmd_t 共 {len(enums)} 项（含 VOICE_CMD_NONE 与 VOICE_CMD_MAX 哨兵）")
    print(f"表项：s_cmds[] 共 {len(entries)} 条")

    # ---- 1. 枚举名合法性 ----
    for e in entries:
        if e["cmd"] not in enums:
            problems.append(f"[{SR_C.name}:{e['line']}] 指令 {e['cmd']} 不在 voice_cmd_t 里（拼错？）")

    # ---- 2. 覆盖与唯一 ----
    expected = enums[enums.index("VOICE_CMD_NONE") + 1: enums.index("VOICE_CMD_MAX")] \
        if "VOICE_CMD_NONE" in enums and "VOICE_CMD_MAX" in enums else []
    got = [e["cmd"] for e in entries]
    for name in expected:
        if name not in got:
            problems.append(f"指令 {name} 在 s_cmds[] 里【没有】对应的命令词 —— 这条永远说不出来")
    for name in got:
        if got.count(name) > 1:
            problems.append(f"指令 {name} 在 s_cmds[] 里出现了 {got.count(name)} 次（应恰好 1 次）")
    for name in got:
        if name not in expected:
            problems.append(f"表里出现了不该有的指令 {name}（不在 VOICE_CMD_NONE..VOICE_CMD_MAX 之间）")

    # ---- 3. 拼音音节 ----
    total_phrases = 0
    for e in entries:
        phrases = split_phrases(e["pinyin"])
        if not phrases:
            problems.append(f"[{SR_C.name}:{e['line']}] {e['cmd']} 的拼音字段是空的")
            continue
        for ph in phrases:
            total_phrases += 1
            for syl in ph.split():
                if syl not in VALID_SYLLABLES:
                    problems.append(
                        f"[{SR_C.name}:{e['line']}] {e['cmd']} 的句式「{ph}」里有非法音节 '{syl}'"
                        f" —— MultiNet 会解析失败，这句喊不出来")
        # 同一指令下不要重复注册完全相同的句式
        if len(set(phrases)) != len(phrases):
            dup = sorted({p for p in phrases if phrases.count(p) > 1})
            problems.append(f"[{SR_C.name}:{e['line']}] {e['cmd']} 有重复句式 {dup}")
        # 句式本身不能带中文（中文进识别引擎是乱码）
        for ph in phrases:
            if any(ord(ch) > 127 for ch in ph):
                problems.append(f"[{SR_C.name}:{e['line']}] {e['cmd']} 的句式「{ph}」含非 ASCII 字符"
                                f" —— 必须写拼音，中文进识别引擎会被当乱码")

    print(f"句式：{total_phrases} 个口语句式（' ; ' 分隔后统计）")

    # ---- 4. 需求① 的句式护栏 ----
    mode_auto = next((e for e in entries if e["cmd"] == "VOICE_CMD_MODE_AUTO"), None)
    mode_manual = next((e for e in entries if e["cmd"] == "VOICE_CMD_MODE_MANUAL"), None)
    if mode_auto is None or mode_manual is None:
        problems.append("需求①：s_cmds[] 里缺少 VOICE_CMD_MODE_AUTO / VOICE_CMD_MODE_MANUAL")
    else:
        for e, want in ((mode_auto, "zi dong mo shi"), (mode_manual, "shou dong mo shi")):
            phrases = split_phrases(e["pinyin"])
            if want not in phrases:
                problems.append(f"需求①：{e['cmd']} 缺少基线句式「{want}」（文档原文的说法）")
            if len(phrases) < 2:
                problems.append(f"需求①：{e['cmd']} 只有 1 个句式，"
                                f"应至少再挂「开启/打开 + 模式」这类口语变体")
            has_kaiqi = any(p.startswith("kai qi ") for p in phrases)
            if not has_kaiqi:
                problems.append(f"需求①：{e['cmd']} 没有「开启…」句式（需求点名要支持这种说法）")

    # ---- 输出 ----
    if args.verbose:
        print("-" * 74)
        for e in entries:
            for i, ph in enumerate(split_phrases(e["pinyin"])):
                tag = "    " if i else "  → "
                print(f"{tag}{e['cn']:<10} {ph:<32} {e['cmd']}")

    print("-" * 74)
    if problems:
        print(f"发现 {len(problems)} 个问题：")
        for p in problems:
            print(f"  ✗ {p}")
        return 1

    print("全部通过：枚举覆盖完整、音节合法、无重复句式、需求① 的「开启…」口语已注册。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
