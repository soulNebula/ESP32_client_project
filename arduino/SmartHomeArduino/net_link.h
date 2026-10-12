// c
// 联网接口
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_common.h"
#include "sensor.h"

typedef enum {
    NET_STATE_DOWN = 0,
    NET_STATE_WIFI_OK,
    NET_STATE_MQTT_OK,
} net_state_t;

#define NET_UID_LEN 8

#define NET_RX_PAYLOAD_MAX 256

// 起来才算成功
app_err_t net_link_init(void);

// 主循环里轮询
void net_link_tick(void);

void net_link_stop(void);

// 现在连到哪一步了
net_state_t net_link_get_state(void);

bool net_link_wifi_ok(void);

bool net_link_mqtt_ok(void);

// MAC 后 3 字节
const char *net_link_uid(void);

const char *net_link_ip(void);

int8_t net_link_rssi(void);

// topic 前缀
const char *net_link_topic_base(void);

// 串口调试用
void net_link_dump(void);

// 状态发到 base
app_err_t net_link_publish_state(void);

// 2s 一次
app_err_t net_link_publish_sensor(const sensor_snapshot_t *snap);

// 天气发卡片
app_err_t net_link_publish_weather(const sensor_snapshot_t *snap);

app_err_t net_link_publish_config(void);

app_err_t net_link_publish_ack(const char *dev, const char *action, bool ok, const char *detail);

// 事件，info 可空
app_err_t net_link_publish_event(const char *what, const char *info);

// 面板重连时重发
app_err_t net_link_publish_all(void);

// MQTT/蓝牙共用
// detail 装原因
app_err_t net_link_handle_cmd_payload(const char *data, int len, char *detail, size_t detail_len);

// 换 WiFi 并重连
// 空 ssid 回默认
app_err_t net_link_set_wifi(const char *ssid, const char *pass);

// 手机 App 下发的那份
const char *net_link_get_wifi_ssid(void);
