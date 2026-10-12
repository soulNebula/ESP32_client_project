#!/usr/bin/env python3
# Python
# 逐路传感器接线自检
# -*- coding: utf-8 -*-
"""
check_sensors.py -- 逐路传感器的"接线自检"脚本

★ 这是接完线后最有用的一个工具：它循环敲 status 并把关键读数打成一张表，
  你一边看表一边"动那个传感器"，就能立刻判断那一路通没通：

    光敏电阻   → 用手遮住   → 光照 light_pct 必须明显变化
    温湿度     → 哈气 / 用手捂 → 温度或湿度必须变化
    雨滴       → 往感应面滴水 → 雨滴 rain_pct 必须上升、天气变 Raining!
    光照BH1750 → 遮住       → lux 必须明显变化

  数值【一动不动】= 那一路没接上 / 接错脚 / 模块没供电。
  （比"灯亮不亮"可靠得多：led 只反映输出，传感器才反映输入。）

用法:
    python tools/verify/check_sensors.py [秒数] [串口号]
    python tools/verify/check_sensors.py 40 COM33
"""
import os
import re
import sys
import time

import serial

if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
    print(__doc__.strip())
    sys.exit(0)

SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 40
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM33"
BAUD = 115200
PERIOD = 1.0

# 本脚本在 tools/verify/ 下，工程根 = 上两级目录。
# ★ 结果不再写进 tools/（那是脚本目录）：统一落在 logs/，与其它串口证据放一起。
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG_DIR = os.path.join(ROOT, "logs")
os.makedirs(LOG_DIR, exist_ok=True)
out_path = os.path.join(LOG_DIR, "sensor_watch.txt")

print("=" * 96)
print("逐路传感器接线自检 —— 现在去动那个传感器（遮光 / 哈气 / 滴水），看数值会不会变")
print("=" * 96)
print("  时间     温度           湿度           光照             雨滴/天气")
print("-" * 96)

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()
time.sleep(0.4)

rows = []
end = time.time() + SECONDS

while time.time() < end:
    ser.reset_input_buffer()
    ser.write(b"status\r\n")
    ser.flush()
    time.sleep(PERIOD)
    txt = ser.read(65536).decode("utf-8", errors="replace")
    txt = re.sub(r"\x1b\[[0-9;]*m", "", txt)

    t = re.search(r"温度\s+([\d.\-]+)C \(valid=(\d)\)\s+湿度\s+([\d.]+)%", txt)
    l = re.search(r"光照\s+([\d.]+)% \(([\d.]+)lux, valid=(\d)", txt)
    r = re.search(r"雨滴\s+([\d.]+)% 天气=(\S+) \(valid=(\d)", txt)

    ts = time.strftime("%H:%M:%S")

    if t:
        temp = f"{t.group(1)}C v{t.group(2)}"
        humi = f"{t.group(3)}% v{t.group(2)}"
    else:
        temp = humi = "?"
    light = f"{l.group(1)}% v{l.group(3)}" if l else "?"
    rain = f"{r.group(1)}% {r.group(2)} v{r.group(3)}" if r else "?"

    # 全部用 ASCII 输出，避免 Windows 控制台按 GBK 解码 UTF-8 出乱码
    row = f"  {ts}  {temp:<14} {humi:<14} {light:<16} {rain}"
    print(row, flush=True)
    rows.append(row)

ser.close()

open(out_path, "w", encoding="utf-8").write("\n".join(rows) + "\n")

print("-" * 96)
print("怎么判读：")
print("  温度/湿度 v0 或一直不变  -> 温湿度模块没接上（查 GPIO8/9 与 3V3）")
print("  光照 一直不动            -> 光敏电阻那路没接上（查 GPIO1 与分压电阻）")
print("  雨滴 v0                  -> 雨滴模块没接上（查 GPIO2 的 AO）")
print("  valid=1 但数值不动       -> 接了，只是你动它的时候变化不明显，再用力一点")
print(f"（原始结果已存 {out_path}）")
