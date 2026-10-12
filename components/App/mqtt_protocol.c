// c
// JSON 拼装

#include "mqtt_protocol.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "net_link.h"
#include "device_model.h"
#include "automation.h"
#include "sensor.h"


static const char *TAG = "PROTO";

// 退化 JSON 缓冲
#define PROTO_MIN_SCRATCH 192

// NULL 兜底快照
static const sensor_snapshot_t s_zero_snap;

// 布尔转 JSON
static const char *b2s(bool v) {
    return v ? "true" : "false";
}

// 一位小数
// NaN 当 0 处理
static void fmt_f1(char *buf, size_t len, float v) {
    if (buf == NULL || len == 0) {
        return;
    }
    if (!(v > -100000.0f && v < 100000.0f)) {
        v = 0.0f;
    }

    int        i10 = (int)(v * 10.0f + ((v >= 0.0f) ? 0.5f : -0.5f));
    const bool neg = (i10 < 0);
    if (neg) {
        i10 = -i10;
    }
    snprintf(buf, len, "%s%d.%d", neg ? "-" : "", i10 / 10, i10 % 10);
}

// 追加失败返 -1
static int appendf(char *buf, int len, int used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static int appendf(char *buf, int len, int used, const char *fmt, ...) {
    if (buf == NULL || len <= 0 || used < 0 || used >= len || fmt == NULL) {
        return -1;
    }

    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + used, (size_t)(len - used), fmt, ap);
    va_end(ap);

    if (n < 0 || used + n >= len) {
        return -1;
    }
    return used + n;
}

// 只转引号反斜杠
// 控制符换空格
static int append_escaped(char *buf, int len, int used, const char *src) {
    if (buf == NULL || len <= 0 || used < 0 || used >= len) {
        return -1;
    }

    size_t o = (size_t)used;
    for (const char *p = (src != NULL) ? src : ""; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 > (size_t)len) {
                return -1;
            }
            buf[o++] = '\\';
            buf[o++] = *p;
        } else if ((unsigned char)*p < 0x20u) {
            if (o + 2 > (size_t)len) {
                return -1;
            }
            buf[o++] = ' ';
        } else {
            if (o + 2 > (size_t)len) {
                return -1;
            }
            buf[o++] = *p;
        }
    }

    buf[o] = '\0';
    return (int)o;
}

// 放不下就清空
static int put_whole(char *buf, int len, const char *json) {
    if (buf == NULL || len <= 0 || json == NULL) {
        return -1;
    }

    const int n = snprintf(buf, (size_t)len, "%s", json);
    if (n < 0 || n >= len) {
        buf[0] = '\0';
        return -1;
    }
    return n;
}

// 退化：短但合法
static int put_minimal(char *buf, int len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int put_minimal(char *buf, int len, const char *fmt, ...) {
    char scratch[PROTO_MIN_SCRATCH];

    if (buf == NULL || len <= 0 || fmt == NULL) {
        return -1;
    }

    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);

    if (n < 0 || n >= (int)sizeof(scratch)) {
        ESP_LOGE(TAG, "退化 JSON 自身超长(%d)，放弃本条上报", n);
        return -1;
    }
    return put_whole(buf, len, scratch);
}

// 字段顺序是契约
// 截断常丢 '}'
int net_proto_state_json(char *buf, int len) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    // 一次拷完省锁
    device_state_t st[DEV_MAX] = {0};
    home_ctrl_get_all(st, (int)DEV_MAX);

    const auto_mode_t mode      = auto_policy_get_mode();
    const char       *mode_name = auto_policy_mode_name(mode);
    if (mode_name == NULL) {
        // 防御 NULL
        mode_name = "manual";
    }
    const char *ip = net_link_ip();
    if (ip == NULL) {
        ip = "0.0.0.0";
    }

    // 第 1 段
    int used =
        snprintf(buf, (size_t)len,
                 "{\"led_living\":{\"power\":%s,\"level\":%d},"
                 "\"led_kitchen\":{\"power\":%s,\"level\":%d},"
                 "\"led_bedroom\":{\"power\":%s,\"level\":%d},"
                 "\"led_bath\":{\"power\":%s,\"level\":%d},"
                 "\"door\":{\"power\":%s,\"level\":%d},"
                 "\"buzzer\":{\"power\":%s,\"muted\":%s}",
                 b2s(st[DEV_LED_LIVING].power), st[DEV_LED_LIVING].level, b2s(st[DEV_LED_KITCHEN].power),
                 st[DEV_LED_KITCHEN].level, b2s(st[DEV_LED_BEDROOM].power), st[DEV_LED_BEDROOM].level,
                 b2s(st[DEV_LED_BATH].power), st[DEV_LED_BATH].level, b2s(st[DEV_DOOR].power),
                 st[DEV_DOOR].level, b2s(st[DEV_BUZZER].power), b2s(auto_policy_buzzer_muted()));

    // 第 2 段
    if (used >= 0 && used < len) {
        used = appendf(buf, len, used,
                       ",\"mode\":\"%s\",\"auto\":%s,\"wifi\":%s,\"mqtt\":%s,"
                       "\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%d}",
                       mode_name, b2s(auto_policy_is_auto()), b2s(net_link_wifi_ok()),
                       b2s(net_link_mqtt_ok()), (int)net_link_rssi(),
                       // ip 不用转义
                       ip, (int)(esp_timer_get_time() / 1000000LL));
    }

    if (used >= 0 && used < len) {
        return used;
    }

    // 退化只发模式
    ESP_LOGW(TAG, "state JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"mode\":\"%s\",\"auto\":%s,\"wifi\":%s,\"mqtt\":%s}", mode_name,
                       b2s(auto_policy_is_auto()), b2s(net_link_wifi_ok()), b2s(net_link_mqtt_ok()));
}

int net_proto_sensor_json(char *buf, int len, const sensor_snapshot_t *snap) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    if (snap == NULL) {
        snap = &s_zero_snap;
    }

    char t[16], h[16], lx[16], lp[16], r[16];
    fmt_f1(t, sizeof(t), snap->temperature);
    fmt_f1(h, sizeof(h), snap->humidity);
    fmt_f1(lx, sizeof(lx), snap->light_lux);
    fmt_f1(lp, sizeof(lp), snap->light_pct);
    fmt_f1(r, sizeof(r), snap->rain_pct);

    // 阈值不写死
    const auto_cfg_t *cfg = auto_policy_cfg();
    const bool temp_alert = snap->th_valid && (cfg != NULL) && (snap->temperature >= cfg->temp_alert_c);

    const int used = snprintf(buf, (size_t)len,
                              "{\"temp\":%s,\"humi\":%s,\"temp_valid\":%s,\"temp_alert\":%s,"
                              "\"lux\":%s,\"light_pct\":%s,\"light_is_lux\":%s,\"light_valid\":%s,"
                              "\"rain\":%s,\"raining\":%s,\"rain_valid\":%s}",
                              t, h, b2s(snap->th_valid), b2s(temp_alert), lx, lp,
                              b2s(snap->light_kind == SENSOR_LIGHT_BH1750), b2s(snap->light_valid), r,
                              b2s(snap->raining), b2s(snap->rain_valid));

    if (used >= 0 && used < len) {
        return used;
    }

    // 退化只发温湿度
    ESP_LOGW(TAG, "sensor JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len,
                       "{\"temp\":%s,\"humi\":%s,\"temp_valid\":%s,\"temp_alert\":%s,"
                       "\"rain_valid\":%s}",
                       t, h, b2s(snap->th_valid), b2s(temp_alert), b2s(snap->rain_valid));
}

// lux 不放这里
int net_proto_weather_json(char *buf, int len, const sensor_snapshot_t *snap) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    if (snap == NULL) {
        snap = &s_zero_snap;
    }

    char r[16];
    fmt_f1(r, sizeof(r), snap->rain_pct);

    // 静态串免转义
    const char *text = net_proto_weather_text(snap->raining);

    const int used = snprintf(buf, (size_t)len, "{\"raining\":%s,\"text\":\"%s\",\"rain_pct\":%s}",
                              b2s(snap->raining), text, r);
    if (used >= 0 && used < len) {
        return used;
    }

    ESP_LOGW(TAG, "weather JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"raining\":%s,\"text\":\"%s\"}", b2s(snap->raining), text);
}

// 必须 '}' 收尾
int net_proto_config_json(char *buf, int len) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    const int n = auto_policy_cfg_to_json(buf, len);
    if (n <= 0) {
        ESP_LOGW(TAG, "auto_policy_cfg_to_json 返回 %d，放弃本次 config 上报", n);
        return -1;
    }
    if (n >= len) {
        ESP_LOGE(TAG, "config JSON 返回值越界(%d >= %d)，丢弃", n, len);
        return -1;
    }
    if (buf[n - 1] != '}') {
        ESP_LOGE(TAG, "config JSON 未以 '}' 收尾（很可能被截断），丢弃本条");
        return -1;
    }
    return n;
}

// 外部输入要转义
int net_proto_ack_json(char *buf, int len, const char *dev, const char *action, bool ok, const char *detail) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    int used = snprintf(buf, (size_t)len, "{\"dev\":\"");
    if (used >= 0) {
        used = append_escaped(buf, len, used, dev);
    }
    if (used >= 0) {
        used = appendf(buf, len, used, "\",\"action\":\"");
    }
    if (used >= 0) {
        used = append_escaped(buf, len, used, action);
    }
    if (used >= 0) {
        used = appendf(buf, len, used, "\",\"ok\":%s,\"detail\":\"", b2s(ok));
    }
    if (used >= 0) {
        used = append_escaped(buf, len, used, detail);
    }
    if (used >= 0) {
        used = appendf(buf, len, used, "\"}");
    }
    if (used >= 0) {
        return used;
    }

    // 退化不带外部串
    ESP_LOGW(TAG, "ack JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"dev\":\"\",\"action\":\"\",\"ok\":%s,\"detail\":\"ack 缓冲不足\"}",
                       b2s(ok));
}

// uptime 单位秒
int net_proto_event_json(char *buf, int len, const char *what, const char *info) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    const int uptime = (int)(esp_timer_get_time() / 1000000LL);

    int used = snprintf(buf, (size_t)len, "{\"what\":\"");
    if (used >= 0) {
        used = append_escaped(buf, len, used, what);
    }
    if (used >= 0) {
        used = appendf(buf, len, used, "\",\"info\":\"");
    }
    if (used >= 0) {
        used = append_escaped(buf, len, used, info);
    }
    if (used >= 0) {
        used = appendf(buf, len, used, "\",\"uptime\":%d}", uptime);
    }
    if (used >= 0) {
        return used;
    }

    ESP_LOGW(TAG, "event JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"what\":\"event\",\"uptime\":%d}", uptime);
}

// 天气标签（无雨用英文 Sunny：与 "Raining!" 同为 ASCII，
// 屏上那行也不用再依赖中文字库里有"晴"字）
const char *net_proto_weather_text(bool raining) {
    return raining ? "Raining!" : "Sunny";
}
