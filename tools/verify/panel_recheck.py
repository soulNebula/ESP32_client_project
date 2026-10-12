# Python
# 重插线后复测
"""重插线之后的复测：先重跑初始化+自检，再做两次"灭/亮"对照。

顺序说明（为什么先 panel-retry 而不是先 disp-off）：
  · panel-retry 会完整重发 SWRESET/SLPOUT/MADCTL/DISPON 一整套。
    如果之前是"上电时序赶上了"这类偶发，重发一次就能救回来；
    如果接线真的不对，它也只是再失败一次，没有副作用。
  · 然后才用 disp-off/disp-on 做终判。disp-off 只发一条命令，
    最能说明"控制器到底收不收得到命令"。

用法：
    python tools/verify/panel_recheck.py            # COM33，每段 10 秒
    python tools/verify/panel_recheck.py COM33 15
"""
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial：用 ESP-IDF 自带的 python 跑本脚本", file=sys.stderr)
    raise

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM33"
HOLD = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
BAUD = 115200


def clean(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == "\x1b":
            while i < len(text) and text[i] not in "mK":
                i += 1
            i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def talk(ser, cmd, seconds):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    end = time.time() + seconds
    chunks = []
    while time.time() < end:
        data = ser.read(4096)
        if data:
            chunks.append(data)
    return clean(b"".join(chunks).decode("utf-8", errors="replace")).rstrip()


with serial.Serial() as ser:
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("=" * 60, flush=True)
    print("第 1 步：重跑屏幕初始化 + 自检（不需要你做什么，看屏就好）", flush=True)
    print("=" * 60, flush=True)
    print(talk(ser, "panel-retry", 12), flush=True)

    print("\n" + "=" * 60, flush=True)
    print("第 2 步：现在开始【灭 / 亮】对照 —— 全程盯住屏！", flush=True)
    print("=" * 60, flush=True)

    for k in (1, 2):
        print(f"\n------ 第 {k} 轮 ------", flush=True)
        print(f">>>>>> 【灭】屏幕现在应该整屏变白/变亮（{HOLD:.0f} 秒）", flush=True)
        print(talk(ser, "disp-off", HOLD), flush=True)
        print(f">>>>>> 【亮】屏幕现在应该恢复（{HOLD:.0f} 秒）", flush=True)
        print(talk(ser, "disp-on", HOLD), flush=True)

    print("\n" + "=" * 60, flush=True)
    print("第 3 步：颜色自检，每张停 1 秒（红→绿→蓝→白→黑→自检卡）", flush=True)
    print("=" * 60, flush=True)
    print(talk(ser, "screen-test 1000", 12), flush=True)

    print("\n" + "=" * 60, flush=True)
    print("结束。请回答：", flush=True)
    print("  A) 【灭】的时候屏幕有没有变白/变亮？", flush=True)
    print("  B) 颜色自检里有没有看到红/绿/蓝/白/黑依次出现？", flush=True)
    print("=" * 60, flush=True)
