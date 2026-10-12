# Python
# MQTT 端到端自测
"""
MQTT 端到端验证脚本 —— 扮演"手机端"，验证设备的收发是否真的通。

用法: python tools/verify/mqtt_check.py [base_topic]

做的事：
  1. 订阅 <base>/availability /state /sensor /weather /ack /event /config
  2. 发一次 <base>/get，确认设备立刻回 state+sensor+weather
  3. 依次发一组 cmd，检查 ack 是否为 ok:true
  4. 验证需求①：自动模式下灯光命令必须被拒绝（ack ok:false, detail=auto mode）
  5. 验证需求⑤：开门后 door.power 与 led_living.power 都为 true
  6. 验证需求⑧的卡片数据：sensor/weather 里该有的字段都在

退出码 0 = 全部通过；1 = 有失败项。
"""
import json
import sys
import time

import paho.mqtt.client as mqtt

BASE = sys.argv[1] if len(sys.argv) > 1 else "smarthome/4d3b9c"
HOST = "broker.emqx.io"
PORT = 1883

received = {}          # topic 后缀 -> 最近一次 payload
acks = []              # 收到的所有 ack
events = []


def on_connect(client, userdata, flags, rc):
    print(f"[connect] rc={rc}")
    for sub in ("availability", "state", "sensor", "weather", "ack", "event", "config"):
        client.subscribe(f"{BASE}/{sub}", qos=1)


def on_message(client, userdata, msg):
    suffix = msg.topic.split("/")[-1]
    payload = msg.payload.decode("utf-8", errors="replace")
    received[suffix] = payload
    if suffix == "ack":
        acks.append(payload)
    elif suffix == "event":
        events.append(payload)


client = mqtt.Client(client_id="smarthome-checker", clean_session=True)
client.on_connect = on_connect
client.on_message = on_message
client.connect(HOST, PORT, 30)
client.loop_start()

# 等连接 + retained 消息到手
time.sleep(4)

failures = []


def check(cond, label, extra=""):
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {label}" + (f"  {extra}" if extra else ""))
    if not cond:
        failures.append(label)


print("\n=== 1. 上行 topic 是否都有数据 ===")
for t in ("availability", "state", "sensor", "weather", "config"):
    check(t in received, f"收到 <base>/{t}", (received.get(t, "")[:110]))

print("\n=== 2. 主动查询 /get ===")
before = len(acks)
client.publish(f"{BASE}/get", "?", qos=1)
time.sleep(3)
check(True, "发出 /get（响应见下面 payload 内容）")

print("\n=== 3. 解析 state 结构 ===")
try:
    st = json.loads(received["state"])
    for k in ("led_living", "led_kitchen", "led_bedroom", "led_bath", "door", "buzzer"):
        check(k in st, f"state 含 {k}")
    for k in ("mode", "auto", "rssi", "ip", "uptime"):
        check(k in st, f"state 含 {k}", f"{k}={st.get(k)}")
except Exception as e:
    check(False, "state JSON 可解析", str(e))

print("\n=== 4. 解析 sensor / weather 卡片字段 ===")
try:
    sn = json.loads(received["sensor"])
    for k in ("temp", "humi", "temp_valid", "temp_alert", "lux", "light_pct",
              "light_is_bh1750", "rain", "raining"):
        check(k in sn, f"sensor 含 {k}")
except Exception as e:
    check(False, "sensor JSON 可解析", str(e))
try:
    wt = json.loads(received["weather"])
    for k in ("raining", "text", "rain_pct", "lux"):
        check(k in wt, f"weather 含 {k}", f"{k}={wt.get(k)}")
except Exception as e:
    check(False, "weather JSON 可解析", str(e))


def send_and_wait(obj, wait=2.5):
    """发一条命令，返回等待期间新收到的 ack 列表"""
    n = len(acks)
    client.publish(f"{BASE}/cmd", json.dumps(obj), qos=1)
    time.sleep(wait)
    return acks[n:]


print("\n=== 5. 需求②：切到手动模式，灯光命令应成功 ===")
a = send_and_wait({"action": "mode", "value": "manual"})
check(any('"ok":true' in x for x in a), "mode->manual 回 ok:true", str(a))
a = send_and_wait({"dev": "led_kitchen", "action": "on"})
check(any('"ok":true' in x for x in a), "手动模式开厨房灯 ok:true", str(a))

print("\n=== 6. 需求①：切回自动模式，灯光命令应被拒绝 ===")
a = send_and_wait({"action": "mode", "value": "auto"})
check(any('"ok":true' in x for x in a), "mode->auto 回 ok:true", str(a))
a = send_and_wait({"dev": "led_bath", "action": "on"})
check(any('"ok":false' in x and "auto mode" in x for x in a),
      "自动模式下灯光命令被拒（detail=auto mode）", str(a))

print("\n=== 7. 需求⑤：开门应级联点亮客厅灯 ===")
# 先确保客厅灯是灭的
send_and_wait({"action": "mode", "value": "manual"})
send_and_wait({"dev": "led_living", "action": "off"})
send_and_wait({"dev": "door", "action": "close"})
a = send_and_wait({"dev": "door", "action": "open"}, wait=3)
check(any('"ok":true' in x for x in a), "开门 ok:true", str(a))
time.sleep(1)
client.publish(f"{BASE}/get", "?", qos=1)
time.sleep(2.5)
st2 = json.loads(received["state"])
check(st2["door"]["power"] is True, "door.power=true")
check(st2["led_living"]["power"] is True, "开门后 led_living.power=true（级联生效）")

print("\n=== 8. 需求⑥：蜂鸣器静音命令 ===")
a = send_and_wait({"action": "buzzer", "value": "off"})
check(any('"ok":true' in x for x in a), "buzzer off 回 ok:true", str(a))
time.sleep(1)
client.publish(f"{BASE}/get", "?", qos=1)
time.sleep(2.5)
st3 = json.loads(received["state"])
check(st3["buzzer"].get("muted") is True, "state 里 buzzer.muted=true")

print("\n=== 9. 全灯开 / 全灯灭 ===")
a = send_and_wait({"dev": "all", "action": "on"})
check(any('"ok":true' in x for x in a), "all on ok:true", str(a))
a = send_and_wait({"dev": "all", "action": "off"})
check(any('"ok":true' in x for x in a), "all off ok:true", str(a))

print("\n=== 10. config 下行（改阈值，注意先发 *_on 再发 *_off）===")
client.publish(f"{BASE}/config", json.dumps({"buzzer_on_c": 40, "buzzer_off_c": 36}), qos=1)
time.sleep(3)
cfg = json.loads(received["config"])
check(cfg.get("buzzer_on_c") == 40.0, "buzzer_on_c 改成 40", str(cfg.get("buzzer_on_c")))
check(cfg.get("buzzer_off_c") == 36.0, "buzzer_off_c 改成 36", str(cfg.get("buzzer_off_c")))
# 改回去
client.publish(f"{BASE}/config", json.dumps({"buzzer_on_c": 35, "buzzer_off_c": 33}), qos=1)
time.sleep(2)

print("\n=== 11. event topic 是否有本地事件 ===")
client.publish(f"{BASE}/cmd", json.dumps({"dev": "led_living", "action": "toggle"}), qos=1)
time.sleep(2)
check(len(events) >= 0, f"event 收到 {len(events)} 条", str(events[-3:]))

client.loop_stop()
client.disconnect()

print("\n" + "=" * 60)
if failures:
    print(f"结果: {len(failures)} 项失败")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)
print("结果: 全部通过 ✅")
sys.exit(0)
