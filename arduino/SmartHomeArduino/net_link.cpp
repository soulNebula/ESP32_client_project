// c
// WiFi 与 MQTT 客户端

#include <stdio.h>
#include <string.h>

#include "WiFi.h"
#include "Preferences.h"

#include "app_config.h"

#include "net_link.h"
#include "mqtt_protocol.h"
#include "app_cmd.h"
// 蓝牙同时来一份
#include "ble_app.h"

static const char *TAG = "NET";

// 最长 topic
#define NET_TOPIC_LEN 96

// 重连节流
#define NET_WIFI_RETRY_MS 5000
#define NET_MQTT_RETRY_MS 5000

// MQTT 保活秒数
#define NET_MQTT_KEEPALIVE 30

// 收发缓冲
#define NET_TX_BUF_LEN 768
#define NET_RX_BUF_LEN 384

// 剩余长度最多 4 字节
#define NET_REMAIN_MAX 4

// 重发标志
static bool s_need_republish = true;

static char s_uid[NET_UID_LEN];
static char s_ip[16]        = "0.0.0.0";
static char s_client_id[32] = {0};

static char s_base[64]             = {0};
static char s_topic_state[NET_TOPIC_LEN]        = {0};
static char s_topic_sensor[NET_TOPIC_LEN]       = {0};
static char s_topic_weather[NET_TOPIC_LEN]      = {0};
static char s_topic_config[NET_TOPIC_LEN]       = {0};
static char s_topic_ack[NET_TOPIC_LEN]          = {0};
static char s_topic_event[NET_TOPIC_LEN]        = {0};
static char s_topic_cmd[NET_TOPIC_LEN]          = {0};
static char s_topic_cmd_sub[NET_TOPIC_LEN]      = {0};
static char s_topic_get[NET_TOPIC_LEN]          = {0};
static char s_topic_availability[NET_TOPIC_LEN] = {0};

// 上次发过的 config
static char s_last_pub_config[NET_JSON_CONFIG_LEN] = {0};

static WiFiClient s_sock;

static bool s_inited;
static bool s_mqtt_ok;

// 连上要先订阅
static bool s_need_subscribe;

static uint32_t s_wifi_retry_at;
static uint32_t s_mqtt_retry_at;
static uint32_t s_last_ping_ms;
// 等 PINGRESP
static bool s_ping_out;

// 收包状态机
static uint8_t s_rx[NET_RX_BUF_LEN];
static int     s_rx_len;

// App 下发的那份
static char s_wifi_ssid[33];
static char s_wifi_pass[65];

// 计数
static uint16_t s_packet_id = 1;

// 从 Preferences 读 App 下发的
static void wifi_load_saved(void) {
    Preferences prefs;
    if (!prefs.begin("wifi_cfg", true)) {
        return;
    }

    const size_t n = prefs.getString("ssid", s_wifi_ssid, sizeof(s_wifi_ssid));
    prefs.getString("pass", s_wifi_pass, sizeof(s_wifi_pass));
    prefs.end();

    if (n > 0) {
        LOGI(TAG, "WiFi 用手机 App 下发的那份: %s", s_wifi_ssid);
    }
}

// 客户端 ID 带 UID
static void make_uid(void) {
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);

    snprintf(s_uid, sizeof(s_uid), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "sh-%s", s_uid);
}

static void topics_reset(void) {
    s_base[0]             = '\0';
    s_topic_state[0]      = '\0';
    s_topic_sensor[0]     = '\0';
    s_topic_weather[0]    = '\0';
    s_topic_config[0]     = '\0';
    s_topic_ack[0]        = '\0';
    s_topic_event[0]      = '\0';
    s_topic_cmd[0]        = '\0';
    s_topic_cmd_sub[0]    = '\0';
    s_topic_get[0]        = '\0';
    s_topic_availability[0] = '\0';
}

static app_err_t topics_build(void) {
    topics_reset();

    if (s_uid[0] == '\0') {
        make_uid();
    }

    const int n = snprintf(s_base, sizeof(s_base), "%s/%s", APP_MQTT_PREFIX, s_uid);
    if (n < 0 || n >= (int)sizeof(s_base)) {
        LOGE(TAG, "base topic 太长（前缀 \"%s\" + UID \"%s\"，上限 %d 字节）", APP_MQTT_PREFIX, s_uid,
             (int)sizeof(s_base) - 1);
        return APP_ERR_INVALID_ARG;
    }

    struct topic_slot {
        char       *dst;
        size_t      cap;
        const char *suffix;
    };

    const struct topic_slot table[] = {
        {       s_topic_state,        sizeof(s_topic_state),        "/state"},
        {      s_topic_sensor,       sizeof(s_topic_sensor),       "/sensor"},
        {     s_topic_weather,      sizeof(s_topic_weather),      "/weather"},
        {      s_topic_config,       sizeof(s_topic_config),       "/config"},
        {         s_topic_ack,          sizeof(s_topic_ack),          "/ack"},
        {       s_topic_event,        sizeof(s_topic_event),        "/event"},
        {         s_topic_cmd,          sizeof(s_topic_cmd),          "/cmd"},
        {     s_topic_cmd_sub,      sizeof(s_topic_cmd_sub),        "/cmd/#"},
        {         s_topic_get,          sizeof(s_topic_get),          "/get"},
        {s_topic_availability, sizeof(s_topic_availability), "/availability"},
    };

    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) {
        const int m = snprintf(table[i].dst, table[i].cap, "%s%s", s_base, table[i].suffix);
        if (m < 0 || m >= (int)table[i].cap) {
            LOGE(TAG, "topic \"%s%s\" 放不下（上限 %d 字节）", s_base, table[i].suffix, (int)table[i].cap - 1);
            return APP_ERR_INVALID_ARG;
        }
    }
    return APP_OK;
}

// 在 s_rx 后面追加字节
static bool rx_push(const uint8_t *data, int len) {
    if (len <= 0) {
        return true;
    }
    if (s_rx_len + len > NET_RX_BUF_LEN) {
        LOGW(TAG, "接收缓冲溢出（%d + %d），已丢弃整段", s_rx_len, len);
        s_rx_len = 0;
        return false;
    }

    memcpy(s_rx + s_rx_len, data, (size_t)len);
    s_rx_len += len;
    return true;
}

// 往 socket 里写满
static bool sock_write(const uint8_t *data, int len) {
    if (!s_sock.connected()) {
        return false;
    }

    const int n = s_sock.write(data, (size_t)len);
    if (n != len) {
        LOGW(TAG, "发送只写出 %d/%d 字节", n, len);
        return false;
    }
    return true;
}

// 发 MQTT 固定头
static bool mqtt_send(uint8_t type_flags, const uint8_t *payload, int len) {
    uint8_t hdr[5];
    int     n = 0;

    hdr[n++] = type_flags;

    // 剩余长度变长编码
    int remain = len;
    do {
        uint8_t byte = (uint8_t)(remain % 128);
        remain /= 128;
        if (remain > 0) {
            byte |= 0x80;
        }
        hdr[n++] = byte;
    } while (remain > 0 && n < 5);

    if (!sock_write(hdr, n)) {
        return false;
    }
    if (len > 0 && !sock_write(payload, len)) {
        return false;
    }
    return true;
}

// 拼长度前缀字符串
static int put_str(uint8_t *buf, int at, const char *s) {
    const int len = (s != NULL) ? (int)strlen(s) : 0;

    buf[at++] = (uint8_t)((len >> 8) & 0xFF);
    buf[at++] = (uint8_t)(len & 0xFF);
    if (len > 0) {
        memcpy(buf + at, s, (size_t)len);
        at += len;
    }
    return at;
}

// 发 CONNECT
static bool mqtt_connect(void) {
    uint8_t buf[NET_TX_BUF_LEN];
    int     at = 0;

    at = put_str(buf, at, "MQTT");
    // 协议级别 4 = 3.1.1
    buf[at++] = 4;
    // 清会话，不用遗嘱
    buf[at++] = 0x02;
    buf[at++] = (uint8_t)((NET_MQTT_KEEPALIVE >> 8) & 0xFF);
    buf[at++] = (uint8_t)(NET_MQTT_KEEPALIVE & 0xFF);

    at = put_str(buf, at, s_client_id);

    // 这里没加用户名密码
    if (at > NET_TX_BUF_LEN) {
        return false;
    }

    // CONNECT 的 type=1
    if (!mqtt_send(0x10, buf, at)) {
        return false;
    }

    // 等 CONNACK
    const uint32_t t0 = millis();
    while ((millis() - t0) < 5000) {
        if (s_sock.available() >= 4) {
            uint8_t ack[4];
            if (s_sock.read(ack, 4) == 4) {
                if (ack[0] == 0x20 && ack[3] == 0x00) {
                    return true;
                }
                LOGW(TAG, "CONNACK 被拒，返回码 %d", ack[3]);
                return false;
            }
        }
        delay(10);
    }

    LOGW(TAG, "等 CONNACK 超时");
    return false;
}

// 发 SUBSCRIBE
static bool mqtt_subscribe(const char *topic) {
    uint8_t buf[NET_TX_BUF_LEN];
    int     at = 0;

    const uint16_t pid = s_packet_id++;

    buf[at++] = (uint8_t)((pid >> 8) & 0xFF);
    buf[at++] = (uint8_t)(pid & 0xFF);

    at = put_str(buf, at, topic);
    // qos 1
    buf[at++] = 1;

    // SUBSCRIBE 的 type=8，标志位必须是 2
    if (!mqtt_send(0x82, buf, at)) {
        return false;
    }

    LOGI(TAG, "已订阅 %s", topic);
    return true;
}

// 发 PUBLISH
static app_err_t pub(const char *topic, const char *payload, int qos, bool retain) {
    if (topic == NULL || topic[0] == '\0' || payload == NULL) {
        return APP_ERR_INVALID_ARG;
    }
    if (!s_mqtt_ok) {
        s_need_republish = true;
        return APP_ERR_INVALID_STATE;
    }

    uint8_t buf[NET_TX_BUF_LEN];
    int     at = 0;

    const int plen = (int)strlen(payload);

    if (qos > 0) {
        const uint16_t pid = s_packet_id++;
        buf[at++] = (uint8_t)((pid >> 8) & 0xFF);
        buf[at++] = (uint8_t)(pid & 0xFF);
    }

    at = put_str(buf, at, topic);

    if ((at + plen) > NET_TX_BUF_LEN) {
        LOGW(TAG, "payload 太长（%d 字节），本条不发布", plen);
        return APP_ERR_NO_MEM;
    }

    memcpy(buf + at, payload, (size_t)plen);
    at += plen;

    // type=3，qos 与 retain 进标志位
    const uint8_t flags = (uint8_t)(0x30 | ((qos & 0x03) << 1) | (retain ? 0x01 : 0x00));

    if (!mqtt_send(flags, buf, at)) {
        LOGW(TAG, "发布失败：topic=%s（%d 字节）", topic, plen);
        s_mqtt_ok         = false;
        s_need_republish  = true;
        return APP_ERR_FAIL;
    }
    return APP_OK;
}

app_err_t net_link_publish_state(void) {
    char buf[NET_JSON_STATE_LEN];
    const int n = net_proto_state_json(buf, sizeof(buf));
    if (n <= 0) {
        return APP_ERR_FAIL;
    }
    // 蓝牙同时来一份
    // 没 WiFi 也照发
    ble_app_notify_state(buf, n);
    return pub(s_topic_state, buf, 1, true);
}

app_err_t net_link_publish_sensor(const sensor_snapshot_t *snap) {
    char buf[NET_JSON_SENSOR_LEN];
    const int n = net_proto_sensor_json(buf, sizeof(buf), snap);
    if (n <= 0) {
        return APP_ERR_FAIL;
    }
    ble_app_notify_sensor(buf, n);
    return pub(s_topic_sensor, buf, 0, false);
}

app_err_t net_link_publish_weather(const sensor_snapshot_t *snap) {
    char buf[NET_JSON_WEATHER_LEN];
    const int n = net_proto_weather_json(buf, sizeof(buf), snap);
    if (n <= 0) {
        return APP_ERR_FAIL;
    }
    ble_app_notify_weather(buf, n);
    return pub(s_topic_weather, buf, 1, true);
}

app_err_t net_link_publish_config(void) {
    char buf[NET_JSON_CONFIG_LEN];
    const int n = net_proto_config_json(buf, sizeof(buf));
    if (n <= 0) {
        return APP_ERR_FAIL;
    }

    // 内容没变就不重复发
    if (strcmp(buf, s_last_pub_config) == 0) {
        return APP_OK;
    }

    const app_err_t err = pub(s_topic_config, buf, 1, true);
    if (err == APP_OK) {
        snprintf(s_last_pub_config, sizeof(s_last_pub_config), "%s", buf);
    }
    ble_app_notify_config(buf, n);
    return err;
}

app_err_t net_link_publish_ack(const char *dev, const char *action, bool ok, const char *detail) {
    char buf[NET_JSON_ACK_LEN];
    const int n = net_proto_ack_json(buf, sizeof(buf), dev, action, ok, detail);
    if (n <= 0) {
        return APP_ERR_FAIL;
    }
    ble_app_notify_ack(buf, n);
    return pub(s_topic_ack, buf, 1, false);
}

app_err_t net_link_publish_event(const char *what, const char *info) {
    char buf[NET_JSON_ACK_LEN];
    const int n = net_proto_event_json(buf, sizeof(buf), what, info);
    if (n <= 0) {
        return APP_ERR_FAIL;
    }
    return pub(s_topic_event, buf, 1, false);
}

app_err_t net_link_publish_all(void) {
    sensor_snapshot_t snap;
    bsp_sensors_get(&snap);

    app_err_t first_err = net_link_publish_state();

    app_err_t e = net_link_publish_sensor(&snap);
    if (first_err == APP_OK) {
        first_err = e;
    }
    e = net_link_publish_weather(&snap);
    if (first_err == APP_OK) {
        first_err = e;
    }
    e = net_link_publish_config();
    if (first_err == APP_OK) {
        first_err = e;
    }

    if (first_err == APP_OK) {
        s_need_republish = false;
    }
    return first_err;
}

// 从 JSON 里抠出字段
// 找不到给 false
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') {
        p++;
    }
    if (*p != '\"') {
        return false;
    }
    p++;

    size_t i = 0;
    while (*p != '\0' && *p != '\"' && i < (out_len - 1)) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0);
}

static bool json_get_int(const char *json, const char *key, int *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        // 只认非负整数
        if (*p != '-') {
            return false;
        }
    }
    *out = atoi(p);
    return true;
}

// 下行指令统一处理
app_err_t net_link_handle_cmd_payload(const char *data, int len, char *detail, size_t detail_len) {
    if (data == NULL || len <= 0) {
        if (detail != NULL) {
            snprintf(detail, detail_len, "payload 为空");
        }
        (void)net_link_publish_ack(NULL, NULL, false, "payload 解析失败");
        return APP_ERR_INVALID_ARG;
    }

    char json[NET_RX_PAYLOAD_MAX + 1];
    const int copy = (len < NET_RX_PAYLOAD_MAX) ? len : NET_RX_PAYLOAD_MAX;
    memcpy(json, data, (size_t)copy);
    json[copy] = '\0';

    char dev[32]    = {0};
    char action[32] = {0};
    int  pct        = -1;

    if (!json_get_string(json, "dev", dev, sizeof(dev))) {
        if (detail != NULL) {
            snprintf(detail, detail_len, "缺少 dev 字段");
        }
        (void)net_link_publish_ack(NULL, NULL, false, "payload 解析失败");
        return APP_ERR_INVALID_ARG;
    }
    if (!json_get_string(json, "action", action, sizeof(action))) {
        if (detail != NULL) {
            snprintf(detail, detail_len, "缺少 action 字段");
        }
        (void)net_link_publish_ack(dev, NULL, false, "payload 解析失败");
        return APP_ERR_INVALID_ARG;
    }

    // 可选
    (void)json_get_int(json, "pct", &pct);

    LOGD(TAG, "下行 cmd: %s/%s pct=%d", dev, action, pct);

    char            ack_detail[HOME_CMD_DETAIL_LEN] = {0};
    const app_err_t err = home_command_execute(dev, action, pct, SRC_APP, ack_detail);

    if (detail != NULL) {
        snprintf(detail, detail_len, "%s", ack_detail);
    }

    (void)net_link_publish_ack(dev, action, err == APP_OK, ack_detail);

    // 指令可能改了状态
    (void)net_link_publish_state();

    if (err != APP_OK) {
        LOGW(TAG, "指令没走通: %s/%s → %s", dev, action, ack_detail);
    }
    return err;
}

// 处理一条完整报文
static void mqtt_handle_packet(const uint8_t *pkt, int total, int remain_len, int header_len) {
    const uint8_t type = (uint8_t)(pkt[0] >> 4);
    const uint8_t *body = pkt + header_len;

    switch (type) {
    case 3: {
        // PUBLISH：先长度前缀 topic
        if (remain_len < 2) {
            return;
        }
        const int tlen = (body[0] << 8) | body[1];
        if (2 + tlen > remain_len) {
            return;
        }

        char topic[NET_TOPIC_LEN];
        const int copy = (tlen < (int)sizeof(topic) - 1) ? tlen : (int)sizeof(topic) - 1;
        memcpy(topic, body + 2, (size_t)copy);
        topic[copy] = '\0';

        // qos>0 才有包标识
        const int qos   = (pkt[0] >> 1) & 0x03;
        int       off   = 2 + tlen;
        if (qos > 0) {
            off += 2;
        }
        if (off > remain_len) {
            return;
        }

        const int plen = remain_len - off;
        const char *payload = (const char *)(body + off);

        (void)total;

        // topic 前缀必须对上
        if (strncmp(topic, s_base, strlen(s_base)) != 0) {
            LOGD(TAG, "忽略别人的 topic: %s", topic);
            return;
        }

        if (strcmp(topic, s_topic_cmd) == 0) {
            char detail[HOME_CMD_DETAIL_LEN] = {0};
            (void)net_link_handle_cmd_payload(payload, plen, detail, sizeof(detail));
        } else if (strcmp(topic, s_topic_get) == 0) {
            (void)net_link_publish_all();
        } else if (strcmp(topic, s_topic_config) == 0) {
            // 面板重连时重发
            if (s_last_pub_config[0] != '\0') {
                (void)pub(s_topic_config, s_last_pub_config, 1, true);
            }
        }
        break;
    }

    case 9:
        // SUBACK，不用管
        break;

    case 13:
        // PINGRESP
        s_ping_out = false;
        break;

    default:
        LOGD(TAG, "收到报文类型 %u，忽略", (unsigned)type);
        break;
    }
}

// 把缓冲区里的完整包都取走
static void mqtt_drain(void) {
    int at = 0;

    while (at < s_rx_len) {
        // 头 1 字节 + 剩余长度
        if ((s_rx_len - at) < 2) {
            break;
        }

        const uint8_t *p    = s_rx + at;
        int            mult = 1;
        int            rlen = 0;
        int            used = 1;
        bool           done = false;

        for (int i = 0; i < NET_REMAIN_MAX; i++) {
            if ((at + used) >= s_rx_len) {
                break;
            }
            const uint8_t byte = p[used++];
            rlen += (byte & 0x7F) * mult;
            mult *= 128;
            if ((byte & 0x80) == 0) {
                done = true;
                break;
            }
        }

        if (!done) {
            break;
        }
        if ((at + used + rlen) > s_rx_len) {
            // 还没收全
            break;
        }

        mqtt_handle_packet(p, used + rlen, rlen, used);
        at += used + rlen;
    }

    // 挪走已消费的
    if (at > 0) {
        memmove(s_rx, s_rx + at, (size_t)(s_rx_len - at));
        s_rx_len -= at;
    }
}

// 断开并把状态清干净
static void mqtt_drop(void) {
    if (s_mqtt_ok) {
        LOGW(TAG, "MQTT 连接断开");
    }

    s_sock.stop();
    s_mqtt_ok        = false;
    s_ping_out       = false;
    s_rx_len         = 0;
    s_need_republish = true;
    s_mqtt_retry_at  = millis() + NET_MQTT_RETRY_MS;
}

// 起 MQTT
static void mqtt_try_connect(void) {
    if (millis() < s_mqtt_retry_at) {
        return;
    }
    s_mqtt_retry_at = millis() + NET_MQTT_RETRY_MS;

    LOGI(TAG, "连接 MQTT %s:%d ...", APP_MQTT_HOST, APP_MQTT_PORT);

    if (!s_sock.connect(APP_MQTT_HOST, APP_MQTT_PORT)) {
        LOGW(TAG, "MQTT 服务器连不上（%s:%d）—— 网页面板用不了，蓝牙与语音照常", APP_MQTT_HOST,
             APP_MQTT_PORT);
        return;
    }

    // 报文最大 768 字节
    s_sock.setTimeout(1);

    if (!mqtt_connect()) {
        LOGW(TAG, "MQTT 握手失败");
        mqtt_drop();
        return;
    }

    s_mqtt_ok        = true;
    s_need_subscribe = true;
    s_rx_len         = 0;
    s_last_ping_ms   = millis();

    LOGI(TAG, "MQTT 已连接，客户端 ID=%s", s_client_id);
}

// 重连后补订阅与补发
static void mqtt_after_connect(void) {
    if (!s_need_subscribe) {
        return;
    }
    s_need_subscribe = false;

    (void)mqtt_subscribe(s_topic_cmd_sub);
    (void)mqtt_subscribe(s_topic_config);
    (void)mqtt_subscribe(s_topic_get);

    (void)pub(s_topic_availability, "online", 1, true);

    LOGI(TAG, "topics: state=%s cmd=%s sensor=%s weather=%s", s_topic_state, s_topic_cmd_sub, s_topic_sensor,
         s_topic_weather);

    (void)net_link_publish_all();
}

// 收数据
static void mqtt_pump(void) {
    if (!s_mqtt_ok) {
        return;
    }
    if (!s_sock.connected()) {
        mqtt_drop();
        return;
    }

    // 一次别读太久
    const int avail = s_sock.available();
    if (avail > 0) {
        uint8_t tmp[128];
        const int want = (avail < (int)sizeof(tmp)) ? avail : (int)sizeof(tmp);
        const int got  = s_sock.read(tmp, (size_t)want);
        if (got > 0) {
            if (rx_push(tmp, got)) {
                mqtt_drain();
            }
        }
    }

    // 保活
    const uint32_t now = millis();
    if ((now - s_last_ping_ms) >= ((uint32_t)NET_MQTT_KEEPALIVE * 1000u / 2u)) {
        s_last_ping_ms = now;
        if (s_ping_out) {
            LOGW(TAG, "上一次 PINGREQ 没回应，重连");
            mqtt_drop();
            return;
        }
        // PINGREQ 的 type=12
        if (mqtt_send(0xC0, NULL, 0)) {
            s_ping_out = true;
        } else {
            mqtt_drop();
        }
    }
}

// 连得差不多了就补发
static void net_republish_if_needed(void) {
    if (!s_mqtt_ok || !s_need_republish) {
        return;
    }
    (void)net_link_publish_all();
}

// 起来才算成功
app_err_t net_link_init(void) {
    if (s_inited) {
        return APP_OK;
    }

    make_uid();
    wifi_load_saved();

    if (topics_build() != APP_OK) {
        // 没网也不影响别的
        LOGW(TAG, "topic 拼装失败：联网功能不可用，其余功能照常");
        s_inited = true;
        return APP_ERR_FAIL;
    }

    if (s_wifi_ssid[0] == '\0' && APP_WIFI_SSID[0] == '\0') {
        LOGW(TAG, "没配 WiFi：网页面板不可用。用手机 App 通过蓝牙下发 WiFi 即可（不用重新烧录）");
        s_inited = true;
        return APP_OK;
    }

    WiFi.mode(WIFI_STA);
    // 省电会拖慢响应
    WiFi.setSleep(false);

    const char *ssid = (s_wifi_ssid[0] != '\0') ? s_wifi_ssid : APP_WIFI_SSID;
    const char *pass = (s_wifi_ssid[0] != '\0') ? s_wifi_pass : APP_WIFI_PASS;

    LOGI(TAG, "连接 WiFi \"%s\" ...", ssid);
    WiFi.begin(ssid, pass);

    s_inited       = true;
    s_wifi_retry_at = millis() + NET_WIFI_RETRY_MS;

    LOGI(TAG, "base topic: %s（网页面板填这串）", s_base);
    return APP_OK;
}

// 主循环里轮询
void net_link_tick(void) {
    if (!s_inited) {
        return;
    }
    if (s_wifi_ssid[0] == '\0' && APP_WIFI_SSID[0] == '\0') {
        return;
    }

    // WiFi 自动重连
    if (WiFi.status() != WL_CONNECTED) {
        if (s_mqtt_ok) {
            mqtt_drop();
        }
        if (millis() >= s_wifi_retry_at) {
            s_wifi_retry_at = millis() + NET_WIFI_RETRY_MS;
            LOGD(TAG, "WiFi 还没连上，重试中 ...");
            WiFi.reconnect();
        }
        return;
    }

    // 记 IP 与信号
    snprintf(s_ip, sizeof(s_ip), "%s", WiFi.localIP().toString().c_str());

    if (!s_mqtt_ok) {
        mqtt_try_connect();
        if (!s_mqtt_ok) {
            return;
        }
    }

    mqtt_after_connect();
    mqtt_pump();
    net_republish_if_needed();
}

void net_link_stop(void) {
    if (s_mqtt_ok) {
        // DISCONNECT 的 type=14
        (void)mqtt_send(0xE0, NULL, 0);
    }
    mqtt_drop();
    WiFi.disconnect(true);
    LOGI(TAG, "联网已停止");
}

// 现在连到哪一步了
net_state_t net_link_get_state(void) {
    if (s_mqtt_ok) {
        return NET_STATE_MQTT_OK;
    }
    return net_link_wifi_ok() ? NET_STATE_WIFI_OK : NET_STATE_DOWN;
}

bool net_link_wifi_ok(void) {
    return WiFi.status() == WL_CONNECTED;
}

bool net_link_mqtt_ok(void) {
    return s_mqtt_ok;
}

// MAC 后 3 字节
const char *net_link_uid(void) {
    return s_uid;
}

const char *net_link_ip(void) {
    return s_ip;
}

int8_t net_link_rssi(void) {
    if (!net_link_wifi_ok()) {
        return 0;
    }
    return (int8_t)WiFi.RSSI();
}

// topic 前缀
const char *net_link_topic_base(void) {
    if (s_base[0] == '\0') {
        (void)topics_build();
    }
    return s_base;
}

// 串口调试用
void net_link_dump(void) {
    LOGI(TAG, "WiFi     : %s（\"%s\"）", net_link_wifi_ok() ? "已连接" : "未连接",
         (s_wifi_ssid[0] != '\0') ? s_wifi_ssid : APP_WIFI_SSID);
    LOGI(TAG, "IP       : %s   RSSI=%d dBm", s_ip, (int)net_link_rssi());
    LOGI(TAG, "MQTT     : %s（%s:%d）", s_mqtt_ok ? "已连接" : "未连接", APP_MQTT_HOST, APP_MQTT_PORT);
    LOGI(TAG, "client id: %s", s_client_id);
    LOGI(TAG, "base     : %s", s_base);
    LOGI(TAG, "--- 上行 topic ---");
    LOGI(TAG, "  availability : %s   (retain=1, qos=1)", s_topic_availability);
    LOGI(TAG, "  state        : %s   (retain=1, qos=1)", s_topic_state);
    LOGI(TAG, "  sensor       : %s   (retain=0, qos=0, 周期 %d ms)", s_topic_sensor,
         APP_MQTT_PUBLISH_SENSOR_MS);
    LOGI(TAG, "  weather      : %s   (retain=1, qos=1)", s_topic_weather);
    LOGI(TAG, "  ack          : %s   (retain=0, qos=1)", s_topic_ack);
    LOGI(TAG, "  event        : %s   (retain=0, qos=1)", s_topic_event);
    LOGI(TAG, "  config       : %s   (retain=1, qos=1)", s_topic_config);
    LOGI(TAG, "--- 下行 topic（已订阅 qos=1）---");
    LOGI(TAG, "  cmd          : %s", s_topic_cmd_sub);
    LOGI(TAG, "  config       : %s", s_topic_config);
    LOGI(TAG, "  get          : %s", s_topic_get);
}

// 换 WiFi 并重连
// 空 ssid 回默认
app_err_t net_link_set_wifi(const char *ssid, const char *pass) {
    if (ssid == NULL || ssid[0] == '\0') {
        LOGW(TAG, "WiFi 名不能为空");
        return APP_ERR_INVALID_ARG;
    }

    snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", ssid);
    snprintf(s_wifi_pass, sizeof(s_wifi_pass), "%s", (pass != NULL) ? pass : "");

    Preferences prefs;
    if (prefs.begin("wifi_cfg", false)) {
        prefs.putString("ssid", s_wifi_ssid);
        prefs.putString("pass", s_wifi_pass);
        prefs.end();
        LOGI(TAG, "已保存 WiFi \"%s\" 到 NVS（换路由器不用重新烧录）", s_wifi_ssid);
    } else {
        LOGW(TAG, "WiFi 存不下，本次仍会用新账号重连");
    }

    s_need_republish = true;

    LOGI(TAG, "重连 WiFi \"%s\" ...", s_wifi_ssid);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(s_wifi_ssid, s_wifi_pass);

    s_wifi_retry_at = millis() + NET_WIFI_RETRY_MS;
    return APP_OK;
}

// 手机 App 下发的那份
const char *net_link_get_wifi_ssid(void) {
    return s_wifi_ssid;
}
