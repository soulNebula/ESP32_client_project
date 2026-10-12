// c
// MQTT topic 与 JSON 生成

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_protocol.h"

#include "device_model.h"
#include "automation.h"
#include "net_link.h"

static const char *TAG = "PROTO";

// 退化 JSON 的暂存
#define PROTO_MIN_SCRATCH 160

// 全 0 快照兜底
static const sensor_snapshot_t s_zero_snap = {0};

// 真值写 JSON
static const char *b2s(bool v) {
    return v ? "true" : "false";
}

// 定点一位小数
static void fmt_f1(char *buf, int len, float v) {
    if (buf == NULL || len <= 0) {
        return;
    }
    // NaN 自比不等
    if (v != v) {
        snprintf(buf, (size_t)len, "0.0");
        return;
    }
    if (v > 9999.0f) {
        v = 9999.0f;
    } else if (v < -999.0f) {
        v = -999.0f;
    }

    const bool neg = (v < 0.0f);
    const int  i10 = (int)((neg ? -v : v) * 10.0f + 0.5f);
    snprintf(buf, (size_t)len, "%s%d.%d", neg ? "-" : "", i10 / 10, i10 % 10);
}

// 拼接并推进下标
static int appendf(char *buf, int len, int used, const char *fmt, ...) {
    if (buf == NULL || fmt == NULL || used < 0 || used >= len) {
        return -1;
    }

    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + used, (size_t)(len - used), fmt, ap);
    va_end(ap);

    if (n < 0 || (used + n) >= len) {
        // 截断了就算失败
        return -1;
    }
    return used + n;
}

// 转义后追加
static int append_escaped(char *buf, int len, int used, const char *s) {
    if (buf == NULL || used < 0 || used >= len) {
        return -1;
    }
    if (s == NULL) {
        s = "";
    }

    for (const char *p = s; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        // 控制字符一律丢
        if (c < 0x20) {
            continue;
        }

        const char *esc = NULL;
        switch (c) {
        case '\"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        default:
            break;
        }

        if (esc != NULL) {
            if ((used + 2) >= len) {
                return -1;
            }
            buf[used++] = esc[0];
            buf[used++] = esc[1];
        } else {
            if ((used + 1) >= len) {
                return -1;
            }
            buf[used++] = (char)c;
        }
    }

    buf[used] = '\0';
    return used;
}

// 整条放下才返回
static int put_whole(char *buf, int len, const char *json) {
    if (buf == NULL || len <= 0 || json == NULL) {
        return -1;
    }

    const int n = snprintf(buf, (size_t)len, "%s", json);
    if (n < 0 || n >= len) {
        // 绝不发半截 JSON
        LOGE(TAG, "JSON 放不下(需 %d，有 %d)，本条丢弃", n, len);
        return -1;
    }
    return n;
}

// 放不下再试一次
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
        LOGE(TAG, "退化 JSON 自身超长(%d)，放弃本条上报", n);
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
                       mode_name, b2s(auto_policy_is_auto()), b2s(net_link_wifi_ok()), b2s(net_link_mqtt_ok()),
                       (int)net_link_rssi(),
                       // ip 不用转义
                       ip, (int)(app_uptime_ms() / 1000));
    }

    if (used >= 0 && used < len) {
        return used;
    }

    // 退化只发模式
    LOGW(TAG, "state JSON 放不下（%d 字节），退化为最小合法 JSON", len);
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
    const auto_cfg_t *cfg        = auto_policy_cfg();
    const bool        temp_alert = snap->th_valid && (cfg != NULL) && (snap->temperature >= cfg->temp_alert_c);

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
    LOGW(TAG, "sensor JSON 放不下（%d 字节），退化为最小合法 JSON", len);
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

    LOGW(TAG, "weather JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"raining\":%s,\"text\":\"%s\"}", b2s(snap->raining), text);
}

// 必须 '}' 收尾
int net_proto_config_json(char *buf, int len) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    const int n = auto_policy_cfg_to_json(buf, len);
    if (n <= 0) {
        LOGW(TAG, "auto_policy_cfg_to_json 返回 %d，放弃本次 config 上报", n);
        return -1;
    }
    if (n >= len) {
        LOGE(TAG, "config JSON 返回值越界(%d >= %d)，丢弃", n, len);
        return -1;
    }
    if (buf[n - 1] != '}') {
        LOGE(TAG, "config JSON 未以 '}' 收尾（很可能被截断），丢弃本条");
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
    LOGW(TAG, "ack JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"dev\":\"\",\"action\":\"\",\"ok\":%s,\"detail\":\"ack 缓冲不足\"}",
                       b2s(ok));
}

// uptime 单位秒
int net_proto_event_json(char *buf, int len, const char *what, const char *info) {
    if (buf == NULL || len <= 0) {
        return -1;
    }

    const int uptime = (int)(app_uptime_ms() / 1000);

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

    LOGW(TAG, "event JSON 放不下（%d 字节），退化为最小合法 JSON", len);
    return put_minimal(buf, len, "{\"what\":\"event\",\"uptime\":%d}", uptime);
}

// 单色屏没有彩色
const char *net_proto_weather_text(bool raining) {
    return raining ? "Raining!" : "Sunny";
}
