#!/usr/bin/env python3
# Python
# 按需求编号批量敲命令
# -*- coding: utf-8 -*-
"""
run_console_cmds.py -- 向 ESP32 串口调试台发一组命令并抓回响应（真机功能验收）

★ 这一组命令是按【需求编号】排的，跑一遍就能逐条核对：
    需求①  手动/自动模式切换、自动模式拒绝灯光
    需求④  单灯 / 全灯
    需求⑤  开门级联亮客厅灯（重点看是否被光控秒关）
    需求⑥  蜂鸣器静音锁
    需求⑦  语音链路（say = 注入一条语音指令，与真实语音走同一条下游）
    需求⑧  status 里会打印手机端要用的 base topic

用法:
    python tools/monitor/run_console_cmds.py <输出文件> [串口号] [每条命令等待秒数]

注意：打开串口时 DTR/RTS 都保持 False，避免一打开就复位板子
（复位了就得重新等它连 WiFi/MQTT，测试会慢且不稳定）。
"""
import sys
import time

import serial

out_path = sys.argv[1] if len(sys.argv) > 1 else "console_test.txt"
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM33"
WAIT = float(sys.argv[3]) if len(sys.argv) > 3 else 1.5

BAUD = 115200

# 顺序有意为之：
#   · 先 status 拿基线；
#   · help-voice 直接敲（不是 say help-voice —— 那是"注入一条叫 help-voice 的
#     语音指令"，模块里没这条命令词，只会回一句"未知指令短名"）；
#   · 切手动再点灯（自动模式下灯光会被拒绝，那是需求①的行为，最后单独验）；
#   · 关掉客厅灯再开门 —— 这样才能看出"开门把客厅灯点亮了"这个级联（需求⑤）；
#   · 最后切回自动并发一条灯光指令，验证需求①的拒绝路径。
CMDS = [
    "status",                       # 基线：设备/传感器/网络/模式/阈值
    "help-voice",                   # 列出全部语音指令（需求⑦的指令集）
    "mode manual",                  # 需求①：切手动，光控停用
    "on led_kitchen",               # 需求④：单灯
    "on led_bedroom",
    "off led_kitchen",
    "all on",                       # 需求④：全灯亮（只点 4 路灯，绝不开门）
    "off all",                      # 需求④：全灯灭
    "off led_living",               # 先把客厅灯关掉，好观察级联
    "open door",                    # 需求⑤：开门 → 应级联点亮客厅灯，并保持 30s
    "close door",                   # 关门（客厅灯不动）
    "buzzer mute",                  # 需求⑥：静音锁
    "buzzer on",                    # 解除静音锁
    "say query_status",             # 需求⑦：播报温湿度+天气（走完整语音链路）
    "say led_living_on",            # 需求⑦：语音开灯
    "mode auto",                    # 切回自动
    "on led_bath",                  # 需求①：自动模式下应【被拒绝】
    "status",                       # 收尾快照
]

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()

time.sleep(0.5)
ser.reset_input_buffer()

all_text = []

for cmd in CMDS:
    ser.write((cmd + "\r\n").encode("ascii"))
    ser.flush()
    time.sleep(WAIT)
    data = ser.read(65536)
    if data:
        txt = data.decode("utf-8", errors="replace")
        all_text.append(f"\n===== >>> {cmd}\n{txt}")

ser.close()

text = "".join(all_text)
with open(out_path, "w", encoding="utf-8") as f:
    f.write(text)

# ★ 用 ASCII 打印，避免 Windows 控制台按 GBK 解码 UTF-8 出乱码
print(f"sent {len(CMDS)} commands, captured {len(text)} chars -> {out_path}")
print("（结果文件是 UTF-8，用编辑器或本工程的 grep 工具查看）")
