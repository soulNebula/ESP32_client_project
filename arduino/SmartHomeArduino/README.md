# Arduino 版 · ESP32-S3 智能家居（光影语音版）

这是 ESP-IDF 主工程的 **Arduino 版**。功能、串口命令、MQTT 话题、JSON 字段、
蓝牙协议、屏幕排版都跟主工程一致，方便对照排查。

## 需要装什么（就这些，没别的）

| 要装的 | 说明 |
|---|---|
| **Arduino IDE** | 2.x 或更新，官方下载 <https://www.arduino.cc/en/software> |
| **ESP32 开发板支持** | 开发板管理器里装 `esp32 by Espressif Systems` |

**就这两个。** 具体说：

- ❌ **不需要 ESP-IDF** —— 一行 IDF 命令都不用敲，不用装 ESP-IDF 终端、不用 `idf.py`
- ❌ **不需要装任何第三方库** —— 屏幕驱动、中文字库、MQTT 客户端都是工程里自己写的
  （没用 U8g2、没用 PubSubClient、没用 ESP32Servo、没用 Adafruit_NeoPixel）
- ✅ **BLE / WiFi / Wire / Preferences 都是 esp32 开发板支持自带的**，装完就有
- ⚠ 只有**一个例外**：板载语音识别要用核心自带的 `ESP_SR` 库，
  而那个库是**核心 3.0.0 以上**才有的（见下面「语音」一节）

> 换句话说：**只要开发板管理器里那个 `esp32` 装好了，装什么版本都能编译**。
> 默认配置是按 2.x 也能编设的。

---

## 一、先装这两样

1. **Arduino IDE**（2.x 或更新）：<https://www.arduino.cc/en/software>
2. **ESP32 开发板支持**：

   Arduino IDE → `文件` → `首选项` → 在「附加开发板管理器网址」里填：

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

   → `工具` → `开发板` → `开发板管理器` → 搜 `esp32` → 装 **esp32 by Espressif Systems**。

### 装哪个版本？

| 核心版本 | 能不能编 | 语音 |
|---|---|---|
| **2.x**（比如 2.0.9） | ✅ 能，默认配置直接过 | ❌ 喊不动（用手机 App / 串口下发指令，效果一样） |
| **3.0.0 以上** | ✅ 能 | ✅ 能，把 `app_config.h` 的 `APP_USE_ESP_SR` 改成 1，分区方案选 `ESP SR 16M ...` |

**只要不做语音，装哪个版本都行。** 看自己装的哪个版本：
开发板管理器里看 `esp32` 那一行的版本号。

> 想要"喊一声就能控"（和出货固件一样），就装 **3.0.0 以上**，然后：
> ① `app_config.h` 里 `APP_USE_ESP_SR` 改成 `1`
> ② 分区方案选 `ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)`
>
> 默认给的是 `0`，这样 2.x 也能开箱即编。

## 二、打开工程

**关键：文件夹名必须和 `.ino` 同名。** 本目录已经对了，直接：

```text
文件 → 打开 → 选中 arduino/SmartHomeArduino/SmartHomeArduino.ino
```

> ⚠ 工程路径**别带中文**，也别放在 OneDrive 同步目录里（编译会偶发失败）。

## 三、开发板设置（照抄，一项都别错）

`工具` 菜单里：

| 菜单项 | 选什么 |
|---|---|
| 开发板 | **ESP32S3 Dev Module** |
| **Partition Scheme** | 核心 3.x → **ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)**<br>核心 2.x → **16M Flash (3MB APP/9.9MB FATFS)** ← 最重要 |
| Flash Size | **16MB (128Mb)** |
| PSRAM | **OPI PSRAM**（N16R8 模组；如果是 N8R2 就选 QSPI PSRAM） |
| USB CDC On Boot | 用板载 USB 口看串口 → **Enabled**；用外部 USB-TTL → Disabled |
| Upload Speed | 921600（不稳定就降到 115200） |
| CPU Frequency | 240MHz (WiFi) |
| Arduino Runs On | Core 1 |

> **Partition Scheme 为什么这么重要**：
> 本工程编出来约 **1.31 MB**，而 Arduino 默认方案只给 **1.2 MB** App ——
> 不改分区会直接报 `text section exceeds available space in board`。
> 上面那个方案给了 3 MB App，同时给语音模型留了 3.9 MB 的 `model` 分区，一步到位。
>
> 只有核心 2.x 的话（它没有 `ESP SR 16M` 这个选项），选
> **`16M Flash (3MB APP/9.9MB FATFS)`** 也一样够用（就是没地方放语音模型）。

## 四、编译与上传

点左上角 **✓（验证）** 编译，**→（上传）** 烧录。

第一次编译要下工具链，慢一点；之后都是几十秒。

正常情况下应该看到（默认配置，语音关）：

```text
Sketch uses 1374657 bytes (43%) of program storage space. Maximum is 3145728 bytes.
Global variables use 71640 bytes (21%) of dynamic memory, leaving 256040 bytes for local variables.
```

看到这两行就说明编过了，可以点上传。

> **报 `text section exceeds available space in board` 或 `Sketch too big`**：
> 分区方案没选对，按上面第三节改。

## 五、看板子活没活

`工具` → `串口监视器`，右下角波特率选 **115200**。

**中文显示成乱码**：把串口监视器右下角的编码改成 **UTF-8**
（IDE 2.x 默认就是）。嫌麻烦就在 `app_config.h` 里把 `APP_CONSOLE_ASCII` 改成 `1`，
串口台就全变英文了。

开机应该看到：

```text
###########################################################
#  ESP32-S3 智能家居（光影语音版）—— Arduino 版
###########################################################
BOARD: =========== 引脚分配表（唯一真源 bsp_config.h） ===========
...
```

敲 `help` 看全部命令，敲 `status` 看设备/传感器/网络状态。

## 六、客户怎么用（和 ESP-IDF 版一模一样）

1. 手机装 [`android/apk/SmartHomeBLE-debug.apk`](../../android/apk/SmartHomeBLE-debug.apk)
2. App 里点「扫描」→ 选列表里的 `SmartHome-xxxxxx`
3. 进「WiFi 设置」把家里 WiFi 名和密码发过去（走蓝牙，板子存 flash）
4. 回主界面，开关灯 / 开关门 / 看温湿度全能用

语音：喊 **「你好小智」**，6 秒内说命令（说法表见 `help-voice`）。

---

## 和 ESP-IDF 版的差别（就这三处）

| 项 | ESP-IDF 版 | Arduino 版 |
|---|---|---|
| **语音识别** | ESP-SR（AFE + WakeNet + MultiNet） | 同一个 ESP_SR 库，说法表逐字相同；判读逻辑更简单，唤醒后**只有一次**命令窗口 |
| **按键** | 独立任务 10ms 扫描 | 主循环里轮询，`loop()` 每轮 5ms |
| **采样与刷屏** | 各自独立任务 | 主循环里按时间片跑（周期一致） |

**行为完全一致的部分**：四条指令入口、自动联动两条规则、静音锁、开门级联
（含 30 秒免打扰）、手动保护期、阈值与 NVS 落盘、屏幕四行排版、
MQTT 话题与 JSON、蓝牙帧协议、串口调试台命令。

> 另外：**WiFi + 蓝牙同时开时可用内存会紧张**。ESP32-S3 内部 SRAM 就那么大，
> 如果日志里出现 `malloc failed`，把 `app_config.h` 的 `APP_USE_BLE` 或 `APP_USE_MQTT` 关一个。

---

## 文件都在这儿

> ⚠ **所有 `.cpp` / `.h` 必须和 `.ino` 平铺在同一层**，不能放进子文件夹。
> Arduino 的构建只有一个「草图根目录」在编译和 include 路径里 ——
> 放子目录里编译器根本不会去编（子目录里的头文件也 include 不到根目录的头文件）。
> 已验证过：`bsp/`、`ui/` 子目录布局编不出固件，平铺才行。
> **加新文件就直接放这一层**，用文件名前缀分组（`bsp_*` / `ui_*` 之类）。

```text
SmartHomeArduino/
├── SmartHomeArduino.ino      开机启动、主循环、串口调试台、语音指令翻译
├── bsp_config.h              ★ 全部引脚定义（改接线只改它）
├── app_config.h              ★ 编译期开关与默认值（WiFi / 阈值 / 关功能）
├── app_common.h/.cpp         错误码、日志、时间
├── board.h/.cpp              上电初始化顺序 + 自检表
│
│   ── 硬件驱动（唯一直接碰总线与 GPIO 的一层）──
├── i2c_bus.cpp               I2C 总线封装（只跑 100kHz）
├── gpio_util.cpp             GPIO 读写 + 板载 WS2812 状态灯
├── led.cpp                   4 路灯（LEDC CH2~CH5 / TIMER_2）
├── servo.cpp                 门舵机（LEDC CH0 / TIMER_0 @50Hz，14bit）
├── buzzer.cpp                蜂鸣器（LEDC CH1 / TIMER_1 @2kHz，或 GPIO 电平）
├── sensor.cpp                温湿度 + 光照 + 雨滴（都带 valid 标志）
├── key.cpp                   按键消抖 + 短按/长按
├── voice.cpp                 指令表、播报、ESP_SR 唤醒与命令词
│
│   ── 屏幕 ──
├── panel.cpp/.h              SSD1306 驱动 + 1KB 显存 + 脏页差分
├── panel_font.cpp/.h         字库查表 + UTF-8 解码
├── panel_font_data.h         点阵字模（自动生成，别手改）
├── panel_text.h              一行字的像素宽度
├── display_view.cpp/.h       四行排版
│
│   ── 应用层 ──
├── device_model.h/.cpp       设备状态中枢（唯一真源 + 唯一硬件出口）
├── automation.h/.cpp         模式 + 全部阈值 + NVS 落盘
├── app_cmd.h / app_link.cpp  四条入口的唯一收敛点
├── home_auto.h/.cpp          光照控灯 + 高温报警
├── net_link.h/.cpp           WiFi + MQTT 客户端（自己实现，不依赖 PubSubClient）
├── mqtt_protocol.h/.cpp      话题与 JSON 生成（蓝牙共用同一份）
├── ble_app.h/.cpp            蓝牙 GATT 服务端（和 App 的协议逐帧一致）
│
├── README.md                 本文件
└── tools/                    不参与编译，随便放
    ├── check_style.ps1       结构自检（文件头/编码/注释/包含/实现）
    ├── check_font.ps1        字库覆盖自检
    ├── gen_font.py           字模生成器
    ├── font_charset.txt      字库字符集
    └── build_check.md        编译自检记录（验过什么、没验过什么）
```

**实际编出来的大小**（ESP32S3 Dev Module、3MB App 分区、关掉 ESP_SR）：

```text
Sketch uses 1374593 bytes (43%) of program storage space. Maximum is 3145728 bytes.
Global variables use 71640 bytes (21%) of dynamic memory, leaving 256040 bytes for local variables.
```

---

## 改点东西

**改引脚** → `bsp_config.h`，一处改完到处都是对的。

**改 WiFi / MQTT / 阈值默认值** → `app_config.h`。

**关掉某个功能少占内存** → `app_config.h` 里的 `APP_USE_*` 开关：

```c
#define APP_USE_PANEL  1   // 0 = 不用屏
#define APP_USE_BLE    1   // 0 = 不用手机蓝牙
#define APP_USE_MQTT   1   // 0 = 不联网（MQTT 客户端不编译）
#define APP_USE_KEY    1   // 0 = 没有按键板
#define APP_USE_ESP_SR 1   // 0 = 不用板载唤醒词
```

**改了屏幕上的中文文案** → 新字要进字库，否则屏上是空心方框：

```powershell
C:\Python314\python.exe arduino\SmartHomeArduino\tools\gen_font.py --scan
```

（需要 `pip install pillow`；它扫源码里所有中文字符，重新生成 `ui/panel_font_data.h`）

**改完代码先跑两个自检**（都不烧板子，几秒）：

```powershell
powershell -ExecutionPolicy Bypass -File arduino\SmartHomeArduino\tools\check_style.ps1
powershell -ExecutionPolicy Bypass -File arduino\SmartHomeArduino\tools\check_font.ps1
```

- **check_style.ps1** 查：文件头两行格式、UTF-8 无 BOM、行尾 LF、有没有 Tab、
  注释里有没有分隔线、`#include "xxx.h"` 能不能找到、头文件声明的函数有没有实现
- **check_font.ps1** 查：源码里的汉字是不是都在字模里（缺了屏上就是空心方框）

---

## 排错

| 现象 | 怎么办 |
|---|---|
| 编译报 `ESP_SR.h: No such file` | `APP_USE_ESP_SR` 被改成 1 了，但开发板支持是 2.x。改回 0，或把核心升到 3.0.0 以上 |
| 编译报 `text section exceeds available space in board` | 分区方案没改。核心 3.x 选 `ESP SR 16M`，2.x 选 `16M Flash (3MB APP/9.9MB FATFS)` |
| 编译报 `xxx: No such file or directory`（第三方库名） | 工程不用第三方库。九成是文件名不小心放进子目录了 —— 见「文件都在这儿」那节的警告 |
| 串口全是乱码 | 串口监视器编码改 UTF-8，或 `APP_CONSOLE_ASCII` 改 1 |
| 屏全黑 | 敲 `i2c-scan` 看 0x3C 在不在；在就 `panel-retry`；地址跳线可能是 0x3D |
| 蜂鸣器一直响 | 敲 `cfg buzzer_active_low 1`（或改 0，听 `buzzer-test` 哪段安静） |
| 喊了没反应 | 先看 `APP_USE_ESP_SR` 是不是 0（默认就是 0，本就不识别）。改成 1 之后还不行，串口敲 `hw` 看「板载识别(ESP_SR)」起没起来 |
| 舵机抖动 / 板子重启 | 舵机必须**独立 5V 供电**，并且 GND 和开发板**拧在一起** |
| 配网后网页面板没数据 | 串口敲 `mqtt`，看 base topic；公共 broker 上前缀撞了会串台 |

更多接线与不可用引脚见主工程 [`docs/接线文档.md`](../../docs/接线文档.md)。
