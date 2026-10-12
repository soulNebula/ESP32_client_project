# Python
# 抓完整开机日志
"""
抓取 ESP32 的串口启动日志，用于验证固件在真机上的启动行为。

为什么用 Python 而不是 idf.py monitor：
  · idf.py monitor 会一直阻塞，需要在别的进程里发 Ctrl-] 才能退出；
  · 本脚本自己控制"复位 → 读 N 秒 → 退出"，适合无人值守地跑一次。

★ 关键点：打开串口时把 DTR/RTS 置成"不复位"的电平。
  否则打开串口的瞬间会拉低 EN 脚把板子重启，而那时我们还来不及开始读，
  就会漏掉最前面的启动日志（而那几行恰恰是烧录成功与否的第一现场）。

用法：
    python tools/monitor/capture_boot.py                      # COM33，读 50 秒
    python tools/monitor/capture_boot.py out.txt              # 指定输出文件
    python tools/monitor/capture_boot.py out.txt reset        # 先手动复位再读（抓完整开机流程）
    python tools/monitor/capture_boot.py out.txt reset COM5 30
参数顺序：输出文件、是否先复位、串口号、读取秒数
"""
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial：用 ESP-IDF 自带的 python 跑本脚本", file=sys.stderr)
    raise

out_path = sys.argv[1] if len(sys.argv) > 1 else "boot_log.txt"
reset_first = (len(sys.argv) > 2 and sys.argv[2] == "reset")
PORT = sys.argv[3] if len(sys.argv) > 3 else "COM33"
SECONDS = int(sys.argv[4]) if len(sys.argv) > 4 else 50
BAUD = 115200


def capture(seconds, path):
    # DTR/RTS 都置 False = 不拉低 EN / 不进下载模式 → 打开串口不会让板子复位
    with serial.Serial() as ser:
        ser.port = PORT
        ser.baudrate = BAUD
        ser.timeout = 1
        ser.dtr = False
        ser.rts = False
        ser.open()

        if reset_first:
            # 手动复位一次，好抓完整的启动过程
            ser.setDTR(False)
            ser.setRTS(True)      # EN 拉低 = 复位
            time.sleep(0.15)
            ser.setRTS(False)     # 松开 = 运行
            time.sleep(0.05)

        end = time.time() + seconds
        chunks = []
        while time.time() < end:
            data = ser.read(4096)
            if data:
                chunks.append(data)

    # 按 UTF-8 解码，坏字节用替换符而不是抛异常 —— 中文 SSID 可能被串口截断
    raw = b"".join(chunks)
    text = raw.decode("utf-8", errors="replace")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return text


print(f"capturing {SECONDS}s from {PORT} (reset_first={reset_first}) ...", flush=True)
text = capture(SECONDS, out_path)
print(f"captured {len(text)} chars -> {out_path}", flush=True)
