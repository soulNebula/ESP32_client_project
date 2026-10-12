# 安卓 App（蓝牙直连版）

需求⑧ 的手机端：控制 4 路灯、门、全灯，看温湿度和天气，还能在线改阈值。原生 Android，
Kotlin + 传统 View（界面全是代码拼的，没有 layout XML）；**蓝牙 BLE 客户端是自己写的**，
不引第三方蓝牙库 —— 和以前不引第三方 MQTT 库一个道理。

和以前 MQTT 版最大的区别：**没有 broker、没有 topic、没有端口、不用读串口日志**。
客户装好 App、开蓝牙、在设置页点一下自己的板子就连上了，唯一要填的是 WiFi 账号密码。

## 0. 已经编译过了，APK 在 apk/ 里

```text
android\apk\SmartHomeBLE-debug.apk     5.42 MB  (5682093 字节)
android\apk\SmartHomeBLE-release.apk   4.42 MB  (4638695 字节)   （用 debug 签名，能直接装）
```

包名 `com.smarthome.ble.panel`，应用名「智能家居蓝牙」，minSdk 26 / targetSdk 35。
**包名故意和参考工程的 `com.smarthome.ble` 差一个后缀**，这样两个 App 能装在同一台手机上互不覆盖。
权限只有蓝牙那一套（12+ 的 BLUETOOTH_SCAN/CONNECT，11 及以下的 BLUETOOTH 族 + 位置），
已经**没有** INTERNET —— 这台 App 不连网。

> 本机没装 `pwsh`（PowerShell 7），调它必挂，所以 `build.ps1` 走 `powershell.exe`（5.1）
> 和 Gradle 缓存。另外 **`.cmd` 里不能写中文**：cmd.exe 按 GBK 读 UTF-8，中文会变乱码命令。
> 中文全放在 `build.ps1`，`build.cmd` 只是个纯 ASCII 的转发壳。

## 1. 怎么编译

```bat
cd android
build.cmd                                  :: 默认 assembleDebug
build.cmd assembleDebug assembleRelease     :: 两个都出
```

或者直接用 PowerShell（推荐，能正常显示中文）：

```powershell
powershell -ExecutionPolicy Bypass -File android\build.ps1 assembleDebug assembleRelease
```

- 不需要 `gradle-wrapper.jar`：脚本自己在本机缓存里找 Gradle 8.9，找不到再退回 PATH 里的 `gradle`；JDK 优先用 Android Studio 自带的 JBR 21。
- 用 Android Studio：**Open** 选 `android\` 目录（不是仓库根，也不是 `app\`），Gradle JDK 选 **17 或 21**，然后点 ▶。
- 装到手机：`adb install -r android\apk\SmartHomeBLE-debug.apk`
- 环境：JDK 21 / Gradle 8.9 / AGP 8.7.0 / Kotlin 2.0.21 / compileSdk 35。

## 2. 第一次用：扫一台板子

1. 板子上电（在广播 `SmartHome-4d3b9c` 这种名字，按 Service UUID 广播）。
2. App 右上「菜单」→ **扫描与设置** → 「开始扫描」，列表里会出设备名 + MAC + RSSI，点一下就连。
   扫描按 Service UUID 过滤，**10 秒自己停**，不会一直扫。
3. 连上后 App 自动订阅 TX 通知、发一帧 `0x03 GET` 把状态拉全，顶栏变绿「在线」。
4. 同一个页面填 **WiFi 名字 + 密码** → 「保存并发给设备」（走 `0x04` 帧）。板子存 NVS 并立刻重连。
   这是客户唯一要填的东西。传空 SSID 表示清除，回到固件编译时的默认值。
5. **网页面板地址**：板子回的 `0x16 INFO` 帧里有 `base`（例如 `smarthome/4d3b9c`），
   App 直接显示出来，抄进网页面板就行 —— 不用再翻串口日志。

> 权限被拒的话主界面有红黄提示条，点一下重新弹系统授权框；勾了「不再询问」就去系统设置里开。
> 蓝牙没开、不是 12+ 附近设备权限的机型，提示文案都会说清缺哪一项。

## 3. 界面按哪个键，发出什么

| 按哪儿 | 发到板子的指令 |
|---|---|
| 客厅灯 / 厨房灯 / 卧室灯 / 浴室灯 的「开」「关」 | `{"dev":"led_living","action":"on"}`，关是 `off`；另三盏换成 `led_kitchen` / `led_bedroom` / `led_bath` |
| 门「开门」「关门」 | `{"dev":"door","action":"open"}` / `{"dev":"door","action":"close"}` |
| 「全灯亮」「全灯灭」 | `{"dev":"all","action":"on"}` / `off`（all 只管灯，不含门） |
| 模式徽章（点一下在自动/手动间切） | `{"action":"mode","value":"auto"}`，切手动是 `"manual"` |
| 「🔔 蜂鸣器」 | `{"action":"buzzer","value":"off"}` 静音（固件里静音是锁）/ `"on"` 解除 |
| 「改阈值」（数字 6 个 + 开关 4 个，只发改过的） | `{"dev":"config","action":"light_on_pct","value":30}`，开关是 `{"dev":"config","action":"auto_light_enable","value":false}`，一帧一个键 |
| 语音 6 个按钮 | `{"dev":"voice","action":"query_temp"}`，其余 `query_humi` / `query_all` / `query_weather` / `query_light` / `query_status` |
| 菜单 → 刷新状态 | 发一帧 `0x03 GET` |

按了键**不**立刻把界面点亮（不做乐观 UI）：自动模式下固件会拒绝灯光指令，乐观 UI 会显示「灯已开」
而实际没开。状态一律以板子回的 `0x11` / `0x15` 为准。

能改的阈值键：数字 `light_on_pct` / `light_off_pct` / `buzzer_on_c` / `buzzer_off_c` / `temp_alert_c` / `rain_pct`；
开关 `auto_light_enable` / `auto_buzzer_enable` / `door_light_cascade` / `buzzer_active_low`。
上下限是一对，原值没动的不发；被拒时 `0x15 ACK` 的 `detail` 会显示在顶部红条上。

> ⚠ 开关型阈值**必须发 JSON 的真布尔** `true` / `false`，**不能发字符串** `"false"`：
> 固件里字符串数字解析失败会让值停在 `-1`，兜底成 `"1"`，反而把开关打开。
> App 里拼的是 `$value`（Kotlin Boolean 直接进模板），别手贱加引号。

## 4. 温湿度 / 天气卡片

板子每 2 秒推一帧 `0x12 SENSOR`（温度、湿度、光照、雨滴）和 `0x13 WEATHER`，**只显示、不下发**。
温度 ≥32℃ 时数字变红，阈值以板子报的 `temp_alert` 为准，App 不自己判。

## 5. 代码在哪

```
android/app/src/main/java/com/smarthome/ble/
├── MainActivity.kt      主界面：卡片 + 设备开关 + 模式徽章 + 静音 + 阈值弹窗 + 日志
├── SettingsActivity.kt  扫描选设备 + 下发 WiFi + 显示 base（原来是 broker/端口/TLS）
├── SmartHomeApp.kt      唯一状态源：持有 BleManager，收帧、解析、发给界面
├── ble/BleManager.kt    扫描 / 连接 / 订通知 / 分片收发 / 断线退避重连
├── data/Contract.kt     协议契约：三个 UUID、帧类型、指令和键名（唯一出处）
├── data/Json.kt         JSON 小工具（只包 org.json）
├── data/Models.kt       数据模型 + 帧解析 + 下行指令
└── ui/Ui.kt             手写 UI 小工具（颜色 / dp / 卡片 / 按钮 / 输入框）
```

协议本身看 `docs/蓝牙协议.md`，那边是唯一真源。

## 6. 已知问题和注意事项

- 没板子，端到端没验过：扫描能不能扫到、通知收不收得到、指令固件认不认，得你拿手机 + 板子跑一遍。
- 只编译验证过；BLE 的分片重组和 MTU 退回路径只有编译保证，没有真机跑过。
- 阈值里那 4 个布尔开关已经能改，发的是 JSON 真布尔（见第 3 节那个 ⚠）。
  但 `buzzer_active_low` 是现场接反了才动的，别没事去点它。
- 不做后台常驻：App 退到后台蓝牙可能被系统冻结，回前台会自动刷新；要常驻得加前台 Service。
- 一次只管一块板子；换板子到设置页重新扫一台，会记住上次连的那台并在启动时自动连。
