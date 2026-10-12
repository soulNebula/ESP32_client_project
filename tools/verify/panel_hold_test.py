# Python
# 灭亮交替，人眼判屏
"""
屏幕状态判定：用"灭 8 秒 / 亮 8 秒"来回切，给肉眼足够时间分辨。

为什么要拉长节奏：DISPOFF 只改控制器的显示开关，效果是"整屏变白/变亮"，
和"蓝屏/灰屏"的差别很细微，400ms 一闪根本看不清。拉长到 8 秒，
"屏幕有没有跟着我报的时间点在变"就一目了然了。

判读（脚本会同步打印 [T=..] 标记，照着看屏）：
  · 屏跟着 [灭] / [亮] 明显变化   → 控制器【收到命令】，接线 OK。
                                     问题在数据/窗口几何（偏移、反相）。
  · 屏一直纹丝不动（一直蓝/灰）   → 命令根本没进控制器。
                                     查 DC / CS / SCL / SDA 四根线。

用法：
    python tools/verify/panel_hold_test.py                # COM33，每段 8 秒
    python tools/verify/panel_hold_test.py COM33 10       # 每段 10 秒
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
HOLD = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
BAUD = 115200


def drain(ser, seconds):
    """把串口内容读出来并实时打印（去掉 ANSI 颜色码）。"""
    end = time.time() + seconds
    raw = []
    while time.time() < end:
        data = ser.read(4096)
        if not data:
            continue
        raw.append(data)
        text = data.decode("utf-8", errors="replace")
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


with serial.Serial() as ser:
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()

    SEG = 2
    print(f"开始：每段 {HOLD:.0f} 秒，共 {SEG} 轮（灭→亮）。COM33，{BAUD} 8N1\n", flush=True)

    for k in range(1, SEG + 1):
        print(f"===== 第 {k} 轮 =====", flush=True)

        print(f">>>>>> 【灭】现在屏幕应该整屏变白/变亮 —— 盯住屏！({HOLD:.0f}s)", flush=True)
        ser.write(b"disp-off\r\n")
        ser.flush()
        print(drain(ser, HOLD).rstrip(), flush=True)

        print(f"\n>>>>> 【亮】现在应该恢复原来的显示状态 ({HOLD:.0f}s)", flush=True)
        ser.write(b"disp-on\r\n")
        ser.flush()
        print(drain(ser, HOLD).rstrip(), flush=True)
        print("", flush=True)

    print("===== 最后跑一遍颜色自检（红→绿→蓝→白→黑→自检卡） =====", flush=True)
    ser.write(b"screen-test 900\r\n")
    ser.flush()
    print(drain(ser, 12).rstrip(), flush=True)

    print("\n===== 结束 =====", flush=True)
    print("请告诉我：哪几个时间点屏幕【确实变了】，哪几个【完全没反应】。", flush=True)
