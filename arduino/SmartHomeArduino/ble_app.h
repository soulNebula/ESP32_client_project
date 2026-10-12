// c
// 蓝牙控制通道接口
#pragma once
#include <stdbool.h>
#include "app_common.h"

// 起蓝牙并开广播
// 不等连接，失败只返错
app_err_t ble_app_start(void);

// 手机连上没有
bool ble_app_is_connected(void);

// 上行五路
// len 不含帧头
void ble_app_notify_state(const char *json, int len);

void ble_app_notify_sensor(const char *json, int len);

void ble_app_notify_weather(const char *json, int len);

void ble_app_notify_config(const char *json, int len);

void ble_app_notify_ack(const char *json, int len);

// 自己拼 INFO 发
void ble_app_notify_info(void);

// 广播名带后 6 位 ID
const char *ble_app_name(void);
