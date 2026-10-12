// c
// 蓝牙 GATT 服务端

#include "ble_app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_BT_ENABLED || !CONFIG_APP_BLE_ENABLE

static const char *TAG = "BLE";

// 没编进来就空跑
// 失败只记日志
esp_err_t ble_app_start(void) {
    ESP_LOGW(TAG, "蓝牙没编进固件，手机直连不可用（MQTT 照常）");
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_app_is_connected(void) {
    return false;
}

// 五个上行都空着
void ble_app_notify_state(const char *json, int len) {
    (void)json;
    (void)len;
}

void ble_app_notify_sensor(const char *json, int len) {
    (void)json;
    (void)len;
}

void ble_app_notify_weather(const char *json, int len) {
    (void)json;
    (void)len;
}

void ble_app_notify_config(const char *json, int len) {
    (void)json;
    (void)len;
}

void ble_app_notify_ack(const char *json, int len) {
    (void)json;
    (void)len;
}

void ble_app_notify_info(void) {
}

#else

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// 上行和同名口都在这
#include "net_link.h"
// 回执文案
#include "mqtt_protocol.h"
// 拼 JSON 靠它
#include "app_cmd.h"
#include "cJSON.h"

static const char *TAG = "BLE";

// 原型要自己声明
void ble_store_config_init(void);

// 广播名前缀
#define BLE_NAME_PREFIX  CONFIG_APP_BLE_DEVICE_NAME_PREFIX

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

// MTU 优先 512
#define BLE_MTU_PREFERRED 512
// ATT 头 3 字节
#define BLE_ATT_OVERHEAD  3

// 一条写最长这么多
#define BLE_RX_CHUNK_MAX (BLE_MTU_PREFERRED + 1)
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

// 三个号小端写
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xa3, 0x92, 0x81, 0x70, 0x6f, 0x5e, 0x4d, 0x9c, 0x3b, 0x4a, 0x2f, 0x1e, 0x01, 0x00, 0x1c, 0x5a);

static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0xa3, 0x92, 0x81, 0x70, 0x6f, 0x5e, 0x4d, 0x9c, 0x3b, 0x4a, 0x2f, 0x1e, 0x02, 0x00, 0x1c, 0x5a);

static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0xa3, 0x92, 0x81, 0x70, 0x6f, 0x5e, 0x4d, 0x9c, 0x3b, 0x4a, 0x2f, 0x1e, 0x03, 0x00, 0x1c, 0x5a);

// 防重复启动
static bool s_started = false;
// 广播名
static char s_dev_name[BLE_NAME_MAX_LEN + 1] = {0};
// 连着才有号
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
// 发通知要用
static uint16_t s_tx_val_handle = 0;
// 订了 TX 才发
static volatile bool s_tx_subscribed = false;
// 广播用哪种地址
static uint8_t s_own_addr_type = 0;

// 收包暂存
static uint8_t s_rx_chunk[BLE_RX_CHUNK_MAX];
// 拼帧的地方
static uint8_t s_rx_buf[BLE_RX_BUF_LEN];
static int     s_rx_len  = 0;
static uint8_t s_rx_type = 0;

static int  ble_gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_start_advertising(void);
static void ble_frame_input(const uint8_t *data, int len);
static void ble_frame_dispatch(uint8_t type, const char *json, int len);
static void ble_handle_wifi(const char *json, int len);
static esp_err_t ble_send_frame(uint8_t type, const char *json, int len);

// 地址打成字
static void ble_format_addr(const uint8_t addr[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// 前缀-UID
static esp_err_t ble_build_device_name(void) {
    char        fallback[NET_UID_LEN] = {0};
    const char *uid                   = net_link_uid();

    if (uid == NULL || uid[0] == '\0') {
        // 拿不到就自己算
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
            ESP_LOGE(TAG, "UID 取不到，名字拼不出来");
            return ESP_FAIL;
        }
        snprintf(fallback, sizeof(fallback), "%02x%02x%02x", mac[3], mac[4], mac[5]);
        uid = fallback;
    }

    const int n = snprintf(s_dev_name, sizeof(s_dev_name), "%s-%s", BLE_NAME_PREFIX, uid);
    if (n < 0 || n >= (int)sizeof(s_dev_name)) {
        ESP_LOGW(TAG, "名字太长，截成 \"%s\"", s_dev_name);
    }
    return ESP_OK;
}

// TX 只发不读
static int ble_gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    ESP_LOGW(TAG, "TX 只发通知，别来读写 op=%d", ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

// 一个服务挂两个特征
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // 手机往这写
                .uuid      = &s_rx_uuid.u,
                .access_cb = ble_gatt_rx_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 板子往这发
                .uuid       = &s_tx_uuid.u,
                .access_cb  = ble_gatt_tx_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            {
                // 特征到此
                0,
            },
        },
    },
    {
        // 表结束
        0,
    },
};

// 开广播，播到有人连
static void ble_start_advertising(void) {
    struct ble_hs_adv_fields  adv    = {0};
    struct ble_hs_adv_fields  rsp    = {0};
    struct ble_gap_adv_params params = {0};
    int                       rc;

    // 服务号放主包
    adv.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128             = &s_svc_uuid;
    adv.num_uuids128         = 1;
    adv.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播数据设不上 rc=%d", rc);
        return;
    }

    // 名字放扫描响应
    rsp.name             = (uint8_t *)s_dev_name;
    rsp.name_len         = (uint8_t)strlen(s_dev_name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "扫描响应设不上 rc=%d（名字超 31 字节？）", rc);
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min  = BLE_GAP_ADV_ITVL_MS(100);
    params.itvl_max  = BLE_GAP_ADV_ITVL_MS(110);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "开广播失败 rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "广播中：名 \"%s\"，Service 在广播包里", s_dev_name);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "连接没成 status=%d，重开广播", event->connect.status);
            ble_start_advertising();
            return 0;
        }

        struct ble_gap_conn_desc desc;
        char                     addr[18] = "??";

        s_conn_handle   = event->connect.conn_handle;
        // 新连接要重订
        s_tx_subscribed = false;

        if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
            ble_format_addr(desc.peer_id_addr.val, addr);
        }
        ESP_LOGI(TAG, "手机连上了 handle=%d addr=%s", s_conn_handle, addr);

        // 再声明一次大包
        (void)ble_att_set_preferred_mtu(BLE_MTU_PREFERRED);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "手机断了 handle=%d reason=%d", event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        // 清干净，别再往那边发
        s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
        s_tx_subscribed = false;
        s_rx_len        = 0;
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "广播结束 reason=%d，重开", event->adv_complete.reason);
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d，单片净载荷 %d 字节", event->mtu.value, event->mtu.value - BLE_ATT_OVERHEAD);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // 只认 TX 那个号
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_tx_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "TX 通知%s", s_tx_subscribed ? "已打开" : "已关闭");

            // 刚订上就推一遍全量
            if (s_tx_subscribed) {
                (void)net_link_publish_all();
            }
        }
        return 0;

    default:
        return 0;
    }
}

// 收片拼帧
static void ble_frame_input(const uint8_t *data, int len) {
    if (data == NULL || len < 1) {
        return;
    }

    const uint8_t raw  = data[0];
    const uint8_t type = raw & BLE_TYPE_MASK;
    const bool    more = (raw & BLE_MORE_BIT) != 0;
    const int     n    = len - 1;

    if (s_rx_len == 0) {
        s_rx_type = type;
    } else if (type != s_rx_type) {
        // 换类型了，前面作废
        ESP_LOGW(TAG, "分片类型变了 0x%02X→0x%02X，重来", s_rx_type, type);
        s_rx_len  = 0;
        s_rx_type = type;
    }

    if (s_rx_len + n > BLE_RX_FRAME_MAX) {
        ESP_LOGW(TAG, "拼到 %d 字节超上限，整帧丢掉", s_rx_len + n);
        s_rx_len = 0;
        return;
    }

    if (n > 0) {
        memcpy(s_rx_buf + s_rx_len, data + 1, (size_t)n);
        s_rx_len += n;
    }

    if (more) {
        return;
    }

    // 收全了就分发
    const uint8_t t     = s_rx_type;
    const int     total = s_rx_len;

    s_rx_len        = 0;
    s_rx_buf[total] = '\0';
    ble_frame_dispatch(t, (const char *)s_rx_buf, total);
}

// 按类型分发
static void ble_frame_dispatch(uint8_t type, const char *json, int len) {
    switch (type) {
    case BLE_DOWN_CMD:
    case BLE_DOWN_CONFIG: {
        // 走同一条路
        char detail[HOME_CMD_DETAIL_LEN] = {0};
        (void)net_link_handle_cmd_payload(json, len, detail, sizeof(detail));
        break;
    }

    case BLE_DOWN_GET:
        // 四帧全量刷一遍
        ESP_LOGI(TAG, "手机要全量，发 state/sensor/weather/config");
        (void)net_link_publish_all();
        break;

    case BLE_DOWN_WIFI:
        ble_handle_wifi(json, len);
        break;

    case BLE_DOWN_PING:
        ble_app_notify_info();
        break;

    default:
        // 不认识的号不理
        ESP_LOGW(TAG, "不认识的帧 0x%02X，忽略（%d 字节）", type, len);
        break;
    }
}

// 回一帧指令结果
static void ble_send_ack(const char *dev, const char *action, bool ok, const char *detail) {
    char      buf[NET_JSON_ACK_LEN];
    const int n = net_proto_ack_json(buf, sizeof(buf), dev, action, ok, detail);
    if (n <= 0) {
        return;
    }
    ble_app_notify_ack(buf, n);
}

// 存账号并重连
static void ble_handle_wifi(const char *json, int len) {
    char   ssid[BLE_WIFI_SSID_MAX] = {0};
    char   pass[BLE_WIFI_PASS_MAX] = {0};
    bool   got  = false;
    cJSON *root = NULL;

    if (len > 0) {
        root = cJSON_ParseWithLength(json, (size_t)len);
    }

    if (root != NULL && cJSON_IsObject(root)) {
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "pass");

        if (js != NULL && cJSON_IsString(js) && js->valuestring != NULL) {
            snprintf(ssid, sizeof(ssid), "%s", js->valuestring);
            got = true;
        }
        if (jp != NULL && cJSON_IsString(jp) && jp->valuestring != NULL) {
            snprintf(pass, sizeof(pass), "%s", jp->valuestring);
        }
    }

    if (root != NULL) {
        cJSON_Delete(root);
    }

    if (!got) {
        ESP_LOGW(TAG, "WIFI 帧里没有 ssid");
        ble_send_ack("wifi", "set", false, "要带 ssid 字段");
        return;
    }

    const esp_err_t err = net_link_set_wifi(ssid, pass);
    if (err == ESP_ERR_INVALID_STATE) {
        // 联网通道还没起来
        ESP_LOGW(TAG, "联网通道没起，账号先存着");
        ble_send_ack("wifi", "set", false, "网络通道未就绪");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 账号没存上：%s", esp_err_to_name(err));
        ble_send_ack("wifi", "set", false, "存不进 NVS，看串口日志");
        return;
    }

    // 空串就是清掉
    ble_send_ack("wifi", "set", true, (ssid[0] == '\0') ? "已清除，回出厂 WiFi" : "已保存，正在连");
}

static int ble_gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    // 只收写
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "RX 只收写，拒掉 op=%d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);
    if (pkt_len == 0) {
        ESP_LOGW(TAG, "空包，忽略");
        return 0;
    }
    if (pkt_len > BLE_RX_CHUNK_MAX) {
        ESP_LOGE(TAG, "单包 %u 字节超上限 %u", (unsigned)pkt_len, (unsigned)BLE_RX_CHUNK_MAX);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t  copied = 0;
    const int rc     = ble_hs_mbuf_to_flat(ctxt->om, s_rx_chunk, (uint16_t)sizeof(s_rx_chunk), &copied);
    if (rc != 0) {
        ESP_LOGE(TAG, "取数据失败 rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    ble_frame_input(s_rx_chunk, (int)copied);
    return 0;
}

// 订了才值得发
static bool ble_can_notify(void) {
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) && s_tx_subscribed;
}

// 加分片发一帧
static esp_err_t ble_send_frame(uint8_t type, const char *json, int len) {
    const uint16_t conn = s_conn_handle;

    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (json == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int mtu = ble_att_mtu(conn);
    if (mtu <= 0) {
        // 还没协商好就按最小算
        mtu = BLE_MTU_PREFERRED;
    }

    // 一片能带的净数据
    int per = mtu - BLE_ATT_OVERHEAD - 1;
    if (per < 1) {
        per = 1;
    }

    int off = 0;
    do {
        int n = len - off;
        if (n > per) {
            n = per;
        }

        const bool more = (off + n) < len;
        const int  flen = n + 1;

        uint8_t *frame = (uint8_t *)malloc((size_t)flen);
        if (frame == NULL) {
            ESP_LOGE(TAG, "组帧 malloc(%d) 失败", flen);
            return ESP_ERR_NO_MEM;
        }

        // 头一字节放类型
        frame[0] = (uint8_t)(type | (more ? BLE_MORE_BIT : 0x00));
        memcpy(frame + 1, json + off, (size_t)n);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)flen);
        free(frame);
        if (om == NULL) {
            ESP_LOGE(TAG, "mbuf 要不到（%d 字节）", flen);
            return ESP_ERR_NO_MEM;
        }

        const int rc = ble_gatts_notify_custom(conn, s_tx_val_handle, om);
        if (rc != 0) {
            // 多半是刚断开
            ESP_LOGW(TAG, "发 0x%02X 失败 rc=%d（连接刚断？）", type, rc);
            return ESP_FAIL;
        }

        off += n;
    } while (off < len);

    ESP_LOGD(TAG, "已发 0x%02X（%d 字节，MTU %d）", type, len, mtu);
    return ESP_OK;
}

// 五个上行共用一个口
static void ble_notify(uint8_t type, const char *json, int len) {
    if (!ble_can_notify()) {
        return;
    }
    (void)ble_send_frame(type, json, len);
}

bool ble_app_is_connected(void) {
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

void ble_app_notify_state(const char *json, int len) {
    ble_notify(BLE_UP_STATE, json, len);
}

void ble_app_notify_sensor(const char *json, int len) {
    ble_notify(BLE_UP_SENSOR, json, len);
}

void ble_app_notify_weather(const char *json, int len) {
    ble_notify(BLE_UP_WEATHER, json, len);
}

void ble_app_notify_config(const char *json, int len) {
    ble_notify(BLE_UP_CONFIG, json, len);
}

void ble_app_notify_ack(const char *json, int len) {
    ble_notify(BLE_UP_ACK, json, len);
}

// 自己拼 INFO
void ble_app_notify_info(void) {
    if (!ble_can_notify()) {
        return;
    }

    char        buf[BLE_JSON_INFO_LEN];
    const int   mtu  = ble_att_mtu(s_conn_handle);
    const char *base = net_link_topic_base();

    const int n = snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"ver\":\"%s\",\"base\":\"%s\",\"ble_mtu\":%d}",
                           s_dev_name, BLE_APP_VER, (base != NULL) ? base : "",
                           (mtu > 0) ? mtu : BLE_MTU_PREFERRED);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "INFO 装不下，跳过");
        return;
    }
    (void)ble_send_frame(BLE_UP_INFO, buf, n);
}

// 复位只记日志
static void ble_on_reset(int reason) {
    ESP_LOGE(TAG, "协议栈复位 reason=%d", reason);
}

// 同步好就开广播
static void ble_on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "本机没有可用的蓝牙地址");
        return;
    }

    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "地址类型判不出来");
        return;
    }

    {
        uint8_t addr[6] = {0};
        char    str[18] = {0};
        if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0) {
            ble_format_addr(addr, str);
            ESP_LOGI(TAG, "本机蓝牙地址 %s", str);
        }
    }

    ble_start_advertising();
}

// 常驻蓝牙任务
static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    vTaskDelete(NULL);
}

esp_err_t ble_app_start(void) {
    int rc;

    // 重复调直接返回
    if (s_started) {
        ESP_LOGI(TAG, "蓝牙已经起过了，跳过");
        return ESP_OK;
    }

    if (ble_build_device_name() != ESP_OK) {
        return ESP_FAIL;
    }

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败：%s", esp_err_to_name(err));
        return err;
    }

    // 基础服务先注册
    ble_svc_gap_init();
    rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc != 0) {
        ESP_LOGW(TAG, "GAP 名设不上 rc=%d", rc);
    }
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    // 挂服务和特征
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败 rc=%d", rc);
        return ESP_FAIL;
    }

    // 只挂自己的回调
    ble_hs_cfg.reset_cb        = ble_on_reset;
    ble_hs_cfg.sync_cb         = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 配对信息照例程存
    ble_store_config_init();

    // 想要大包
    rc = ble_att_set_preferred_mtu(BLE_MTU_PREFERRED);
    if (rc != 0) {
        ESP_LOGW(TAG, "MTU 想设 %d 没成 rc=%d", BLE_MTU_PREFERRED, rc);
    }

    // 起任务，同步完开广播
    nimble_port_freertos_init(ble_host_task);
    s_started = true;

    ESP_LOGI(TAG, "蓝牙已启动：名 \"%s\"，等手机连", s_dev_name);
    return ESP_OK;
}

#endif
