# Python
# 屏诊断命令驱动
"""
串口驱动一遍屏幕诊断命令，把结果抓回来。

为什么需要它：屏"亮着背光但画面不对"时，最快的判定不是猜寄存器，而是问硬件：
  · disp-off / disp-on  —— 只发 DISPOFF/DISPON。屏如果变白再恢复，
                            说明控制器【收到命令了】，问题在数据/窗口；
                            完全没反应，说明命令根本没进控制器（接线/时序）。
  · hw                  —— 打印各引脚实际电平和背光逻辑电平。
  · screen-test         —— 纯色轮播，肉眼对照。
  · panel-retry         —— 重跑初始化序列，改完接线不必重新烧录。

用法：
    python tools/verify/panel_diag.py                   # COM33
    python tools/verify/panel_diag.py COM33 6           # 指定串口 / 每条等待秒数
"""
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial：用 ESP-IDF 自带的 python 跑本脚本", file=sys.stderr)
    raise

# ★ 日志里有 "⚠" 这类字符，而中文 Windows 控制台默认是 GBK，直接 print 会
#   UnicodeEncodeError 把脚本打断（排查脚本自己先崩掉最气人）。强制 UTF-8 输出。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM33"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
BAUD = 115200

# (要发的命令, 发完等多久)
STEPS = [
    ("disp-off",    WAIT),
    ("disp-on",     WAIT),
    ("hw",          4.0),
    ("screen-test", 8.0),
]

with serial.Serial() as ser:
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False          # 不复位：我们要看的是"现在"这一刻的状态
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()

    for cmd, wait in STEPS:
        print(f"\n########## >>> {cmd} ##########", flush=True)
        ser.write((cmd + "\r\n").encode())
        ser.flush()
        end = time.time() + wait
        buf = []
        while time.time() < end:
            data = ser.read(8192)
            if data:
                buf.append(data)
        text = b"".join(buf).decode("utf-8", errors="replace")
        # 去掉 ANSI 颜色码，方便阅读
        out = []
        i = 0
        while i < len(text):
            if text[i] == "\x1b":
                while i < len(text) and text[i] not in "mK":
                    i += 1
                i += 1
                continue
            out.append(text[i])
            i += 1
        print("".join(out).rstrip(), flush=True)

print("\n########## done ##########")
