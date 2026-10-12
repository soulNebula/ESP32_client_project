// c
// 设备状态与硬件出口
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "device_model.h"
#include "automation.h"

#include "led.h"
#include "servo.h"
#include "buzzer.h"

static const char *TAG = "CTRL";

// 状态真源
static device_state_t s_dev[DEV_MAX];

static home_ctrl_cb_t s_cbs[HOME_CTRL_MAX_CB];
static void          *s_cb_user[HOME_CTRL_MAX_CB];

// init 前为空
static SemaphoreHandle_t s_lock;

static bool s_inited;

// 顺序同枚举
static const char *s_dev_name[DEV_MAX] = {
    [DEV_LED_LIVING]  = "led_living",
    [DEV_LED_KITCHEN] = "led_kitchen",
    [DEV_LED_BEDROOM] = "led_bedroom",
    [DEV_LED_BATH]    = "led_bath",
    [DEV_DOOR]        = "door",
    [DEV_BUZZER]      = "buzzer",
};

// 顺序同上
static const char *s_dev_name_cn[DEV_MAX] = {
    [DEV_LED_LIVING]  = "客厅灯",
    [DEV_LED_KITCHEN] = "厨房灯",
    [DEV_LED_BEDROOM] = "卧室灯",
    [DEV_LED_BATH]    = "浴室灯",
    [DEV_DOOR]        = "门",
    [DEV_BUZZER]      = "蜂鸣器",
};

// 顺序同枚举
static const char *s_src_name[] = {
    [SRC_VOICE] = "voice",     [SRC_APP] = "app",   [SRC_BUTTON] = "button",
    [SRC_CONSOLE] = "console", [SRC_AUTO] = "auto",
};

// 没锁就跳过
static void ctrl_lock(void) {
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

// 与加锁成对
static void ctrl_unlock(void) {
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

// 编号合法吗
static bool id_valid(device_id_t id) {
    return ((int)id >= 0) && (id < DEV_MAX);
}

// 是不是 4 路灯
static bool is_led(device_id_t id) {
    return ((int)id >= (int)DEV_LED_LIVING) && ((int)id <= (int)DEV_LED_BATH);
}

// switch 防漏
static bsp_led_zone_t led_zone_of(device_id_t id) {
    switch (id) {
    case DEV_LED_LIVING:
        return BSP_LED_LIVING;
    case DEV_LED_KITCHEN:
        return BSP_LED_KITCHEN;
    case DEV_LED_BEDROOM:
        return BSP_LED_BEDROOM;
    case DEV_LED_BATH:
        return BSP_LED_BATH;
    default:
        return BSP_LED_MAX;
    }
}

// 唯一硬件出口
static esp_err_t hw_set_power(device_id_t id, bool on) {
    if (is_led(id)) {
        return bsp_led_set_power(led_zone_of(id), on);
    }
    if (id == DEV_DOOR) {
        // 角度由板级定
        return bsp_servo_door_set(on);
    }
    if (id == DEV_BUZZER) {
        return bsp_buzzer_set(on);
    }
    return ESP_ERR_INVALID_ARG;
}

// 唯一硬件出口
static esp_err_t hw_set_level(device_id_t id, int pct) {
    if (is_led(id)) {
        return bsp_led_set_level(led_zone_of(id), pct);
    }
    if (id == DEV_DOOR) {
        return bsp_servo_door_set_pct(pct);
    }
    if (id == DEV_BUZZER) {
        // 无亮度只有响停
        return bsp_buzzer_set(pct > 0);
    }
    return ESP_ERR_INVALID_ARG;
}

// 回调里别阻塞
static void notify_change(device_id_t id, device_state_t st, ctrl_source_t src) {
    home_ctrl_cb_t cbs[HOME_CTRL_MAX_CB];
    void          *users[HOME_CTRL_MAX_CB];

    ctrl_lock();
    memcpy(cbs, s_cbs, sizeof(cbs));
    memcpy(users, s_cb_user, sizeof(users));
    ctrl_unlock();

    for (int i = 0; i < HOME_CTRL_MAX_CB; i++) {
        if (cbs[i] != NULL) {
            cbs[i](id, st, src, users[i]);
        }
    }
}

// 开门点亮客厅灯
static void door_cascade_on_rising(bool was_on, bool now_on) {
    if (!now_on || was_on) {
        return;
    }

    const auto_cfg_t *cfg = auto_policy_cfg();
    if (cfg == NULL) {
        // 配置未知别开灯
        ESP_LOGW(TAG, "开门级联跳过：配置不可用");
        return;
    }
    if (!cfg->door_light_cascade) {
        ESP_LOGD(TAG, "开门级联被配置关闭（door_light_cascade=false）");
        return;
    }

    ESP_LOGI(TAG, "开门级联：点亮客厅灯");

    // 先续期再点灯
    auto_policy_notify_door_lit();

    (void)home_ctrl_set_power(DEV_LED_LIVING, true, SRC_AUTO);
}

// 灯预置满亮度
esp_err_t home_ctrl_init(void) {
    if (s_inited) {
        // 幂等不重置
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "创建互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    memset(s_dev, 0, sizeof(s_dev));
    memset(s_cbs, 0, sizeof(s_cbs));
    memset(s_cb_user, 0, sizeof(s_cb_user));

    for (int i = DEV_LED_LIVING; i <= DEV_LED_BATH; i++) {
        s_dev[i].level = 100;
    }
    s_dev[DEV_DOOR].level = 0;
    s_dev[DEV_BUZZER].level = 0;

    s_inited = true;

    ESP_LOGI(TAG, "init ok: %d 个设备（4 灯 + 门 + 蜂鸣器），默认全关", DEV_MAX);
    return ESP_OK;
}

// 复位保留满亮度
// 报警优先于复位
void home_ctrl_reset_all(void) {
    for (int i = 0; i < DEV_MAX; i++) {
        const device_id_t id = (device_id_t)i;

        const esp_err_t err = hw_set_power(id, false);
        if (err != ESP_OK) {
            // 硬件失败不改状态
            ESP_LOGW(TAG, "reset %s 硬件未落地: %s（软件状态保持不变）", home_ctrl_dev_name(id),
                     esp_err_to_name(err));
            continue;
        }

        device_state_t st;
        ctrl_lock();
        s_dev[id].power = false;
        s_dev[id].level = is_led(id) ? 100 : 0;
        st              = s_dev[id];
        ctrl_unlock();

        notify_change(id, st, SRC_AUTO);
    }

    ESP_LOGI(TAG, "reset all: 4 灯 + 门 + 蜂鸣器 全部回到安全初始态");
}

// AUTO 要豁免
esp_err_t home_ctrl_set_power(device_id_t id, bool on, ctrl_source_t src) {
    if (!id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (is_led(id) && src != SRC_AUTO && auto_policy_is_auto()) {
        ESP_LOGD(TAG, "拒绝 %s -> %s：自动模式下灯光由光照控制", home_ctrl_dev_name(id), on ? "ON" : "OFF");
        return ESP_ERR_INVALID_STATE;
    }

    bool was_on;
    ctrl_lock();
    was_on = s_dev[id].power;
    ctrl_unlock();

    const esp_err_t err = hw_set_power(id, on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "硬件失败: %s -> %s (%s)，软件状态保持 %s", home_ctrl_dev_name(id), on ? "ON" : "OFF",
                 esp_err_to_name(err), was_on ? "ON" : "OFF");
        return ESP_FAIL;
    }

    device_state_t st;
    ctrl_lock();
    s_dev[id].power = on;
    // 开关与亮度同步
    s_dev[id].level = on ? 100 : 0;
    st              = s_dev[id];
    ctrl_unlock();

    ESP_LOGI(TAG, "device: %s -> %s (by %s)", home_ctrl_dev_name(id), on ? "ON" : "OFF",
             home_ctrl_src_name(src));

    // 先通知后级联
    notify_change(id, st, src);

    if (id == DEV_DOOR) {
        door_cascade_on_rising(was_on, on);
    }

    return ESP_OK;
}

// level>0 即开
esp_err_t home_ctrl_set_level(device_id_t id, int pct, ctrl_source_t src) {
    if (!id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    // 拒绝规则同上
    if (is_led(id) && src != SRC_AUTO && auto_policy_is_auto()) {
        ESP_LOGD(TAG, "拒绝 %s level=%d：自动模式下灯光由光照控制", home_ctrl_dev_name(id), pct);
        return ESP_ERR_INVALID_STATE;
    }

    const bool target_on = (pct > 0);
    // 蜂鸣器只存两态
    const int stored_level = (id == DEV_BUZZER) ? (target_on ? 100 : 0) : pct;

    bool was_on;
    ctrl_lock();
    was_on = s_dev[id].power;
    ctrl_unlock();

    const esp_err_t err = hw_set_level(id, pct);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "硬件失败: %s level=%d (%s)，软件状态保持不变", home_ctrl_dev_name(id), pct,
                 esp_err_to_name(err));
        return ESP_FAIL;
    }

    device_state_t st;
    ctrl_lock();
    s_dev[id].power = target_on;
    s_dev[id].level = stored_level;
    st              = s_dev[id];
    ctrl_unlock();

    ESP_LOGI(TAG, "device: %s level=%d%% (power=%s, by %s)", home_ctrl_dev_name(id), pct,
             target_on ? "ON" : "OFF", home_ctrl_src_name(src));

    notify_change(id, st, src);

    if (id == DEV_DOOR) {
        door_cascade_on_rising(was_on, target_on);
    }

    return ESP_OK;
}

// 复用设置
esp_err_t home_ctrl_toggle(device_id_t id, ctrl_source_t src) {
    if (!id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    return home_ctrl_set_power(id, !home_ctrl_is_on(id), src);
}

// 逐盏设，别批量
int home_ctrl_set_all_lights(bool on, ctrl_source_t src) {
    int ok_cnt = 0;

    for (int i = DEV_LED_LIVING; i <= DEV_LED_BATH; i++) {
        if (home_ctrl_set_power((device_id_t)i, on, src) == ESP_OK) {
            ok_cnt++;
        }
    }

    ESP_LOGI(TAG, "all lights -> %s (by %s): %d/4 成功", on ? "ON" : "OFF", home_ctrl_src_name(src), ok_cnt);
    return ok_cnt;
}

// 非法给全零
device_state_t home_ctrl_get(device_id_t id) {
    device_state_t st = {false, 0};

    if (!id_valid(id)) {
        return st;
    }

    ctrl_lock();
    st = s_dev[id];
    ctrl_unlock();
    return st;
}

// 非法返回关
bool home_ctrl_is_on(device_id_t id) {
    return home_ctrl_get(id).power;
}

// 屏幕上报用
void home_ctrl_get_all(device_state_t *out, int count) {
    if (out == NULL || count <= 0) {
        return;
    }
    if (count > DEV_MAX) {
        count = DEV_MAX;
    }

    ctrl_lock();
    memcpy(out, s_dev, sizeof(device_state_t) * (size_t)count);
    ctrl_unlock();
}

const char *home_ctrl_dev_name(device_id_t id) {
    return id_valid(id) ? s_dev_name[id] : "?";
}

const char *home_ctrl_dev_name_cn(device_id_t id) {
    return id_valid(id) ? s_dev_name_cn[id] : "?";
}

// 不接受 all
device_id_t home_ctrl_dev_from_name(const char *name) {
    if (name == NULL) {
        return DEV_MAX;
    }

    for (int i = 0; i < DEV_MAX; i++) {
        if (strcmp(name, s_dev_name[i]) == 0) {
            return (device_id_t)i;
        }
    }
    return DEV_MAX;
}

const char *home_ctrl_src_name(ctrl_source_t src) {
    if ((int)src < 0 || (int)src >= (int)(sizeof(s_src_name) / sizeof(s_src_name[0]))) {
        return "?";
    }
    return s_src_name[src];
}

// 先拷再打印
void home_ctrl_dump(void) {
    device_state_t snap[DEV_MAX];

    home_ctrl_get_all(snap, DEV_MAX);

    ESP_LOGI(TAG, "---- 设备状态（唯一真源）----");
    int led_on = 0;
    for (int i = 0; i < DEV_MAX; i++) {
        if (is_led((device_id_t)i) && snap[i].power) {
            led_on++;
        }
        ESP_LOGI(TAG, "  %-11s %-9s power=%-3s level=%3d%%", home_ctrl_dev_name((device_id_t)i),
                 home_ctrl_dev_name_cn((device_id_t)i), snap[i].power ? "ON" : "OFF", snap[i].level);
    }
    ESP_LOGI(TAG, "汇总: 灯 %d/%d 亮, 门=%s, 蜂鸣器=%s", led_on, (int)(DEV_LED_BATH - DEV_LED_LIVING + 1),
             snap[DEV_DOOR].power ? "开" : "关", snap[DEV_BUZZER].power ? "响" : "停");
}

// NULL 即注销
esp_err_t home_ctrl_register_cb(home_ctrl_cb_t cb, void *user) {
    esp_err_t ret = ESP_ERR_NO_MEM;

    ctrl_lock();

    if (cb == NULL) {
        ret = ESP_ERR_INVALID_ARG;
        for (int i = 0; i < HOME_CTRL_MAX_CB; i++) {
            if (s_cbs[i] != NULL && s_cb_user[i] == user) {
                s_cbs[i]     = NULL;
                s_cb_user[i] = NULL;
                ret          = ESP_OK;
                break;
            }
        }
    } else {
        for (int i = 0; i < HOME_CTRL_MAX_CB; i++) {
            if (s_cbs[i] == NULL) {
                s_cbs[i]     = cb;
                s_cb_user[i] = user;
                ret          = ESP_OK;
                break;
            }
        }
    }

    ctrl_unlock();

    if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "回调槽位已满（最多 %d 个），注册失败", HOME_CTRL_MAX_CB);
    }
    return ret;
}
