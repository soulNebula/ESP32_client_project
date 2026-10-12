// c
// WiFi 与 MQTT

#include "net_link.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"

// 上行 JSON
#include "mqtt_protocol.h"
// 业务出口
#include "app_cmd.h"
#include "sensor.h"
// 蓝牙并行通道
#include "ble_app.h"

static const char *TAG = "NET";

// base 缓冲
#define NET_BASE_LEN 64
// 最长 topic
#define NET_TOPIC_LEN 96
#define NET_CLIENT_ID_LEN 48
// 下行字段
#define NET_FIELD_MAX 32
// event 缓冲
#define NET_JSON_EVENT_LEN 192

// WiFi 账号存这
#define NET_NVS_NS "wifi_cfg"
// 名最长 32 字节
#define NET_WIFI_SSID_LEN 33
// 密码最长 64 字节
#define NET_WIFI_PASS_LEN 65

// 首次退避 1s
#define NET_BACKOFF_MIN_MS 1000u
// 退避封顶 30s
#define NET_BACKOFF_MAX_MS 30000u

// 找不到 AP
#define NET_DISC_REASON_NO_AP 201
// 密码错
#define NET_DISC_REASON_PW_ERR 15

// 初始化过没有
static bool s_inited = false;
// stop 之后置位
static bool s_stopped = false;
// 拿到 IP 才算连上
static bool s_wifi_up = false;
// 事件里维护
static volatile bool        s_mqtt_up = false;
static volatile net_state_t s_state   = NET_STATE_DOWN;
// start 调过没
static bool s_mqtt_started = false;
// 欠一条待补发
static bool s_need_republish = true;

static esp_netif_t             *s_netif       = NULL;
static esp_mqtt_client_handle_t s_client      = NULL;
static esp_timer_handle_t       s_retry_timer = NULL;

// 连续重连次数
static int s_retry = 0;
// 当前退避时长
static uint32_t s_backoff_ms = NET_BACKOFF_MIN_MS;

// 6 位小写 hex
static char s_uid[NET_UID_LEN] = {0};
// 前缀 + UID
static char s_base[NET_BASE_LEN]           = {0};
static char s_client_id[NET_CLIENT_ID_LEN] = {0};
// 点分十进制
static char s_ip[16] = "0.0.0.0";

// 当前 WiFi 账号
static char s_ssid[NET_WIFI_SSID_LEN] = {0};
static char s_pass[NET_WIFI_PASS_LEN] = {0};

// 现在到底用哪家
static const char *active_ssid(void) {
    return (s_ssid[0] != '\0') ? s_ssid : CONFIG_APP_WIFI_SSID;
}

// 初始化时拼一次
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

// config 回显
static char s_last_pub_config[NET_JSON_CONFIG_LEN] = {0};

// 结尾补 '\0'
static void copy_into_u8(uint8_t *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy((char *)dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// 超长直接失败
static bool field_copy(char *dst, size_t len, const char *src) {
    if (dst == NULL || len == 0) {
        return false;
    }
    const char  *s = (src != NULL) ? src : "";
    const size_t n = strlen(s);
    if (n >= len) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, s, n + 1);
    return true;
}

// 不用 atoi
static bool str_to_int(const char *s, int *out) {
    if (s == NULL || out == NULL || s[0] == '\0') {
        return false;
    }

    int i      = 0;
    int sign   = 1;
    int v      = 0;
    int digits = 0;

    if (s[i] == '-') {
        sign = -1;
        i++;
    } else if (s[i] == '+') {
        i++;
    }

    for (; s[i] != '\0'; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0');
            digits++;
            // 离谱值不要
            if (v > 100000) {
                return false;
            }
        } else if (s[i] == '.' && digits > 0) {
            // 小数部分忽略
            break;
        } else {
            return false;
        }
    }

    if (digits == 0) {
        return false;
    }
    *out = sign * v;
    return true;
}

// 允许 /子主题
static bool topic_hit(const char *topic, int topic_len, const char *full) {
    if (topic == NULL || full == NULL || topic_len <= 0) {
        return false;
    }
    const size_t fl = strlen(full);
    if (fl == 0 || (size_t)topic_len < fl) {
        return false;
    }
    if (memcmp(topic, full, fl) != 0) {
        return false;
    }
    return ((size_t)topic_len == fl) || (topic[fl] == '/');
}

// 调试台打印
static const char *state_name(net_state_t s) {
    switch (s) {
    case NET_STATE_MQTT_OK:
        return "MQTT_OK（手机端可用）";
    case NET_STATE_WIFI_OK:
        return "WIFI_OK（WiFi 已连，MQTT 未连）";
    default:
        return "DOWN（离线，本地功能照常）";
    }
}

const char *net_link_uid(void) {
    if (s_uid[0] != '\0') {
        return s_uid;
    }

    if (CONFIG_APP_MQTT_UID_OVERRIDE[0] != '\0') {
        const int n = snprintf(s_uid, sizeof(s_uid), "%s", CONFIG_APP_MQTT_UID_OVERRIDE);
        if (n > 0 && n < (int)sizeof(s_uid)) {
            return s_uid;
        }
        ESP_LOGW(TAG, "CONFIG_APP_MQTT_UID_OVERRIDE 太长（%d 字符，最多 %d），改用 MAC 后 3 字节", n,
                 (int)sizeof(s_uid) - 1);
        s_uid[0] = '\0';
    }

    uint8_t   mac[6] = {0};
    esp_err_t err    = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        // 读 eFuse
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读 MAC 失败(%s)，退化为固定 ID（多台设备会撞车）", esp_err_to_name(err));
        snprintf(s_uid, sizeof(s_uid), "000000");
        return s_uid;
    }

    // 只取后 3 字节
    snprintf(s_uid, sizeof(s_uid), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return s_uid;
}

// 失败必须清空
static void topics_reset(void) {
    s_base[0]               = '\0';
    s_client_id[0]          = '\0';
    s_topic_state[0]        = '\0';
    s_topic_sensor[0]       = '\0';
    s_topic_weather[0]      = '\0';
    s_topic_config[0]       = '\0';
    s_topic_ack[0]          = '\0';
    s_topic_event[0]        = '\0';
    s_topic_cmd[0]          = '\0';
    s_topic_cmd_sub[0]      = '\0';
    s_topic_get[0]          = '\0';
    s_topic_availability[0] = '\0';
}

static esp_err_t topics_build(void) {
    topics_reset();

    const char *uid = net_link_uid();
    if (uid == NULL || uid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    int n = snprintf(s_base, sizeof(s_base), "%s/%s", CONFIG_APP_MQTT_TOPIC_PREFIX, uid);
    if (n < 0 || n >= (int)sizeof(s_base)) {
        ESP_LOGE(TAG, "base topic 太长（前缀 \"%s\" + UID \"%s\"，上限 %d 字节）",
                 CONFIG_APP_MQTT_TOPIC_PREFIX, uid, (int)sizeof(s_base) - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    n = snprintf(s_client_id, sizeof(s_client_id), "esp32sh-%s", uid);
    if (n < 0 || n >= (int)sizeof(s_client_id)) {
        ESP_LOGE(TAG, "client_id 太长（上限 %d 字节）", (int)sizeof(s_client_id) - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    struct topic_slot {
        char       *buf;
        size_t      len;
        const char *suffix;
    } table[] = {
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

    // 一张表拼 10 个
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        n = snprintf(table[i].buf, table[i].len, "%s%s", s_base, table[i].suffix);
        if (n < 0 || n >= (int)table[i].len) {
            ESP_LOGE(TAG, "topic \"%s%s\" 放不下（上限 %d 字节）", s_base, table[i].suffix,
                     (int)table[i].len - 1);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

// 没连上就静默
static esp_err_t pub(const char *topic, const char *payload, int qos, bool retain) {
    if (topic == NULL || topic[0] == '\0' || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mqtt_up || s_client == NULL) {
        s_need_republish = true;
        return ESP_ERR_INVALID_STATE;
    }

    // len=0 自动算长
    const int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        // 失败就标记补发
        ESP_LOGW(TAG, "发布失败(%d)：topic=%s（%d 字节）", msg_id, topic, (int)strlen(payload));
        s_need_republish = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t net_link_publish_state(void) {
    char      buf[NET_JSON_STATE_LEN];
    const int n = net_proto_state_json(buf, sizeof(buf));
    // 宁可不发坏包
    if (n <= 0) {
        ESP_LOGW(TAG, "state JSON 生成失败，跳过本次上报");
        return ESP_FAIL;
    }
    // 蓝牙同时来一份
    // 没 WiFi 也照发
    ble_app_notify_state(buf, n);
    return pub(s_topic_state, buf, 1, true);
}

// 实时不 retain
esp_err_t net_link_publish_sensor(const sensor_snapshot_t *snap) {
    if (snap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char      buf[NET_JSON_SENSOR_LEN];
    const int n = net_proto_sensor_json(buf, sizeof(buf), snap);
    if (n <= 0) {
        ESP_LOGW(TAG, "sensor JSON 生成失败，跳过本次上报");
        return ESP_FAIL;
    }
    ble_app_notify_sensor(buf, n);
    return pub(s_topic_sensor, buf, 0, false);
}

esp_err_t net_link_publish_weather(const sensor_snapshot_t *snap) {
    if (snap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char      buf[NET_JSON_WEATHER_LEN];
    const int n = net_proto_weather_json(buf, sizeof(buf), snap);
    if (n <= 0) {
        ESP_LOGW(TAG, "weather JSON 生成失败，跳过本次上报");
        return ESP_FAIL;
    }
    ble_app_notify_weather(buf, n);
    return pub(s_topic_weather, buf, 1, true);
}

esp_err_t net_link_publish_config(void) {
    char      buf[NET_JSON_CONFIG_LEN];
    const int n = net_proto_config_json(buf, sizeof(buf));
    if (n <= 0) {
        return ESP_FAIL;
    }

    // 存一份比对回显
    snprintf(s_last_pub_config, sizeof(s_last_pub_config), "%s", buf);

    ble_app_notify_config(buf, n);
    return pub(s_topic_config, buf, 1, true);
}

esp_err_t net_link_publish_ack(const char *dev, const char *action, bool ok, const char *detail) {
    char      buf[NET_JSON_ACK_LEN];
    const int n = net_proto_ack_json(buf, sizeof(buf), dev, action, ok, detail);
    if (n <= 0) {
        return ESP_FAIL;
    }
    ble_app_notify_ack(buf, n);
    // 回执不 retain
    return pub(s_topic_ack, buf, 1, false);
}

esp_err_t net_link_publish_event(const char *what, const char *info) {
    char      buf[NET_JSON_EVENT_LEN];
    const int n = net_proto_event_json(buf, sizeof(buf), what, info);
    if (n <= 0) {
        return ESP_FAIL;
    }
    return pub(s_topic_event, buf, 1, false);
}

// 重连后要全量重发
esp_err_t net_link_publish_all(void) {
    sensor_snapshot_t snap;
    bsp_sensors_get(&snap);

    esp_err_t first_err = net_link_publish_state();
    esp_err_t e;

    e = net_link_publish_sensor(&snap);
    if (first_err == ESP_OK) {
        first_err = e;
    }
    e = net_link_publish_weather(&snap);
    if (first_err == ESP_OK) {
        first_err = e;
    }
    e = net_link_publish_config();
    if (first_err == ESP_OK) {
        first_err = e;
    }

    if (first_err == ESP_OK) {
        s_need_republish = false;
    }
    return first_err;
}

// 原始三要素
typedef struct {
    char dev[NET_FIELD_MAX];
    char action[NET_FIELD_MAX];
    bool has_value;
    bool value_is_num;
    // 布尔也算数值
    double value_num;
    char value_str[NET_FIELD_MAX];
} raw_cmd_t;

// 必须带长度解析
static bool raw_from_json(const char *buf, int len, raw_cmd_t *r) {
    cJSON *root = cJSON_ParseWithLength(buf, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "下行 JSON 解析失败：%.*s", len, buf);
        return false;
    }
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "下行 JSON 不是对象，忽略");
        cJSON_Delete(root);
        return false;
    }

    bool         ok   = true;
    const cJSON *jdev = cJSON_GetObjectItemCaseSensitive(root, "dev");
    const cJSON *jact = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *jval = cJSON_GetObjectItemCaseSensitive(root, "value");

    // 拷完再 Delete
    if (jdev != NULL && cJSON_IsString(jdev) && jdev->valuestring != NULL) {
        ok = field_copy(r->dev, sizeof(r->dev), jdev->valuestring);
    }
    if (ok && jact != NULL && cJSON_IsString(jact) && jact->valuestring != NULL) {
        ok = field_copy(r->action, sizeof(r->action), jact->valuestring);
    }

    if (ok && jval != NULL) {
        r->has_value = true;
        if (cJSON_IsNumber(jval)) {
            r->value_is_num = true;
            r->value_num    = jval->valuedouble;
        } else if (cJSON_IsBool(jval)) {
            r->value_is_num = true;
            r->value_num    = cJSON_IsTrue(jval) ? 1.0 : 0.0;
        } else if (cJSON_IsString(jval) && jval->valuestring != NULL) {
            if (!field_copy(r->value_str, sizeof(r->value_str), jval->valuestring)) {
                ok = false;
            } else {
                int iv = 0;
                if (str_to_int(r->value_str, &iv)) {
                    r->value_is_num = true;
                    r->value_num    = (double)iv;
                }
            }
        } else {
            ok = false;
        }
    }

    cJSON_Delete(root);

    if (!ok) {
        ESP_LOGW(TAG, "下行 JSON 字段过长或类型不支持");
        r->dev[0]    = '\0';
        r->action[0] = '\0';
        return false;
    }
    return (r->dev[0] != '\0') || (r->action[0] != '\0');
}

// 不用 strtok
static bool raw_from_kv(const char *buf, int len, raw_cmd_t *r) {
    if (len <= 0 || len > NET_RX_PAYLOAD_MAX) {
        return false;
    }

    char tmp[NET_RX_PAYLOAD_MAX + 1];
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = '\0';

    bool  got = false;
    char *p   = tmp;

    while (*p != '\0') {
        while (*p == ',' || *p == ';' || *p == '&' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        char *tok = p;
        while (*p != '\0' && *p != ',' && *p != ';' && *p != '&' && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }

        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            continue;
        }
        *eq           = '\0';
        const char *k = tok;
        const char *v = eq + 1;

        if (strcmp(k, "dev") == 0) {
            if (!field_copy(r->dev, sizeof(r->dev), v)) {
                return false;
            }
            got = true;
        } else if (strcmp(k, "action") == 0 || strcmp(k, "act") == 0) {
            if (!field_copy(r->action, sizeof(r->action), v)) {
                return false;
            }
            got = true;
        } else if (strcmp(k, "value") == 0 || strcmp(k, "val") == 0) {
            if (!field_copy(r->value_str, sizeof(r->value_str), v)) {
                return false;
            }
            r->has_value = true;
            int iv       = 0;
            if (str_to_int(r->value_str, &iv)) {
                r->value_is_num = true;
                r->value_num    = (double)iv;
            }
            got = true;
        }
    }
    return got;
}

// 短名当语音指令
static bool raw_from_short(const char *buf, int len, raw_cmd_t *r) {
    // 去尾部空白
    int end = len;
    while (end > 0 &&
           (buf[end - 1] == ' ' || buf[end - 1] == '\t' || buf[end - 1] == '\r' || buf[end - 1] == '\n')) {
        end--;
    }
    if (end <= 0) {
        return false;
    }

    char token[NET_FIELD_MAX];
    int  o = 0;
    for (int i = 0; i < end; i++) {
        char c = buf[i];
        if (c == ' ' || c == '\t') {
            c = '_';
        }
        if (o >= (int)sizeof(token) - 1) {
            // 超长就拒绝
            ESP_LOGW(TAG, "短指令名过长（上限 %d 字节），拒绝", (int)sizeof(token) - 1);
            return false;
        }
        token[o++] = c;
    }
    token[o] = '\0';

    if (!field_copy(r->dev, sizeof(r->dev), "voice")) {
        return false;
    }
    if (!field_copy(r->action, sizeof(r->action), token)) {
        return false;
    }
    return true;
}

// value 转动作名
static const char *mode_action_of(const raw_cmd_t *r) {
    if (r->value_str[0] != '\0') {
        static const char *const k_ok[] = {
            "auto", "manual", "on", "off", "true", "false", "1", "0",
        };
        for (size_t i = 0; i < sizeof(k_ok) / sizeof(k_ok[0]); i++) {
            if (strcmp(r->value_str, k_ok[i]) == 0) {
                return r->value_str;
            }
        }
        return NULL;
    }
    if (r->has_value && r->value_is_num) {
        return (r->value_num != 0.0) ? "auto" : "manual";
    }
    return NULL;
}

// off 进静音锁
static const char *buzzer_action_of(const raw_cmd_t *r) {
    if (r->value_str[0] != '\0') {
        if (strcmp(r->value_str, "off") == 0 || strcmp(r->value_str, "mute") == 0 ||
            strcmp(r->value_str, "false") == 0 || strcmp(r->value_str, "0") == 0) {
            return "off";
        }
        if (strcmp(r->value_str, "on") == 0 || strcmp(r->value_str, "unmute") == 0 ||
            strcmp(r->value_str, "true") == 0 || strcmp(r->value_str, "1") == 0) {
            return "on";
        }
        return NULL;
    }
    if (r->has_value && r->value_is_num) {
        return (r->value_num != 0.0) ? "on" : "off";
    }
    return NULL;
}

// 归一到路由三要素
static bool raw_to_command(const raw_cmd_t *r, char *dev, size_t dev_len, char *action, size_t act_len,
                           int *pct) {
    *pct = -1;

    // 四舍五入成整数
    if (r->has_value && r->value_is_num) {
        const double v = r->value_num;
        *pct           = (int)(v + ((v >= 0.0) ? 0.5 : -0.5));
    }

    // 没有 dev
    if (r->dev[0] == '\0') {
        if (strcmp(r->action, "mode") == 0) {
            const char *a = mode_action_of(r);
            if (a == NULL) {
                return false;
            }
            if (!field_copy(dev, dev_len, "mode") || !field_copy(action, act_len, a)) {
                return false;
            }
            *pct = -1;
            return true;
        }
        if (strcmp(r->action, "buzzer") == 0) {
            const char *a = buzzer_action_of(r);
            if (a == NULL) {
                return false;
            }
            if (!field_copy(dev, dev_len, "buzzer") || !field_copy(action, act_len, a)) {
                return false;
            }
            *pct = -1;
            return true;
        }
        if (strcmp(r->action, "config") == 0 || strcmp(r->action, "status") == 0 ||
            strcmp(r->action, "get") == 0) {
            if (!field_copy(dev, dev_len, r->action)) {
                return false;
            }
            action[0] = '\0';
            return true;
        }
        return false;
    }

    if (strcmp(r->dev, "config") == 0 && strcmp(r->action, "mode") == 0) {
        const char *a = mode_action_of(r);
        if (a == NULL) {
            return false;
        }
        if (!field_copy(dev, dev_len, "mode") || !field_copy(action, act_len, a)) {
            return false;
        }
        *pct = -1;
        return true;
    }

    if (!field_copy(dev, dev_len, r->dev) || !field_copy(action, act_len, r->action)) {
        return false;
    }
    return true;
}

// 三种写法统一入口
static bool parse_cmd_payload(const char *buf, int len, char *dev, size_t dev_len, char *action,
                              size_t act_len, int *pct) {
    if (buf == NULL || len <= 0 || dev == NULL || action == NULL || pct == NULL || dev_len == 0 ||
        act_len == 0) {
        return false;
    }
    dev[0]    = '\0';
    action[0] = '\0';
    *pct      = -1;

    // 跳前导空白
    int i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        i++;
    }
    if (i >= len) {
        return false;
    }

    raw_cmd_t r;
    memset(&r, 0, sizeof(r));

    bool parsed;
    if (buf[i] == '{') {
        parsed = raw_from_json(buf + i, len - i, &r);
    } else {
        bool has_eq = false;
        for (int k = i; k < len; k++) {
            if (buf[k] == '=') {
                has_eq = true;
                break;
            }
        }
        parsed = has_eq ? raw_from_kv(buf + i, len - i, &r) : raw_from_short(buf + i, len - i, &r);
    }
    if (!parsed) {
        return false;
    }

    return raw_to_command(&r, dev, dev_len, action, act_len, pct);
}

// MQTT 和蓝牙共用
// detail 装原因
esp_err_t net_link_handle_cmd_payload(const char *data, int len, char *detail, size_t detail_len) {
    char  dev[NET_FIELD_MAX]            = {0};
    char  action[NET_FIELD_MAX]         = {0};
    char  fallback[HOME_CMD_DETAIL_LEN] = {0};
    char *out                           = (detail != NULL && detail_len > 0) ? detail : fallback;
    int   pct                           = -1;

    out[0] = '\0';

    // 太长不收
    if (data == NULL || len <= 0 || len > NET_RX_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "指令长度不合法（%d 字节，上限 %d），丢弃", len, NET_RX_PAYLOAD_MAX);
        (void)net_link_publish_ack(NULL, NULL, false, "payload 过长或分片");
        return ESP_ERR_INVALID_ARG;
    }

    if (!parse_cmd_payload(data, len, dev, sizeof(dev), action, sizeof(action), &pct)) {
        ESP_LOGW(TAG, "payload 解析失败（payload=%.*s）", len, data);
        (void)net_link_publish_ack(dev, action, false, "payload 解析失败");
        return ESP_ERR_INVALID_ARG;
    }

    // 业务出口只有一个
    // 算 SRC_APP
    const esp_err_t err = home_command_execute(dev, action, pct, SRC_APP, out);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "执行 %s/%s pct=%d -> OK%s%s", dev, action, pct, (out[0] != '\0') ? "：" : "", out);
    } else {
        ESP_LOGW(TAG, "执行 %s/%s pct=%d -> %s（%s）", dev, action, pct, esp_err_to_name(err),
                 (out[0] != '\0') ? out : home_command_err_text(err));
    }

    // ack 带失败原因
    const char *ack_detail;
    if (err == ESP_OK) {
        ack_detail = (out[0] != '\0' && strcmp(out, "ok") != 0) ? out : "";
    } else {
        ack_detail = (out[0] != '\0') ? out : home_command_err_text(err);
    }
    (void)net_link_publish_ack(dev, action, err == ESP_OK, ack_detail);

    // 完事后补 state
    (void)net_link_publish_state();

    return err;
}

// 回调里别阻塞
static void handle_downlink(const char *topic, int topic_len, const char *data, int data_len, int total_len) {
    // 收到 /get
    if (topic_hit(topic, topic_len, s_topic_get)) {
        ESP_LOGI(TAG, "收到 /get 查询，回发全量数据");
        (void)net_link_publish_all();
        return;
    }

    // 分片超长都丢
    if (data_len <= 0 || data_len > NET_RX_PAYLOAD_MAX || total_len != data_len) {
        ESP_LOGW(TAG, "下行 payload 长度不合法（单片 %d / 整条 %d 字节，上限 %d），丢弃", data_len, total_len,
                 NET_RX_PAYLOAD_MAX);
        (void)net_link_publish_ack(NULL, NULL, false, "payload 过长或分片");
        return;
    }

    // 防自己回显
    if (topic_hit(topic, topic_len, s_topic_config) && s_last_pub_config[0] != '\0' &&
        data_len == (int)strlen(s_last_pub_config) &&
        memcmp(data, s_last_pub_config, (size_t)data_len) == 0) {
        ESP_LOGD(TAG, "忽略 /config 回显（broker 把自己发的配置发回来了）");
        return;
    }

    ESP_LOGD(TAG, "下行 cmd（topic=%.*s, %d 字节）", topic_len, topic, data_len);

    // 解析执行回执都走这
    char            detail[HOME_CMD_DETAIL_LEN] = {0};
    const esp_err_t err                         = net_link_handle_cmd_payload(data, data_len, detail,
                                                                             sizeof(detail));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "指令没走通（topic=%.*s）：%s", topic_len, topic,
                 (detail[0] != '\0') ? detail : home_command_err_text(err));
    }
}

static const char *conn_refused_text(esp_mqtt_connect_return_code_t code) {
    switch (code) {
    case MQTT_CONNECTION_ACCEPTED:
        return "已接受（不应出现在错误里）";
    case MQTT_CONNECTION_REFUSE_PROTOCOL:
        return "协议版本不被 broker 支持";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:
        return "client_id 被拒绝（可能有同名设备已在线）";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
        return "broker 暂时不可用";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
        return "用户名错误（检查 CONFIG_APP_MQTT_USERNAME）";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
        return "用户名或密码错误（检查用户名/密码配置）";
    default:
        return "未知原因";
    }
}

// 错误码翻成人话
static void mqtt_log_error(const esp_mqtt_event_handle_t event) {
    const esp_mqtt_error_codes_t *e = (event != NULL) ? event->error_handle : NULL;
    if (e == NULL) {
        ESP_LOGE(TAG, "MQTT 错误（没有详细信息）");
        return;
    }

    switch (e->error_type) {
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        ESP_LOGE(TAG,
                 "MQTT 传输错误：连不上/断开 broker（检查 broker 地址 %s / WiFi 信号 / 账号密码）；"
                 "esp_err=%s, sock_errno=%d",
                 CONFIG_APP_MQTT_BROKER_URI, esp_err_to_name(e->esp_tls_last_esp_err),
                 e->esp_transport_sock_errno);
        break;
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        ESP_LOGE(TAG, "broker 拒绝连接：%s", conn_refused_text(e->connect_return_code));
        break;
    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
        ESP_LOGE(TAG, "订阅失败（topic 格式或权限问题）");
        break;
    default:
        ESP_LOGE(TAG, "MQTT 错误：type=%d", (int)e->error_type);
        break;
    }
}

// MQTT 事件回调
// 回调里别阻塞
static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)args;
    (void)base;

    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        s_state   = NET_STATE_MQTT_OK;
        ESP_LOGI(TAG, "已连上 broker");

        // 别重复订 cmd
        (void)esp_mqtt_client_subscribe(s_client, s_topic_cmd_sub, 1);
        (void)esp_mqtt_client_subscribe(s_client, s_topic_config, 1);
        (void)esp_mqtt_client_subscribe(s_client, s_topic_get, 1);

        // 上线通告
        (void)pub(s_topic_availability, "online", 1, true);

        // 重连后要重发
        (void)net_link_publish_all();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up        = false;
        s_state          = s_wifi_up ? NET_STATE_WIFI_OK : NET_STATE_DOWN;
        s_need_republish = true;
        // 等自动重连
        ESP_LOGW(TAG, "与 broker 断开，等待 esp-mqtt 自动重连（不要手动重建客户端）");
        break;

    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGD(TAG, "正在连接 broker：%s", CONFIG_APP_MQTT_BROKER_URI);
        break;

    case MQTT_EVENT_DATA:
        // 不是 C 字符串
        if (event->topic == NULL || event->topic_len <= 0 || event->data == NULL) {
            break;
        }
        handle_downlink(event->topic, event->topic_len, event->data, event->data_len, event->total_data_len);
        break;

    case MQTT_EVENT_ERROR:
        mqtt_log_error(event);
        s_mqtt_up = false;
        s_state   = s_wifi_up ? NET_STATE_WIFI_OK : NET_STATE_DOWN;
        break;

    default:
        break;
    }
}

// 到点才去连
static void retry_timer_cb(void *arg) {
    (void)arg;
    if (s_stopped) {
        // stop 后不重连
        return;
    }
    ESP_LOGI(TAG, "重连 WiFi（第 %d 次）...", s_retry);
    (void)esp_wifi_connect();
}

// 用定时器退避
static void schedule_reconnect(void) {
    if (s_retry_timer == NULL) {
        // 没定时器就直连
        (void)esp_wifi_connect();
        return;
    }

    // stop 报错可忽略
    (void)esp_timer_stop(s_retry_timer);

    if (esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000ULL) != ESP_OK) {
        (void)esp_wifi_connect();
        return;
    }

    s_backoff_ms = (s_backoff_ms >= NET_BACKOFF_MAX_MS) ? NET_BACKOFF_MAX_MS : (s_backoff_ms * 2u);
}

// WiFi 事件回调
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 已启动，开始连接 \"%s\" ...", active_ssid());
            (void)esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disc   = (const wifi_event_sta_disconnected_t *)event_data;
            const unsigned                       reason = (disc != NULL) ? (unsigned)disc->reason : 0u;

            // IP 已失效
            s_wifi_up = false;
            s_mqtt_up        = false;
            s_state          = NET_STATE_DOWN;
            s_need_republish = true;
            snprintf(s_ip, sizeof(s_ip), "0.0.0.0");

            if (s_stopped) {
                ESP_LOGD(TAG, "已停止联网，忽略本次断开事件");
                break;
            }

            s_retry++;

            // 0 = 永不放弃
            if (CONFIG_APP_WIFI_MAX_RETRY > 0 && s_retry > CONFIG_APP_WIFI_MAX_RETRY) {
                ESP_LOGE(TAG, "连续 %d 次连不上 \"%s\"（reason=%u），按配置重启设备", s_retry - 1,
                         active_ssid(), reason);
                esp_restart();
                break;
            }

            if (reason == NET_DISC_REASON_NO_AP) {
                ESP_LOGW(TAG, "断开(reason=%u 找不到该 WiFi)，请确认 SSID \"%s\" 是 2.4GHz 且名字没写错",
                         reason, active_ssid());
            } else if (reason == NET_DISC_REASON_PW_ERR) {
                ESP_LOGW(TAG, "断开(reason=%u 常见于密码错误/加密方式不匹配)，检查手机下发的 WiFi 密码",
                         reason);
            } else {
                ESP_LOGW(TAG, "断开(reason=%u)", reason);
            }

            if (CONFIG_APP_WIFI_MAX_RETRY == 0) {
                ESP_LOGW(TAG, "第 %d 次重连将在 %u ms 后（永不放弃）", s_retry, (unsigned)s_backoff_ms);
            } else {
                ESP_LOGW(TAG, "第 %d/%d 次重连将在 %u ms 后", s_retry, CONFIG_APP_WIFI_MAX_RETRY,
                         (unsigned)s_backoff_ms);
            }

            schedule_reconnect();
            break;
        }

        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got = (const ip_event_got_ip_t *)event_data;
        if (got != NULL) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&got->ip_info.ip));
        }

        s_wifi_up = true;
        s_retry   = 0;
        s_backoff_ms = NET_BACKOFF_MIN_MS;
        s_state      = NET_STATE_WIFI_OK;
        ESP_LOGI(TAG, "WiFi 已连接，IP=%s", s_ip);

        // 拿到 IP 再启
        if (s_client != NULL && !s_stopped) {
            if (!s_mqtt_started) {
                const esp_err_t err = esp_mqtt_client_start(s_client);
                if (err == ESP_OK) {
                    s_mqtt_started = true;
                    ESP_LOGI(TAG, "MQTT 客户端已启动，连接 %s ...", CONFIG_APP_MQTT_BROKER_URI);
                } else {
                    ESP_LOGE(TAG, "MQTT 客户端启动失败：%s", esp_err_to_name(err));
                }
            } else {
                (void)esp_mqtt_client_reconnect(s_client);
            }
        }
    }
}

// 账号先读 NVS
static void wifi_cred_load(void) {
    nvs_handle_t h;
    size_t       len = sizeof(s_ssid);

    s_ssid[0] = '\0';
    s_pass[0] = '\0';

    if (nvs_open(NET_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", s_ssid, &len) != ESP_OK) {
            s_ssid[0] = '\0';
        }
        len = sizeof(s_pass);
        if (nvs_get_str(h, "pass", s_pass, &len) != ESP_OK) {
            s_pass[0] = '\0';
        }
        nvs_close(h);
    }

    if (s_ssid[0] != '\0') {
        ESP_LOGI(TAG, "WiFi 用 NVS 里的 \"%s\"", s_ssid);
        return;
    }

    // 读不到就用编译时
    snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_APP_WIFI_SSID);
    snprintf(s_pass, sizeof(s_pass), "%s", CONFIG_APP_WIFI_PASSWORD);
    ESP_LOGI(TAG, "WiFi 用编译时的 \"%s\"", s_ssid);
}

// 账号写进 NVS
static esp_err_t wifi_cred_save(const char *ssid, const char *pass) {
    nvs_handle_t h;

    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "开 NVS 命名空间失败：%s", esp_err_to_name(err));
        return err;
    }

    // 空名就全清掉
    if (ssid == NULL || ssid[0] == '\0') {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "清 NVS 里的 WiFi 失败：%s", esp_err_to_name(err));
        }
        nvs_close(h);
        return err;
    }

    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", (pass != NULL) ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "存 WiFi 账号失败：%s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

// 按当前账号配并重连
static esp_err_t wifi_apply(void) {
    wifi_config_t cfg = {0};

    copy_into_u8(cfg.sta.ssid, sizeof(cfg.sta.ssid), s_ssid);
    copy_into_u8(cfg.sta.password, sizeof(cfg.sta.password), s_pass);
    // 没密码当开放网
    cfg.sta.threshold.authmode = (s_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "换 WiFi 配置失败：%s", esp_err_to_name(err));
        return err;
    }

    // 退避从头来
    s_retry      = 0;
    s_backoff_ms = NET_BACKOFF_MIN_MS;
    if (s_retry_timer != NULL) {
        (void)esp_timer_stop(s_retry_timer);
    }

    (void)esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        // 断开事件会自己再连
        ESP_LOGW(TAG, "立刻重连没成(%s)，等自动重连", esp_err_to_name(err));
    }
    return ESP_OK;
}

// 换 WiFi 账号用
esp_err_t net_link_set_wifi(const char *ssid, const char *pass) {
    const char *s = (ssid != NULL) ? ssid : "";
    const char *p = (pass != NULL) ? pass : "";

    const esp_err_t err = wifi_cred_save(s, p);
    if (err != ESP_OK) {
        return err;
    }

    if (s[0] == '\0') {
        // 回到编译时的默认值
        snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_APP_WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", CONFIG_APP_WIFI_PASSWORD);
        ESP_LOGW(TAG, "WiFi 账号已清除，回到编译时的 \"%s\"", s_ssid);
    } else {
        snprintf(s_ssid, sizeof(s_ssid), "%s", s);
        snprintf(s_pass, sizeof(s_pass), "%s", p);
        ESP_LOGI(TAG, "新 WiFi 账号 \"%s\" 已存 NVS", s_ssid);
    }

    if (!s_inited) {
        ESP_LOGW(TAG, "联网还没起，账号先存着");
        return ESP_ERR_INVALID_STATE;
    }

    return wifi_apply();
}

// 失败也返回 OK
esp_err_t net_link_init(void) {
    if (s_inited) {
        if (s_stopped) {
            s_stopped    = false;
            s_backoff_ms = NET_BACKOFF_MIN_MS;
            ESP_LOGI(TAG, "net_link 从 stop 状态恢复，重新联网");
            (void)esp_wifi_connect();
            if (s_client != NULL && !s_mqtt_started) {
                if (esp_mqtt_client_start(s_client) == ESP_OK) {
                    s_mqtt_started = true;
                }
            }
        }
        return ESP_OK;
    }

    // NVS 失败也照连
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除重建（%s）", esp_err_to_name(err));
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s)：WiFi 校准数据无法保存，仍然尝试联网", esp_err_to_name(err));
    }

    // 1) 拼 topic
    if (topics_build() != ESP_OK) {
        ESP_LOGE(TAG, "topic 拼装失败（检查 CONFIG_APP_MQTT_TOPIC_PREFIX / UID 长度）：联网功能不可用，"
                      "本地语音/按键/屏幕不受影响");
        s_inited = true;
        return ESP_OK;
    }

    // 2) netif
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败：%s", esp_err_to_name(err));
        goto degrade;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // 已建过不算错
        ESP_LOGE(TAG, "esp_event_loop_create_default 失败：%s", esp_err_to_name(err));
        goto degrade;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta 失败");
        goto degrade;
    }

    // 3) WiFi 驱动
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err                         = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // 已 init 可续用
        ESP_LOGE(TAG, "esp_wifi_init 失败：%s", esp_err_to_name(err));
        goto degrade;
    }

    // 4) 退避定时器
    const esp_timer_create_args_t timer_cfg = {
        .callback        = retry_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "net_retry",
    };
    err = esp_timer_create(&timer_cfg, &s_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重连定时器创建失败(%s)：退化为立即重连", esp_err_to_name(err));
        s_retry_timer = NULL;
    }

    // 5) 注册事件
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WIFI_EVENT 回调失败：%s", esp_err_to_name(err));
        goto degrade;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP_EVENT 回调失败：%s", esp_err_to_name(err));
        goto degrade;
    }

    // 6) WiFi 参数
    // 优先用 NVS
    wifi_cred_load();

    wifi_config_t wifi_cfg = {0};
    copy_into_u8(wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), s_ssid);
    copy_into_u8(wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), s_pass);
    // 开放网设 OPEN
    wifi_cfg.sta.threshold.authmode = (s_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode 失败：%s", esp_err_to_name(err));
        goto degrade;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config 失败：%s", esp_err_to_name(err));
        goto degrade;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start 失败：%s", esp_err_to_name(err));
        goto degrade;
    }

    // 7) MQTT 配置
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri    = CONFIG_APP_MQTT_BROKER_URI,
        .credentials.client_id = s_client_id,
        .session =
            {
                      .keepalive = CONFIG_APP_MQTT_KEEPALIVE_S,
                      // LWT 遗嘱
                .last_will =
                    {
                        .topic = s_topic_availability,
                        .msg   = "offline",
                        // 0 = 自动算长
                        .msg_len = 0,
                        .qos     = 1,
                        .retain  = 1,
                    }, },
        // 缓冲够装最长包
        .buffer.size = NET_JSON_STATE_LEN + 256,
    };

    // 空用户名别设
    if (CONFIG_APP_MQTT_USERNAME[0] != '\0') {
        mqtt_cfg.credentials.username                = CONFIG_APP_MQTT_USERNAME;
        mqtt_cfg.credentials.authentication.password = CONFIG_APP_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init 失败（broker 地址 \"%s\" 是否合法？）",
                 CONFIG_APP_MQTT_BROKER_URI);
        goto degrade;
    }

    err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 MQTT 事件回调失败：%s", esp_err_to_name(err));
        goto degrade;
    }

    // 先不 start

    // 8) 不注册语音回调

    s_inited         = true;
    s_need_republish = true;
    s_state          = NET_STATE_DOWN;

    ESP_LOGI(TAG, "started: broker=%s client_id=%s", CONFIG_APP_MQTT_BROKER_URI, s_client_id);
    ESP_LOGI(TAG, "topics: state=%s cmd=%s sensor=%s weather=%s", s_topic_state, s_topic_cmd_sub,
             s_topic_sensor, s_topic_weather);
    return ESP_OK;

degrade:
    // 失败不 abort
    s_inited  = true;
    s_wifi_up = false;
    s_mqtt_up = false;
    s_state   = NET_STATE_DOWN;
    ESP_LOGE(TAG, "联网初始化未完成：手机端不可用，其余功能照常运行");
    return ESP_OK;
}

// 串口调试用
// 别在回调里调
void net_link_stop(void) {
    s_stopped = true;

    // 先停重连定时器
    if (s_retry_timer != NULL) {
        (void)esp_timer_stop(s_retry_timer);
    }

    if (s_client != NULL && s_mqtt_started) {
        (void)esp_mqtt_client_stop(s_client);
        s_mqtt_started = false;
    }

    if (s_inited) {
        (void)esp_wifi_disconnect();
    }

    s_mqtt_up = false;
    s_wifi_up = false;
    s_state   = NET_STATE_DOWN;
    snprintf(s_ip, sizeof(s_ip), "0.0.0.0");

    ESP_LOGI(TAG, "已停止联网（本地语音/按键/屏幕/自动联动不受影响；再调 net_link_init() 可恢复）");
}

net_state_t net_link_get_state(void) {
    return s_state;
}

bool net_link_wifi_ok(void) {
    return s_wifi_up;
}

bool net_link_mqtt_ok(void) {
    return s_mqtt_up;
}

const char *net_link_ip(void) {
    return s_ip;
}

// 没连就返回 0
int8_t net_link_rssi(void) {
    if (!s_wifi_up) {
        return 0;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

// 懒加载
const char *net_link_topic_base(void) {
    if (s_base[0] == '\0') {
        (void)topics_build();
    }
    return s_base;
}

// 打印全部 topic
void net_link_dump(void) {
    const int8_t rssi = net_link_rssi();

    ESP_LOGI(TAG, "=========== net_link 状态 ===========");
    ESP_LOGI(TAG, "状态     : %s", state_name(s_state));
    ESP_LOGI(TAG, "WiFi     : %s   SSID=\"%s\"   IP=%s   RSSI=%d dBm   重连次数=%d   退避=%u ms",
             s_wifi_up ? "已连接" : "未连接", active_ssid(), s_ip, (int)rssi, s_retry,
             (unsigned)s_backoff_ms);
    ESP_LOGI(TAG, "MQTT     : %s   broker=%s   client_id=%s   待补发=%s", s_mqtt_up ? "已连接" : "未连接",
             CONFIG_APP_MQTT_BROKER_URI, s_client_id, s_need_republish ? "是" : "否");
    ESP_LOGI(TAG, "UID      : %s", net_link_uid());
    ESP_LOGI(TAG, "base     : %s", net_link_topic_base());
    ESP_LOGI(TAG, "--- 上行 topic ---");
    ESP_LOGI(TAG, "  availability : %s   (retain=1, qos=1)", s_topic_availability);
    ESP_LOGI(TAG, "  state        : %s   (retain=1, qos=1)", s_topic_state);
    ESP_LOGI(TAG, "  sensor       : %s   (retain=0, qos=0, 周期 %d ms)", s_topic_sensor,
             CONFIG_APP_MQTT_PUBLISH_SENSOR_MS);
    ESP_LOGI(TAG, "  weather      : %s   (retain=1, qos=1)", s_topic_weather);
    ESP_LOGI(TAG, "  ack          : %s   (retain=0, qos=1)", s_topic_ack);
    ESP_LOGI(TAG, "  event        : %s   (retain=0, qos=1)", s_topic_event);
    ESP_LOGI(TAG, "  config       : %s   (retain=1, qos=1)", s_topic_config);
    ESP_LOGI(TAG, "--- 下行 topic（已订阅 qos=1）---");
    ESP_LOGI(TAG, "  cmd          : %s", s_topic_cmd_sub);
    ESP_LOGI(TAG, "  config       : %s", s_topic_config);
    ESP_LOGI(TAG, "  get          : %s", s_topic_get);
    ESP_LOGI(TAG, "====================================");
}
