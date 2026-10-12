# ESP32-S3 智能家居（光影语音版）

喊一声就能开灯关门，热了会响会闪，手机蓝牙一连就能控，屏幕上看温湿度

```text
2/
│
├── CMakeLists.txt                 顶层构建脚本
├── partitions-16MiB.csv           分区表（nvs / ota / model / storage）
├── sdkconfig.defaults             默认配置（改配置看这里）
├── sdkconfig                      当前生效的配置（改了 defaults 要清掉它）
├── dependencies.lock              依赖版本锁定
├── .clang-format                  代码风格
├── .gitignore / .gitattributes
│
├── main/                          入口
│   ├── main.c                     开机启动、主循环、串口调试台
│   ├── board.c / board.h          上电初始化 + 自检表
│   ├── Kconfig.projbuild          所有可配置项
│   ├── idf_component.yml          声明 esp-sr 依赖
│   └── CMakeLists.txt
│
├── components/
│   ├── BSP/                       硬件驱动，引脚真源在这
│   │   ├── board_config.h         全部引脚定义（改引脚只改它）
│   │   ├── I2C/     i2c_bus.c/.h
│   │   ├── ADC/     adc_bus.c/.h
│   │   ├── GPIO/    gpio_util.c/.h
│   │   ├── LED/     led.c/.h
│   │   ├── KEY/     key.c/.h
│   │   ├── SERVO/   servo.c/.h
│   │   ├── BUZZER/  buzzer.c/.h
│   │   ├── SENSOR/  sensor.c/.h
│   │   ├── I2S_MIC/ i2s_mic.c/.h
│   │   └── VOICE/   voice.c/.h + voice_internal.h + voice_esp_sr.c/.h
│   ├── App/                       业务逻辑
│   │   ├── device_model.c/.h      设备状态，唯一硬件出口
│   │   ├── app_link.c + app_cmd.h 四个入口的指令路由
│   │   ├── automation.c/.h        手动/自动模式 + 阈值（存 NVS）
│   │   ├── home_auto.c/.h         光照和高温联动
│   │   ├── net_link.c/.h          WiFi + MQTT
│   │   ├── mqtt_protocol.c/.h     话题和消息格式
│   │   └── ble_app.c/.h           蓝牙通道（手机直连 + 下发 WiFi）
│   └── ui/                        屏幕
│       ├── panel.c/.h             OLED 驱动 + 画图
│       ├── panel_font.c/.h        字库查表
│       ├── font_cn_data.c         基础字模
│       ├── font_cn_extra.c        补充字模
│       └── display_view.c/.h      四行排版
│
├── android/                       手机 App（Kotlin）
│   ├── app/src/main/              源码（6 个 .kt）
│   ├── apk/                       已经编好的安装包（debug + release）
│   ├── build.ps1                  构建脚本（中文提示都在这）
│   ├── build.cmd                  同上，纯 ASCII 转发壳
│   └── README.md
├── web/index.html                 手机网页版面板
│
├── arduino/SmartHomeArduino/      Arduino 版（同一套硬件，给甲方自己编）
│   ├── SmartHomeArduino.ino       入口：启动、主循环、串口台、语音指令翻译
│   ├── bsp_config.h               ★ 引脚定义（和上面 components/BSP/board_config.h 一一对应）
│   ├── app_config.h               ★ 编译期开关（WiFi / 阈值 / 关功能）
│   ├── 其余 .cpp/.h               驱动层 + 屏幕 + 应用层（★ 全部平铺，不能放子目录）
│   ├── tools/                     check_style.ps1 / check_font.ps1 / gen_font.py
│   └── README.md                  ★ 甲方看这份：装什么、选哪个开发板、怎么烧
│
├── docs/
│   ├── 接线文档.md                 接哪根线、电源、不能接的脚、需求原文、排错
│   ├── 蓝牙协议.md                 UUID、帧格式、App 与板子怎么对话
│   ├── MQTT协议.md                 话题表 + 消息格式 + 下行指令
│   └── 系统架构与实现说明.md        三层结构、主循环、真机踩的坑
│
└── tools/
    ├── build/     build.ps1 / flash.ps1 / idf_build.ps1 / build_verify.ps1
    ├── monitor/   capture_boot.py / run_console_cmds.py
    ├── verify/    注释自检、命令词校验、屏排版预览、MQTT 自测
    └── dev/       字库生成脚本
```

> 表里 `a.c/.h` 就是同一对源文件和头文件
> `build/` 和 `managed_components/` 没列进来，它们是编译时生成的
> 编译烧录统一走 `tools/`，需要电脑上装好 ESP-IDF（见「怎么编译」）

## 客户怎么用（只装 App，不碰串口）

1. 手机装 [`android/apk/SmartHomeBLE-debug.apk`](android/apk/SmartHomeBLE-debug.apk)（release 包同目录）
2. 打开 App → 点「扫描」→ 列表里选 `SmartHome-xxxxxx`
3. 进「WiFi 设置」，把家里 WiFi 名和密码发给板子（走蓝牙写进去，板子存在自己的 flash 里）
4. 回主界面，开关灯 / 开关门 / 看温湿度天气全都能用了

**全程不需要串口、不需要装 ESP-IDF、不需要填 broker 和 topic。**
WiFi 只是给网页面板用的 —— 不填也能用 App 控制，因为它走的是蓝牙。

想再用网页面板：App 里会把 `smarthome/xxxxxx` 这串（base topic）显示出来，填进
[`web/index.html`](web/index.html) 就行。以前这串得翻串口日志抄，现在不用了。

## 改 WiFi / MQTT（开发者用，客户不用管）

**客户**在 App 里填 WiFi 就够了（上一节）。下面这些是改编译时的默认值、或换 broker 用的。

WiFi 账号优先用 App 下发的那份（存在 NVS），没有才用这里的编译时默认值。
改完**必须清一次 sdkconfig**，不然不生效：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -Menuconfig   # 改默认 WiFi / MQTT
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -FullClean    # 清掉 sdkconfig
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -SetTarget    # 首次跑一次
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -Flash -Monitor
```

进 `智能家居（光影语音版）—— 项目配置`：

| 在哪儿 | 改什么 |
|---|---|
| WiFi 设置 | WiFi 名称（**必须是 2.4GHz**，ESP32-S3 不认 5GHz）与密码 —— 客户能在 App 里改，这里只是出厂默认值 |
| MQTT 设置 | Broker 地址，公共测试用 `mqtt://broker.emqx.io:1883` |
| 蓝牙设置 | 广播名前缀（默认 `SmartHome`，手机扫描列表里看到的就是它 + 后 6 位 ID）|

**Topic 前缀顺手改一下**（默认 `smarthome`）：公共 broker 上前缀撞了会看到别人的设备。

几件不用操心的事：

- **WiFi 连不上也能用** —— 蓝牙不依赖 WiFi；语音、屏幕、自动联动照常，只是网页面板用不了
- **屏幕型号不用选** —— 已经固定是 SSD1306 128×64 单色 OLED（I2C）
- **按键默认关着** —— 本项目的控制入口是语音 + 蓝牙 App + 网页面板

## 怎么编译

> 下面这几条要求电脑上已经装好 ESP-IDF —— 工程里原来那个自带整套环境的
> `sandbox\` 已经删了，绿色环境改用另一个工程（见「环境沙箱」一节）。

```powershell
# 编译
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1

# 首次要先 set-target
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -SetTarget

# 烧录（擦除→写→校验，默认连语音模型一起烧）
powershell -ExecutionPolicy Bypass -File tools\build\flash.ps1

# 只烧改动，最快（不含语音模型）
powershell -ExecutionPolicy Bypass -File tools\build\flash.ps1 -AppOnly

# 看串口
powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -Monitor
```

> **中文 Windows 上别绕开脚本直接敲 `idf.py build`**：esp-sr 打包模型那一步会按 GBK 去读
> 带中文注释的 `sdkconfig`，报 `UnicodeDecodeError`，看着像模型坏了其实只是编码问题。
> 脚本里已经设了 `PYTHONUTF8=1`，手工构建就自己先 `$env:PYTHONUTF8='1'`。
>
> 第一次编译要联网（下 esp-sr 和模型，几十 MB），模型会进 flash 的 `model` 分区，
> 之后离线也能编译。`idf.py flash` 会自动把模型一起烧。工程路径**别带中文**。

没装 ESP-IDF 的话，去 [Espressif 下载门户](https://dl.espressif.com/dl/eim/?tab=offline)
拿离线安装包，装完就能编译（`.zst` 别解压，直接指给安装器）。

## 环境沙箱（已删除）

本工程原来有一个自带整套环境的 `sandbox\`（编译器、cmake、ninja、Python、
ESP-IDF 全在里面，双击 `.cmd` 就能编译烧录看串口）。它**已经删掉了** ——
绿色环境不再每个工程各存一份，现在统一用**另一个工程**的
`esp32_smart_home\.idf-sandbox\`（约 1.9 GB，自带 Python 3.11.2 +
ESP-IDF 5.4.4 + xtensa 工具链 + tkinter 图形界面），本工程当前的 `build\`
就是用它编出来的。那边的入口是工程根目录下的 `智能家居控制台.cmd` /
`一键编译烧录.cmd`，用法见那边的 `.idf-sandbox\README.md`。

本工程自己的编译烧录仍然走 `tools\build\*.ps1`（用系统里装的 ESP-IDF，
默认探测 `E:\Espressif\frameworks\esp-idf-v5.4.4`），命令见上一节。


## 串口调试台（自己排查用，客户不用）

> 客户流程全在 App 里（蓝牙），**不需要串口**。这一节是给你自己查问题用的。

`CONFIG_APP_SERIAL_DEBUG_ENABLE` 默认开着，115200 8N1 直接敲：

```text
help                  看所有命令
status                设备状态 + 模式 + 阈值 + 传感器
hw                    自检表 + 麦克风电平
on led_living         开客厅灯（off / toggle 同理）
all on / all off      四路灯全开 / 全关（不动门）
open door             开门（会顺手点亮客厅灯）
mode auto|manual      切手动 / 自动
buzzer mute           蜂鸣器静音
cfg light_on_pct 20   改阈值（cfg reset 恢复默认）
say led_living_on     假装说了一句话（走和真语音同一条路）
screen                强制重画屏幕
```

**配件没到齐也能验**：`say xxx` 和真语音走同一条下游，不用买麦克风就能验需求①④⑤⑥。

不接板子也能跑的自检：

```bash
python tools/verify/check_voice_phrases.py   # 命令词表：枚举覆盖 / 音节 / 句式去重
python tools/verify/preview_oled.py          # 屏排版预览，报越界像素
python tools/verify/mqtt_check.py            # MQTT 端到端
```

> ⚠ 这些脚本用**系统 Python** 跑，别在「ESP-IDF 终端」里跑 ——
> IDF 自带的 Python 环境没装 `Pillow` / `pyserial`，会报 `No module named 'PIL'`。
> 缺库就 `pip install pillow pyserial paho-mqtt`。

## 语音能说什么

先喊 **「你好小智」**，6 秒内说命令。识别在板子上跑（ESP-SR），模型在 flash 的 `model` 分区。

| 说什么 | 干什么 |
|---|---|
| 打开 / 关闭客厅灯、厨房灯、卧室灯、浴室灯 | 对应那路灯开关 |
| 打开全部灯 / 关闭全部灯 | 四路灯全开 / 全关（不动门）|
| 打开门 / 关闭门 | 开关门，开门顺手亮客厅灯 |
| 自动模式 / 手动模式（也能说「开启…」「切换到…」）| 切模式 |
| 打开蜂鸣器 / 关闭蜂鸣器 | 解除 / 上静音锁 |
| 现在温度多少 / 湿度多少 / 天气怎么样 | 查询（本机没喇叭，只进日志）|

一共 **22 条指令、28 种说法**。换唤醒词或命令词要改 `menuconfig` → ESP Speech Recognition，
改完重烧 `model` 分区。

## 手机端

**安卓 App 走蓝牙**（客户就用这个），**网页面板走 MQTT**（可选，要先在 App 里配好 WiFi）。

- **安卓 App** —— 装 [`android/apk/SmartHomeBLE-debug.apk`](android/apk/SmartHomeBLE-debug.apk)，
  用法见上面「客户怎么用」。协议在 [蓝牙协议](docs/蓝牙协议.md)。
  改完代码自己重编：`android\build.ps1`（或 `android\build.cmd`），细节看 [`android/README.md`](android/README.md)
- **网页面板** —— 浏览器打开 [`web/index.html`](web/index.html)，base topic 从 App 里抄。
  话题和消息格式见 [MQTT协议](docs/MQTT协议.md)

## Arduino 版（甲方自己编）

板子已经交付了，甲方那边不一定有 ESP-IDF 环境，所以另有一份
[`arduino/SmartHomeArduino/`](arduino/SmartHomeArduino/)：

- **不用装任何第三方库** —— 屏幕驱动、中文字库、MQTT 客户端都是工程里自己写的
- 装好 Arduino IDE + **esp32 开发板支持（3.0.0 以上）**，选
  `ESP32S3 Dev Module` + 分区方案 `ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)` 就能烧
- 引脚、串口命令、MQTT 话题与 JSON、蓝牙帧协议、屏幕排版都和这份 ESP-IDF 版一致

**行为差别只有三处**：语音判读更简单（唤醒后只有一次命令窗口）、
按键与采样改到主循环里按时间片轮询（周期一致）、刷屏同样由主循环驱动。
语音识别用的还是同一个 `ESP_SR` 库，命令词表逐字相同。

甲方要看的步骤都在 [`arduino/SmartHomeArduino/README.md`](arduino/SmartHomeArduino/README.md)。

## 代码风格

跟参考工程 `esp32_smart_home` 一样：

- 每个文件最上面两行：第一行 `// c` 标语言，第二行一句话说这文件干什么
- 注释**不超过 10 个字**，中文口语；不写 `@param` 那一套，也不要 `/* ==== */` 分隔线
- 花括号跟着上一行（`if (x) {`），4 空格缩进，文件存成 UTF-8 无 BOM
- 静态变量用 `s_` 开头，日志用 `TAG`

排版交给根目录的 [`.clang-format`](.clang-format)，改完跑一下注释自检：

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify\check_comments.ps1
```
