// c
// 编译期默认值，改了重编
#pragma once

// "1" 才编译进去
// 不装库就先关掉
#define APP_USE_PANEL 1
#define APP_USE_BLE   1
#define APP_USE_MQTT  1
#define APP_USE_AUTO  1
#define APP_USE_KEY   1

// 板载唤醒词 + 命令词（用 arduino-esp32 自带的 ESP_SR 库）
//
// ★ 默认 0：任何版本的核心都能直接编译。
//   ESP_SR 是【核心 3.x 才有的库】，2.x 里没有这个头文件，
//   开着它 + 用 2.x 编，会直接停在：fatal error: ESP_SR.h: No such file
//
// ★ 想要"喊一声就能控"（和出货固件一样），做两步：
//   ① 开发板管理器里把 esp32 升级到 3.0.0 以上
//   ② 把下面这行改成 1，并把分区方案选成
//      "ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"
//
// 不开也能用：手机 App 与串口 say <短名> 走【和真语音完全同一条下游】
#define APP_USE_ESP_SR 0

// 英文串口台
// 中文台要开串口监视器的 UTF-8
#define APP_CONSOLE_ASCII 0

// 空则不连网
// 也能用 App 配
#define APP_WIFI_SSID ""
#define APP_WIFI_PASS ""

// 公共测试 broker
#define APP_MQTT_HOST "broker.emqx.io"
#define APP_MQTT_PORT 1883
// 公共 broker 上前缀撞了会看到别人的设备
#define APP_MQTT_PREFIX "smarthome"

// 广播名前缀
#define APP_BLE_NAME_PREFIX "SmartHome"

// 主循环节拍
#define APP_LOOP_TICK_MS 500

// 状态灯节流
#define APP_STATUS_LED_PERIOD_MS 1000

// 传感器采样周期
#define APP_SENSOR_PERIOD_MS 1000

// 刷屏周期
#define APP_DISPLAY_REFRESH_MS 3000

// 周期上报
#define APP_MQTT_PUBLISH_SENSOR_MS 2000

// 变化合并窗口
#define APP_STATE_COALESCE_MS 200

// 手动保护期
#define APP_MANUAL_GUARD_MS 3000

// 开门免打扰
#define APP_CASCADE_GUARD_MS 30000

// 语音反馈条显示时长
#define APP_VOICE_BANNER_MS 3000

// 桥接 16k 单声道
#define APP_VOICE_SAMPLE_RATE 16000

// 命令行长度上限
#define APP_CONSOLE_LINE_MAX 160
