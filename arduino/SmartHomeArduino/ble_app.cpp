// c
// 蓝牙 GATT 服务端

#include <stdio.h>
#include <string.h>

// 核心自带的 BLE 库，头文件都在 libraries\BLE\src 下
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#include "app_config.h"

#include "ble_app.h"
#include "net_link.h"
#include "mqtt_protocol.h"
#include "app_cmd.h"
// 阈值读写
#include "automation.h"
#include "device_model.h"

static const char *TAG = "BLE";

// 广播名前缀
#define BLE_NAME_PREFIX APP_BLE_NAME_PREFIX

// 名字最长 29 字节
#define BLE_NAME_MAX_LEN 29

// 下行帧类型
#define BLE_DOWN_CMD    0x01
#define BLE_DOWN_CONFIG 0x02
#define BLE_DOWN_GET    0x03
#define BLE_DOWN_WIFI   0x04
#define BLE_DOWN_PING   0x05

// 上行帧类型
#define BLE_UP_STATE   0x11
#define BLE_UP_SENSOR  0x12
#define BLE_UP_WEATHER 0x13
#define BLE_UP_CONFIG  0x14
#define BLE_UP_ACK     0x15
#define BLE_UP_INFO    0x16

// 类型在低 7 位
#define BLE_TYPE_MASK 0x7F
// 高位=还有后片
#define BLE_MORE_BIT  0x80

// ATT 头 3 字节
#define BLE_ATT_OVERHEAD 3

// 重组上限 1024
#define BLE_RX_FRAME_MAX 1024
#define BLE_RX_BUF_LEN   (BLE_RX_FRAME_MAX + 1)

// INFO 装得下
#define BLE_JSON_INFO_LEN 192

// 名最长 32
#define BLE_WIFI_SSID_MAX 33
// 密码最长 64
#define BLE_WIFI_PASS_MAX 65

// 版本号给 App 看
#define BLE_APP_VER "1.0.0"

// 三个号照抄，别改
#define BLE_SVC_UUID "5a1c0001-1e2f-4a3b-9c4d-5e6f708192a3"
#define BLE_RX_UUID  "5a1c0002-1e2f-4a3b-9c4d-5e6f708192a3"
#define BLE_TX_UUID  "5a1c0003-1e2f-4a3b-9c4d-5e6f708192a3"

static BLEServer         *s_server;
static BLECharacteristic *s_tx;

static bool s_connected;
// 订阅了才能发
static bool s_tx_subscribed;

static char s_name[BLE_NAME_MAX_LEN + 1];

// 重组缓冲
static uint8_t s_rx[BLE_RX_BUF_LEN];
static int     s_rx_len;
// 当前帧类型
static uint8_t s_rx_type;

// 上行分片用
static uint8_t s_tx_buf[BLE_RX_BUF_LEN + 2];

// MTU 优先 512
static int s_chunk_max = 512 - BLE_ATT_OVERHEAD;

// 发一帧，长了自动分片
static void ble_send(uint8_t type, const char *json, int len) {
    if (!s_connected || !s_tx_subscribed || s_tx == NULL) {
        return;
    }
    if (json == NULL || len < 0) {
        return;
    }

    int at = 0;
    while (at < len) {
        int n = len - at;
        if (n > s_chunk_max) {
            n = s_chunk_max;
        }
        // 还有后片就置高位
        const bool more = (at + n) < len;

        s_tx_buf[0] = (uint8_t)((type & BLE_TYPE_MASK) | (more ? BLE_MORE_BIT : 0x00));
        memcpy(s_tx_buf + 1, json + at, (size_t)n);
        s_tx_buf[1 + n] = '\0';

        s_tx->setValue(s_tx_buf, (size_t)(1 + n));
        s_tx->notify();

        at += n;
    }
}

// 自己拼 INFO
void ble_app_notify_info(void) {
    char buf[BLE_JSON_INFO_LEN];

    const int n = snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"ver\":\"%s\",\"base\":\"%s\",\"ble_mtu\":%d}",
                           s_name, BLE_APP_VER, net_link_topic_base(), s_chunk_max + BLE_ATT_OVERHEAD);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        LOGW(TAG, "INFO JSON 放不下");
        return;
    }
    ble_send(BLE_UP_INFO, buf, n);
}

void ble_app_notify_state(const char *json, int len) {
    ble_send(BLE_UP_STATE, json, len);
}

void ble_app_notify_sensor(const char *json, int len) {
    ble_send(BLE_UP_SENSOR, json, len);
}

void ble_app_notify_weather(const char *json, int len) {
    ble_send(BLE_UP_WEATHER, json, len);
}

void ble_app_notify_config(const char *json, int len) {
    ble_send(BLE_UP_CONFIG, json, len);
}

void ble_app_notify_ack(const char *json, int len) {
    ble_send(BLE_UP_ACK, json, len);
}

// 从 JSON 里抠出字段
static bool json_get_kv(const char *json, const char *key, char *out, size_t out_len) {
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

    if (*p == '\"') {
        // 字符串
        p++;
        size_t i = 0;
        while (*p != '\0' && *p != '\"' && i < (out_len - 1)) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return true;
    }

    // 裸值（数字或 true/false）
    size_t i = 0;
    while (*p != '\0' && *p != ',' && *p != '}' && i < (out_len - 1)) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0);
}

// 0x01 CMD
// 三种写法都认
static void ble_handle_cmd(const char *text) {
    char detail[HOME_CMD_DETAIL_LEN] = {0};

    // 写法一：完整 JSON
    if (text[0] == '{') {
        char dev[32]    = {0};
        char action[40] = {0};

        // 没 dev 就不是 JSON 写法
        if (!json_get_kv(text, "dev", dev, sizeof(dev))) {
            (void)net_link_publish_ack(NULL, NULL, false, "payload 解析失败");
            LOGW(TAG, "CMD JSON 里没有 dev: %s", text);
            return;
        }

        // value 是数字键才用得上
        char value[16] = {0};
        int  pct       = -1;
        if (json_get_kv(text, "pct", value, sizeof(value))) {
            pct = atoi(value);
        } else if (json_get_kv(text, "value", value, sizeof(value))) {
            // CONFIG 的写法复用过来
            if (value[0] >= '0' && value[0] <= '9') {
                pct = atoi(value);
            }
            snprintf(action, sizeof(action), "%s", "set");
        }

        if (action[0] == '\0' && !json_get_kv(text, "action", action, sizeof(action))) {
            (void)net_link_publish_ack(dev, NULL, false, "payload 解析失败");
            LOGW(TAG, "CMD JSON 里没有 action: %s", text);
            return;
        }

        const app_err_t err = home_command_execute(dev, action, pct, SRC_APP, detail);
        (void)net_link_publish_ack(dev, action, err == APP_OK, detail);

        if (err == APP_OK) {
            LOGI(TAG, "CMD ok: %s/%s", dev, action);
        } else {
            LOGW(TAG, "CMD 失败: %s/%s → %s", dev, action, detail);
        }
        return;
    }

    // 写法二：dev=xxx,action=yyy
    if (strchr(text, '=') != NULL) {
        char dev[32]    = {0};
        char action[40] = {0};

        const char *p = strstr(text, "dev=");
        if (p != NULL) {
            p += 4;
            size_t i = 0;
            while (*p != '\0' && *p != ',' && i < (sizeof(dev) - 1)) {
                dev[i++] = *p++;
            }
            dev[i] = '\0';
        }
        p = strstr(text, "action=");
        if (p != NULL) {
            p += 7;
            size_t i = 0;
            while (*p != '\0' && *p != ',' && i < (sizeof(action) - 1)) {
                action[i++] = *p++;
            }
            action[i] = '\0';
        }

        if (dev[0] == '\0' || action[0] == '\0') {
            (void)net_link_publish_ack(NULL, NULL, false, "payload 解析失败");
            LOGW(TAG, "CMD 键值对缺字段: %s", text);
            return;
        }

        const app_err_t err = home_command_execute(dev, action, -1, SRC_APP, detail);
        (void)net_link_publish_ack(dev, action, err == APP_OK, detail);
        return;
    }

    // 写法三：led_living_on
    // 末段是动作，前面是设备
    {
        char token[64];
        snprintf(token, sizeof(token), "%s", text);

        char *last = strrchr(token, '_');
        if (last == NULL) {
            (void)net_link_publish_ack(NULL, NULL, false, "payload 解析失败");
            LOGW(TAG, "CMD 短名认不出: %s", text);
            return;
        }
        *last       = '\0';
        char *dev    = token;
        char *action = last + 1;

        const app_err_t err = home_command_execute(dev, action, -1, SRC_APP, detail);
        (void)net_link_publish_ack(dev, action, err == APP_OK, detail);
    }
}

// 0x02 CONFIG
static void ble_handle_config(const char *text) {
    char dev[32]  = {0};
    char key[32]  = {0};
    char val[24]  = {0};

    (void)json_get_kv(text, "dev", dev, sizeof(dev));
    if (!json_get_kv(text, "action", key, sizeof(key))) {
        (void)net_link_publish_ack(dev, NULL, false, "payload 解析失败");
        LOGW(TAG, "CONFIG 里没有 action(键名): %s", text);
        return;
    }

    // 布尔键必须是 JSON 的 true/false
    if (!json_get_kv(text, "value", val, sizeof(val))) {
        (void)net_link_publish_ack(dev, key, false, "payload 解析失败");
        LOGW(TAG, "CONFIG 里没有 value: %s", text);
        return;
    }

    const app_err_t err = auto_policy_set_threshold(key, val);
    char            detail[HOME_CMD_DETAIL_LEN] = {0};

    if (err == APP_OK) {
        snprintf(detail, sizeof(detail), "已更新 %s=%s", key, val);
    } else if (err == APP_ERR_INVALID_ARG) {
        snprintf(detail, sizeof(detail), "未知配置项: %s", key);
    } else {
        snprintf(detail, sizeof(detail), "%s=%s 不合法（改上下限请先发 on 再发 off）", key, val);
    }

    (void)net_link_publish_ack("config", key, err == APP_OK, detail);

    // 阈值改了立刻回一次全量
    (void)net_link_publish_config();
    (void)net_link_publish_state();
}

// 0x04 WIFI
static void ble_handle_wifi(const char *text) {
    char ssid[BLE_WIFI_SSID_MAX] = {0};
    char pass[BLE_WIFI_PASS_MAX] = {0};

    (void)json_get_kv(text, "ssid", ssid, sizeof(ssid));
    (void)json_get_kv(text, "pass", pass, sizeof(pass));

    // 空 ssid 表示清除
    if (ssid[0] == '\0') {
        (void)net_link_set_wifi(APP_WIFI_SSID, APP_WIFI_PASS);
        (void)net_link_publish_ack("wifi", "clear", true, "已清除手机下发的 WiFi");
        LOGW(TAG, "收到空 ssid，回到编译时的默认 WiFi");
        return;
    }

    const app_err_t err = net_link_set_wifi(ssid, pass);
    (void)net_link_publish_ack("wifi", "set", err == APP_OK, (err == APP_OK) ? "已保存并重连" : "保存失败");

    // 让 App 看到新 IP
    (void)net_link_publish_state();
}

// 一帧收全了再进来
static void ble_handle_frame(uint8_t type, const char *data) {
    switch (type) {
    case BLE_DOWN_CMD:
        ble_handle_cmd(data);
        break;

    case BLE_DOWN_CONFIG:
        ble_handle_config(data);
        break;

    case BLE_DOWN_GET:
        // 四帧全量
        (void)net_link_publish_all();
        break;

    case BLE_DOWN_WIFI:
        ble_handle_wifi(data);
        break;

    case BLE_DOWN_PING:
        ble_app_notify_info();
        break;

    default:
        // 不断连接
        LOGW(TAG, "收到不认识的帧类型 0x%02X，忽略", (unsigned)type);
        break;
    }
}

// 收分片并重组
static void ble_feed(const uint8_t *data, int len) {
    if (data == NULL || len <= 0) {
        return;
    }

    const uint8_t head = data[0];
    const uint8_t type = (uint8_t)(head & BLE_TYPE_MASK);
    const bool    more = (head & BLE_MORE_BIT) != 0;
    const int     n    = len - 1;

    // 换了类型就从头上重来
    if (s_rx_len == 0) {
        s_rx_type = type;
    } else if (type != s_rx_type) {
        LOGW(TAG, "分片类型对不上（%u != %u），丢弃重来", (unsigned)type, (unsigned)s_rx_type);
        s_rx_len  = 0;
        s_rx_type = type;
    }

    if ((s_rx_len + n) > BLE_RX_FRAME_MAX) {
        LOGW(TAG, "重组超过 %d 字节，丢弃整帧", BLE_RX_FRAME_MAX);
        s_rx_len = 0;
        return;
    }

    memcpy(s_rx + s_rx_len, data + 1, (size_t)n);
    s_rx_len += n;
    s_rx[s_rx_len] = '\0';

    if (more) {
        return;
    }

    // 收全了
    ble_handle_frame(s_rx_type, (const char *)s_rx);
    s_rx_len = 0;
}

// 收写请求
class BleRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *ch) override {
        if (ch == NULL) {
            return;
        }
        const std::string v = ch->getValue();
        if (v.length() == 0) {
            return;
        }
        ble_feed((const uint8_t *)v.data(), (int)v.length());
    }
};

// 订阅通知
class BleTxCallbacks : public BLECharacteristicCallbacks {
    void onStatus(BLECharacteristic *ch, Status s, uint32_t code) override {
        (void)ch;
        (void)code;
        if (s == SUCCESS_NOTIFY || s == SUCCESS_INDICATE) {
            // 收到 CCCD 写入才算订上
            s_tx_subscribed = true;
            LOGI(TAG, "手机已订阅 TX，可以发数据了");
            ble_app_notify_info();
        } else if (s == ERROR_NOTIFY_DISABLED) {
            s_tx_subscribed = false;
            LOGD(TAG, "手机取消了 TX 订阅");
        }
    }
};

// 连接与断开
class BleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *srv) override {
        (void)srv;
        s_connected     = true;
        s_tx_subscribed = false;
        s_rx_len        = 0;
        LOGI(TAG, "手机已连接");
    }

    void onDisconnect(BLEServer *srv) override {
        s_connected     = false;
        s_tx_subscribed = false;
        s_rx_len        = 0;
        LOGI(TAG, "手机已断开，重新开广播");
        // 断了要重新广播
        if (srv != NULL) {
            srv->startAdvertising();
        }
    }

    void onMtuChanged(BLEServer *srv, esp_ble_gatts_cb_param_t *param) override {
        (void)srv;
        if (param == NULL) {
            return;
        }

        const int mtu = param->mtu.mtu;
        const int chunk = mtu - BLE_ATT_OVERHEAD;
        if (chunk > 0) {
            s_chunk_max = chunk;
        }
        LOGI(TAG, "MTU 协商完成: %d（一片最多 %d 字节）", mtu, s_chunk_max);
    }
};

// 起蓝牙并开广播
// 不等连接，失败只返错
app_err_t ble_app_start(void) {
    // 名字带 UID
    snprintf(s_name, sizeof(s_name), "%s-%s", BLE_NAME_PREFIX, net_link_uid());

    BLEDevice::init(s_name);

    // 512 是 NimBLE 上限
    BLEDevice::setMTU(512);

    s_server = BLEDevice::createServer();
    if (s_server == NULL) {
        LOGE(TAG, "创建 GATT 服务端失败");
        return APP_ERR_FAIL;
    }
    s_server->setCallbacks(new BleServerCallbacks());

    BLEService *svc = s_server->createService(BLE_SVC_UUID);
    if (svc == NULL) {
        LOGE(TAG, "创建 Service 失败");
        return APP_ERR_FAIL;
    }

    s_tx = svc->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    s_tx->addDescriptor(new BLE2902());
    s_tx->setCallbacks(new BleTxCallbacks());

    BLECharacteristic *rx =
        svc->createCharacteristic(BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new BleRxCallbacks());

    // 不收 bonded 的老数据
    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SVC_UUID);
    adv->setScanResponse(true);
    // 兼容部分手机的连接参数
    adv->setMinPreferred(0x06);

    BLEDevice::startAdvertising();

    LOGI(TAG, "蓝牙已启动：设备名 \"%s\"（用 App 扫描列表里的这个名字）", s_name);
    LOGI(TAG, "          连上后 App 会自己显示 base topic，不用翻串口");
    return APP_OK;
}

// 手机连上没有
bool ble_app_is_connected(void) {
    return s_connected;
}

// 广播名带后 6 位 ID
const char *ble_app_name(void) {
    return s_name;
}
