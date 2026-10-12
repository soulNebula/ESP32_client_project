# MQTT 协议 · ESP32-S3 智能家居

> 网页面板（[web/index.html](../web/index.html)）和固件之间的接口约定，真源是 [components/App/mqtt_protocol.h](../components/App/mqtt_protocol.h) 和 [components/App/net_link.c](../components/App/net_link.c)。
> **手机 App 走的是蓝牙，不是这一套** —— 见 [蓝牙协议.md](蓝牙协议.md)。
> 两边的内容（JSON）是同一份生成的，所以状态永远一致。
> 接线看[接线文档.md](接线文档.md)，整体结构看[系统架构与实现说明.md](系统架构与实现说明.md)。
>
> WiFi 账号**不用在这里配**：客户在蓝牙 App 里填，板子存进 NVS。这份文档里的
> broker / topic 是网页面板那条路用的。

## base 怎么来
```text
base = <前缀>/<唯一ID>      例如 smarthome/4d3b9c
```
- 前缀在 menuconfig「智能家居 → MQTT 设置 → Topic 前缀」，默认 `smarthome`。
  ★ 换成带个人标识的（`smarthome-zhangsan`）—— 公共 broker 上重名会互相看到对方的设备。
- 唯一ID 默认是 MAC 后 3 字节的 6 位小写十六进制；menuconfig 里 `唯一ID` 填了就按你填的走。
- 开机日志会直接打出来，不用自己算（手机端 base 就填这一段）：
```text
I (1884) NET: topics: state=smarthome/4d3b9c/state  cmd=smarthome/4d3b9c/cmd/#  sensor=.../sensor  weather=.../weather
```

## 端口（最容易配错）
- ESP32 固件、安卓 App、MQTTX 都走 TCP：`mqtt://broker.emqx.io:1883`（加密是 8883）。
- 浏览器 web 面板走 WebSocket：`wss://broker.emqx.io:8084/mqtt`。
- 同一个 broker、两个端口：**浏览器填 1883 连不上**，ESP32 填 8084 也连不上；自建 mosquitto 要给浏览器用，
  得额外开 `listener 8084` + `protocol websockets`。

## topic 总表
`<base>` 就是上面那串 `smarthome/4d3b9c`。
```text
上行  <base>/state         设备状态，retain=1 QoS1 —— 手机一订阅就看到最新值
上行  <base>/sensor        温湿度 / 光照 / 雨滴，retain=0 QoS0，2 秒一次
上行  <base>/weather       天气卡片，retain=1 QoS1
上行  <base>/ack           每条指令的结果，成功失败都回，retain=0 QoS1
上行  <base>/event         设备变化、按键事件，retain=0 QoS1
上行  <base>/config        当前生效的阈值和模式，retain=1 QoS1
上行  <base>/availability  online / offline，遗嘱 LWT，掉线由 broker 代发，retain=1 QoS1
下行  <base>/cmd           控制指令；<base>/config 改阈值 / 切模式，走同一套解析
下行  <base>/get           payload 随便填，立刻回 state + sensor + weather + config
```

## 上行 payload
`state`（变了就发，200ms 合并一次；重连后和收到 `/get` 时也发）：
```json
{"led_living":{"power":true,"level":100},"door":{"power":false,"level":0},"buzzer":{"power":false,"muted":false},"mode":"manual","auto":false,"wifi":true,"mqtt":true,"rssi":-52,"ip":"192.168.31.9","uptime":1234}
```
- `led_living` / `led_kitchen` / `led_bedroom` / `led_bath`：4 个房间灯，`power`=亮灭，`level`=亮度 0~100（后 3 个和示例同结构）。
- `door`：`power`=开没开，`level`=开合度百分比（不到 50% 算关）。`buzzer`：`power`=正在响，`muted`=静音锁生效中。
- `mode`= `"auto"` / `"manual"`，`auto` 是它的布尔版；`wifi` / `mqtt`=两段链路通没通；`rssi`=信号 dBm（没连是 0）；`ip` 没连是 `0.0.0.0`；`uptime`=开机秒数。

`sensor`（2 秒一次，menuconfig「传感器上报周期」默认 2000ms；retain=0，掉线不补发）：
```json
{"temp":31.5,"humi":58.2,"temp_valid":true,"temp_alert":false,"lux":18.3,"light_pct":12.0,"light_is_lux":false,"light_valid":true,"rain":12.5,"raining":false,"rain_valid":true}
```
- `temp` / `humi`=温度 ℃ / 湿度 %；`temp_valid`=读数可不可信，`false` 时前端显示 `--`，别当真实数据。
- `temp_alert`=温度 ≥ `temp_alert_c`（默认 32℃），前端据此让温度数字闪烁。
- `lux` / `light_is_lux` / `light_valid`=真实照度；`true`=BH1750 真 lux，`false`=光敏电阻折算的百分比；光照可不可信。
- `light_pct`=统一到 0~100 的光照强度，光控联动用这个。
- `rain` / `raining` / `rain_valid`=雨滴湿度 0~100；是否判定下雨；雨滴读数可不可信（ADC 悬空会飘）。

`weather`（retain=1，打开页面就能看到）示例 `{"raining":false,"text":"Sunny","rain_pct":12.5}`：
- `raining`=是否下雨；`text` 直接拿去显示（`"Raining!"` / `"Sunny"`，都是 ASCII）；`rain_pct`=雨滴原始百分比，调试用。

## ack 和 event
`ack`（你发什么 dev/action 就回什么，`ok=true` 就是成功）示例 `{"dev":"led_living","action":"on","ok":false,"detail":"自动模式下灯光由光照控制，请先切到手动模式"}`：
- `自动模式下灯光由光照控制，请先切到手动模式` —— 自动模式下灯光指令会被拒，属于正常行为。
- `设备无响应（接线或供电问题）` —— 硬件写失败，软件状态没改。
- `未知设备: xxx` / `未知动作: xxx` —— dev / action 不支持；`payload 解析失败` —— JSON 语法错或超过 256 字节。

`event`（retain=0）示例 `{"what":"key","info":"mode","uptime":1234}`：`what=device` 时 `info` 是设备名（语音 / 手机 / 按键 / 串口引起的设备变化），
`what=key` 时 `info` 是 `mode` / `all_toggle` / `door_toggle`；`uptime`=开机秒数。自动联动（光控、开门亮灯）引起的变化不发 event，只看 `state`。

## 下行指令 `<base>/cmd`
三种写法都行，最后都进 `home_command_execute()`，行为完全一致。
```json
{"dev":"led_living","action":"on"}
{"dev":"led_living","action":"level","value":40}
{"dev":"all","action":"off"}
{"dev":"door","action":"open"}
{"action":"mode","value":"manual"}
{"dev":"config","action":"temp_alert_c","value":30}
```
- `dev`：`led_living` / `led_kitchen` / `led_bedroom` / `led_bath` / `door` / `buzzer` / `all` / `mode` / `voice` / `config`。
- `action`：`on` / `off` / `toggle` / `level`（带 `value` 0~100）；门能用 `open` / `close`，开合度用 `set`。
- `all on` / `all off` 只动 4 盏灯、绝不开门，门必须单独发 `door`。
- 不用写 `dev`：`{"action":"mode","value":"manual"}`、`{"action":"buzzer","value":"off"}` 也认。
- `dev=voice` 是注入一条语音指令，和真说话走同一条链路，试播报很方便。
- payload 上限 256 字节，超了回一条失败的 ack。

键值形式：`dev=led_living,action=on`（值也能写成 `act=` / `val=`，逗号空格都能分隔）。纯短名形式等于“说了一句话”：
`led_living_on`、`door_open`、`led_all_off`、`mode manual`、`query_status`（完整表在 [components/BSP/VOICE/voice.c](../components/BSP/VOICE/voice.c) 的 `s_cmd_table[]`）。

## 在线改阈值（下行 `<base>/config`）
发到 `<base>/cmd` 或 `<base>/config` 都行，格式一样：`{"dev":"config","action":"light_on_pct","value":30}`。键名和固件 `auto_policy_set_threshold()` 认识的完全一致：
```text
mode                "auto" / "manual"，也认 true / false（别名键 auto）
light_on_pct        光照低于它 → 自动开客厅灯（默认 20，0~100）
light_off_pct       光照高于它 → 自动关客厅灯（默认 45，必须 > light_on_pct）
auto_light_enable   光照联动总开关（默认 true）
buzzer_on_c         温度 ≥ 它 → 蜂鸣器提示（默认 32，单位 ℃，0~80）
buzzer_off_c        温度 ≤ 它 → 蜂鸣器停（默认 31.5，必须 < buzzer_on_c）
auto_buzzer_enable  蜂鸣器联动开关（默认 true）
temp_alert_c        温度 ≥ 它 → 屏幕温度数字闪烁（默认 32，单位 ℃）
rain_pct            雨滴 > 它 → 判为下雨（默认 30，0~100）
door_light_cascade  开门是否自动亮客厅灯（默认 true）
buzzer_active_low   蜂鸣器低电平触发（默认 false，现场反了才改）
```
- 单位别搞混：MQTT 里 `buzzer_on_c` 发的是 **℃ 的整数**（32 就是 32℃）；menuconfig 的「蜂鸣器启动温度」按 0.1℃ 存（默认 320 = 32.0℃），两处不是一个单位。
- MQTT 只能发整数，31.5 会变成 32；要 31.5 就改 menuconfig，或串口 `cfg buzzer_off_c 31.5`。
- 布尔键发 JSON 的 `true` / `false`；发字符串 `"false"` 会被当成 1，反而变 true。
- 改一对上下限先发 on 再发 off：off 比当前的 on 小会被拒；被拒时原值不动，原因在 ack 和串口日志里。阈值一改立刻存 NVS，掉电不丢。

## 用 MQTTX 手工调试
1. 新建连接 `mqtt://broker.emqx.io:1883`（**不是 8084**，那是给浏览器的）。
2. 订阅 `smarthome/+/state`，能看到所有用这个前缀的设备，从中找出自己的 base，再订阅它下面的 `#`。
3. 往 `smarthome/4d3b9c/get` 发任意内容 → 马上收到 state + sensor + weather + config。
4. 往 `smarthome/4d3b9c/cmd` 发 `{"dev":"door","action":"open"}` → 门转、客厅灯亮，ack 和 event 都有反馈。
5. 发 `{"dev":"config","action":"light_on_pct","value":30}` 改阈值，再看 `/config` 回显的新值。

## 公共 broker 安全提醒
`broker.emqx.io` 是公共测试服务器，谁猜到你的 topic 谁就能控制你的设备。
- 先把 **topic 前缀**改成带个人标识的（`smarthome-zhangsan`），不然订阅 `smarthome/+/state` 就能看到别人家。
- 正式用就换自建 broker + 用户名密码（menuconfig 里有 `Broker 用户名/密码`），或上云平台；局域网最省事：`mqtt://192.168.1.100:1883`。
