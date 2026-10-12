// c
// MQTT 消息格式
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// JSON 缓冲大小
#define NET_JSON_STATE_LEN   640
#define NET_JSON_SENSOR_LEN  320
#define NET_JSON_WEATHER_LEN 160
#define NET_JSON_CONFIG_LEN  384
#define NET_JSON_ACK_LEN     192

// 缓冲不够别发半截
int net_proto_state_json(char *buf, int len);

// snap 空按无效
int net_proto_sensor_json(char *buf, int len, const sensor_snapshot_t *snap);

int net_proto_weather_json(char *buf, int len, const sensor_snapshot_t *snap);

int net_proto_config_json(char *buf, int len);

int net_proto_ack_json(char *buf, int len, const char *dev, const char *action, bool ok, const char *detail);

int net_proto_event_json(char *buf, int len, const char *what, const char *info);

// 下雨文字给播报
const char *net_proto_weather_text(bool raining);

#ifdef __cplusplus
}
#endif

// 协议表到此结束
