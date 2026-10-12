# 编译自检记录

本机 **没有板子**，所以只做「语法 + 类型 + 头文件 + 整包链接」检查，**不烧板**。
两条腿都走过：

1. 手工 `-fsyntax-only`（下面第一~六节）：每个 `.cpp` 单独过一遍编译器；
2. **arduino-cli 整包编译+链接**（第零节）：真编译器、真核心、真链接。

## 零、整包编译结果（★ 已修好，本工程现在整包能过）

用临时下的 `arduino-cli`（`%TEMP%\sh_arduino\arduino-cli.exe`）配已装的 2.0.9 核心：

```powershell
arduino-cli compile -b "esp32:esp32:esp32s3:PartitionScheme=app3M_fat9M_16MB" arduino\SmartHomeArduino
```

**结果（关掉 ESP_SR，因为 2.0.9 没有这个库）：编译 + 链接全过**

```text
Sketch uses 1374593 bytes (43%) of program storage space. Maximum is 3145728 bytes.
Global variables use 71640 bytes (21%) of dynamic memory, leaving 256040 bytes for local variables.
```

### 布局：必须全平铺（这是踩过的坑）

arduino-cli 实测（2.0.9）：

| 布局 | 结果 |
| --- | --- |
| 源文件放子目录（原来的 `bsp/`、`ui/`） | **子目录里的 .cpp 根本不参与编译** → `undefined reference to xxx()` |
| 子目录的 .h 去 include 根目录的 .h | `fatal error: app_common.h: No such file or directory` |
| 全部放 `src/` 子目录（Arduino 的 src 约定） | 同样不行 |
| **全部平铺在 sketch 根目录** | **通过** |

**只有 sketch 根目录在 include 路径上，也只有根目录的源文件会被编译。**
所以 `bsp/`、`ui/` 已经全部挪到根目录（文件名不变，只是不再有目录层级），
`#include "bsp/led.h"` 之类也改成了 `#include "led.h"`。

### 这一轮整包编译真抓到的 bug（都已修）

| 文件 | 问题 | 处理 |
| --- | --- | --- |
| `net_link.cpp` | 用了 `APP_MQTT_PREFIX` 等宏，却没 include `app_config.h` | 补 include |
| `home_auto.cpp` | 用了 `APP_MANUAL_GUARD_MS`，同样没 include | 补 include |
| `ble_app.cpp` | 用了 `auto_policy_set_threshold` / `APP_BLE_NAME_PREFIX` / `APP_WIFI_*`，缺 `automation.h` 与 `app_config.h` | 补 include |
| `ble_app.cpp` | `#include "BLE.h"` —— 核心库里没这个头文件 | 改成 `#include "BLEDevice.h"` |
| `ble_app.cpp` | `onStatus()` 里用了不存在的 `SUCCESS` / `NOTIFY_DISABLED` | 真实枚举是 `SUCCESS_NOTIFY` / `SUCCESS_INDICATE` / `ERROR_NOTIFY_DISABLED` |
| `bsp_config.h` | 漏了 `BSP_I2S_MIC_SCK/WS/SD_GPIO`、`BSP_I2S_MIC_SAMPLE_RATE` | 补上（GPIO38/39/40 @16000Hz） |
| `bsp_config.h` | `BSP_SERVO_RES 16` 在 S3 上不可用（LEDC 计数器只有 14 位） | 改成 14，和出货固件一致 |
| `bsp_config.h` | `BSP_RAIN_ADC_INVERT 1` 与主工程 `sensor.c` 的 0 冲突 | 改成 0，行为与出货固件一致 |

> 前面那 8 个 bug **只有整包编译才会暴露** —— 手工 `-fsyntax-only` 单个文件是看不出来的。

## 零点五、手工 `-fsyntax-only` 的历史记录（下面第一~六节）

下面是引入 arduino-cli 之前，用工具链逐文件过编译器的记录，保留作参考。
`bsp/`、`ui/` 这种子目录既不进 include，也不进编译列表。

修法（二选一，推荐第一个）：

1. **平铺**：把 `bsp/*.h`、`bsp/*.cpp`、`ui/*.h`、`ui/*.cpp` 全部挪到 sketch 根目录，
   同时把 `#include "bsp/xxx.h"` / `"ui/xxx.h"` 改成 `"xxx.h"`（`bsp/led.h` 这类头文件里
   的 `"app_common.h"` 不用动，同一个目录就能找到）。
2. 只把 `.ino` 留下来、其它全部挪进 `src/` 并改 `.ino` 的 include —— **实测不行**，别选。

我这一层的 7 个文件是按方案 1 验的：把 `"bsp/xxx.h"` 改成 `"xxx.h"` 平铺之后，
`arduino-cli compile` 一次过（见上面的 309889 bytes）。

## 一、环境（本机实测）

| 项 | 值 |
| --- | --- |
| arduino-esp32 核心 | **2.0.9**（内置 ESP-IDF **v4.4.4**） |
| 工具链 | `xtensa-esp32s3-elf-g++` `esp-2021r2-patch5-8.4.0` |
| 目标 | ESP32-S3（`variants/esp32s3`） |

## 二、include 路径怎么来的（不是猜的）

直接从核心的 `platform.txt` 里抄 `compiler.cpreprocessor.flags.esp32s3` 这一行
（里面是官方那 200 来条 `-I`），再做三件事：

1. `{compiler.sdk.path}` 换成 `<core>\tools\sdk\esp32s3`；
2. **反斜杠全换成正斜杠** —— gcc 读 `@响应文件` 时会把 `\` 当转义符，
   留着反斜杠会出现 `Arduino.h: No such file or directory`；
3. 补上 `-I<core>\tools\sdk\esp32s3\qio_qspi\include`（生成出来的 `sdkconfig.h` 在这），
   以及 `cores\esp32`、`variants\esp32s3`、`libraries\Wire\src`、工程根目录。

检查用的响应文件（`@xxx.rsp`）里最终是这些开关：

```text
<platform.txt 抄来的全部 -I>
-I<core>/tools/sdk/esp32s3/qio_qspi/include
-DESP32 -DESP32S3 -DARDUINO=200 -DARDUINO_ARCH_ESP32 -DARDUINO_ESP32S3_DEV
-DARDUINO_BOARD="ESP32S3_DEV" -DARDUINO_VARIANT="esp32s3" -DARDUINO_USB_MODE=1
-DHTTP_ANY=0 -DF_CPU=240000000
-I<core>/cores/esp32 -I<core>/variants/esp32s3 -I<core>/libraries/Wire/src -I<工程根>
-w -fsyntax-only -std=gnu++17
```

单文件命令就是：

```bash
xtensa-esp32s3-elf-g++.exe @esp32s3.rsp <文件>.cpp
```

`-std` 跑了**两遍**：`gnu++17`（本任务要求）和 `gnu++11`
（2.0.9 的 `compiler.cpp.flags.esp32s3` 实际就是 `gnu++11`，见 `platform.txt`）。

## 三、bsp/ 七个文件的结果

| 文件 | `-std=gnu++17` | `-std=gnu++11` |
| --- | --- | --- |
| `bsp/i2c_bus.cpp` | 通过 | 通过 |
| `bsp/gpio_util.cpp` | 通过 | 通过 |
| `bsp/led.cpp` | 通过 | 通过 |
| `bsp/servo.cpp` | 通过 | 通过 |
| `bsp/buzzer.cpp` | 通过 | 通过 |
| `bsp/key.cpp` | 通过 | 通过 |
| `bsp/sensor.cpp` | 通过 | 通过 |

只加 `-w`（关库的警告），**没有加任何屏蔽错误的开关**。
另外把每个文件编成 `.o` 后用 `nm` 对了一遍头文件里的声明，函数一个不少。

> **更正**：`xtensa_context.h` 本机是**有的**，不是 SDK 缺文件 ——
> `sdk\esp32s3\include\freertos\port\xtensa\include\freertos\xtensa_context.h`
> 和 `sdk\esp32s3\include\xtensa\include\xtensa\xtensa_context.h` 都在。
> 编不动是 include 只加了 `sdk/esp32s3/include` 的**第一层**子目录，
> 少了 `freertos/port/xtensa/include`、`xtensa/include`、`hal/esp32s3/include`
> 这类深一层的路径。按第二节（照抄 `platform.txt` 那一行）拼 include 之后，
> `bsp/` 七个同样 `#include <Arduino.h>` 的文件全部编过 ——
> 也就是说**本机其实可以把整个工程过一遍语法**，不需要等甲方装核心。

## 四、顺手查的“另一条分支”

`#if` 里的代码不编就等于没写，所以用「先 include 配置头、再 `#undef`/`#define` 覆盖、
最后 include 源文件」的办法，把每条分支都编了一遍：

| 变的宏 | 想查的分支 | 结果 |
| --- | --- | --- |
| `BSP_BUZZER_ACTIVE 0` | 无源蜂鸣器（LEDC 2kHz + LEDC 失败降级 GPIO） | 通过 |
| `BSP_LED_ACTIVE_LOW 1` | 低电平点亮（占空比反相） | 通过 |
| `BSP_STATUS_LED_ENABLE 0` | 状态灯整体关掉的空实现 | 通过 |
| `SENSORS_RAIN_ADC_INVERT 1` | 雨滴反相 | 通过 |
| `BSP_LIGHT_ADC_INVERT 0` | 光敏不反相 | 通过 |

> 这一步**真抓到过 bug**：`BSP_BUZZER_ACTIVE 0` 时 `buzzer_hw_write()` 被定义了两次
> （原 IDF 工程也有这个问题，只是它一直是 1 所以没暴露）。已修。

## 五、core 3.x 是怎么查的

本机只装了 2.0.9，`tools/sdk` 里只有 IDF 4.4 的头文件。但
`%LOCALAPPDATA%\Arduino15\staging\packages\esp32-libs-3.3.7.zip`
里有 **arduino-esp32 3.3.7 的真实 IDF 5.x 头文件**，于是：

1. 把 `driver/rmt_tx.h`、`driver/rmt_encoder.h`、`driver/rmt_common.h`、
   `driver/rmt_types.h`、`driver/ledc.h`、`hal/rmt_types.h`、`hal/ledc_types.h`
   抠出来，放进单独的 include 目录（排在 2.0.9 的路径前面）；
2. 把 `ESP_ARDUINO_VERSION_MAJOR` 顶成 3，编 `bsp/gpio_util.cpp`
   （状态灯 WS2812 的 3.x 分支）→ **通过**；
3. 同时核对了用到的每个字段/函数名（`mem_block_symbols`、`flags.init_level`、
   `flags.eot_level`、`flags.msb_first`、`rmt_tx_wait_all_done`、`rmt_del_encoder` …），
   全部与真实 5.x 头文件一致。

顺带发现并处理了两处 4.4 → 5.x 的改名：

| 4.4（core 2.x） | 5.x（core 3.x） | 处理 |
| --- | --- | --- |
| `SOC_LEDC_TIMER_BIT_WIDE_NUM` | `SOC_LEDC_TIMER_BIT_WIDTH` | `servo.cpp` 里加了别名兜底 |
| `ADC_11db` | `ADC_ATTEN_DB_12` | `sensor.cpp` 里 `#if` 版本 + `#ifndef` 兜底 |

舵机分辨率的降位判断也用 `static_assert` 验过三种组合：
S3（上限 14）→ 压到 14；上限 20 的芯片 → 保留配置里的 16；
5.x 的新宏名下同样成立。

## 六、明确**没**验证到的

1. **没上过板子**。所有“能不能动”的判断（舵机脉宽、雨滴阈值、光敏窗口）
   都来自原 IDF 工程，没有实测。
2. **core 3.x 的 Arduino 层 API 没有 3.x 核心可编**：`analogSetPinAttenuation()`、
   `ADC_ATTEN_DB_12`、`Wire` 这几处只能靠 `#if ESP_ARDUINO_VERSION_MAJOR` 兜底，
   本机没有 3.x 核心，编不了真机版本。3.x 的 **IDF 层**（RMT / LEDC）已经用真实头文件验过。
3. **`esp32-hal-rmt.h`（2.x 状态灯用）只在本机 2.0.9 编过**，没法在 3.x 上编（3.x 已删该头文件，
   所以 3.x 走的是 `driver/rmt_tx.h` 那条新分支）。
4. **RMT 时序只有理论值**：3/9/9/3 个 100ns tick（抄原工程），没有示波器看过波形。
5. **LEDC 通道/定时器分配**是按“舵机 ch0/timer0、蜂鸣器 ch1/timer1、四路灯 ch2~ch5/timer2”
   排的，编译期无法验证运行时是否真的独占，只能靠 IDF 的 `timer_sel` 语义（低速模式任选定时器）。
