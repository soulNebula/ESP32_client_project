#!/usr/bin/env python3
# Python
# 查 ESP_LOG 引号成对
# -*- coding: utf-8 -*-
"""
check_c_quotes.py -- 粗查 C 源文件里 ESP_LOG* 语句的双引号是否成对

用途：中文注释/字符串里很容易误嵌一个半角双引号，导致编译报
      "missing terminating \" character"。这个脚本能在编译之前先抓出来，
      尤其适合在批量改日志文案之后跑一遍。

用法：
    python tools/verify/check_c_quotes.py                      # 扫 components + main
    python tools/verify/check_c_quotes.py components/a/b.c ...  # 只扫指定文件
"""
import os
import re
import sys

# 本脚本在 tools/verify/ 下，工程根 = 上两级目录
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def targets():
    if len(sys.argv) > 1:
        return sys.argv[1:]
    found = []
    for base in (os.path.join(ROOT, "components"), os.path.join(ROOT, "main")):
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in filenames:
                if fn.endswith(".c") or fn.endswith(".h"):
                    found.append(os.path.join(dirpath, fn))
    return found


def main():
    bad_total = 0
    for path in targets():
        if not os.path.exists(path):
            continue
        try:
            lines = open(path, encoding="utf-8").read().splitlines()
        except UnicodeDecodeError:
            print("[SKIP] not utf-8: %s" % path)
            continue

        bad = []
        for i, line in enumerate(lines, 1):
            # 只关心含字符串的日志行；注释里的引号无所谓
            stripped = line.strip()
            if stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if "ESP_LOG" not in line and "snprintf" not in line and "strcmp" not in line:
                continue
            # 数一下未转义的双引号个数
            n = len(re.findall(r'(?<!\\)"', line))
            if n % 2 != 0:
                bad.append((i, stripped))

        if bad:
            bad_total += len(bad)
            print("[FAIL] %s" % os.path.relpath(path, ROOT))
            for i, l in bad:
                print("   LINE %d: %s" % (i, l[:140]))

    if bad_total:
        print("\n发现 %d 行引号不成对，编译前先修掉。" % bad_total)
        return 1
    print("[ OK ] 所有文件的日志/字符串双引号都成对")
    return 0


if __name__ == "__main__":
    sys.exit(main())
