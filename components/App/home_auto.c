// c
// 光照与高温联动

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "home_auto.h"
#include "automation.h"
#include "device_model.h"
#include "sensor.h"
#include "buzzer.h"

static const char *TAG = "AUTO";

// 手动保护期
#define AUTO_MANUAL_GUARD_MS 3000

// 开门免打扰
#define CASCADE_GUARD_MS 30000

// 光照节流 1s
#define RULE_LIGHT_THROTTLE_MS 1000

// 锁里只碰内存
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// 保护期截止
static int64_t s_manual_guard_until_ms;
// init 是否跑过
static bool s_inited;

// 光照节流时间戳
static int64_t s_last_light_ms;

// 上次雨滴阈值
static float s_rain_synced_pct = -1.0f;

// 记手动改动时刻
static void on_ctrl_changed(device_id_t id, device_state_t st, ctrl_source_t src, void *user) {
    (void)st;
    (void)user;

    // 只管客厅灯
    if (id != DEV_LED_LIVING) {
        return;
    }
    // 自动源放行
    if (src == SRC_AUTO) {
        return;
    }

#if AUTO_MANUAL_GUARD_MS > 0
    // 锁外算好时刻
    const int64_t until = (esp_timer_get_time() / 1000) + AUTO_MANUAL_GUARD_MS;

    portENTER_CRITICAL(&s_mux);
    s_manual_guard_until_ms = until;
    portEXIT_CRITICAL(&s_mux);
#else
    (void)id;
#endif
}

// 手动保护期内吗
static bool manual_guard_active(int64_t now_ms) {
#if AUTO_MANUAL_GUARD_MS > 0
    portENTER_CRITICAL(&s_mux);
    const int64_t until = s_manual_guard_until_ms;
    portEXIT_CRITICAL(&s_mux);

    return now_ms < until;
#else
    (void)now_ms;
    return false;
#endif
}

// 续免打扰保护期
void home_auto_protect_living_light(int ms) {
    // 没给用默认
    if (ms <= 0) {
        ms = CASCADE_GUARD_MS;
    }

    const int64_t until = (esp_timer_get_time() / 1000) + ms;

    portENTER_CRITICAL(&s_mux);
    // 只延长不缩短
    if (until > s_manual_guard_until_ms) {
        s_manual_guard_until_ms = until;
    }
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "客厅灯免打扰保护期 +%dms（开门级联安全照明，需求⑤）", ms);
}

// 开门亮灯回调
static void on_door_lit(void *user) {
    (void)user;
    // 0 用默认
    home_auto_protect_living_light(0);
}

// 光照联动客厅灯
static void rule_light(const sensor_snapshot_t *snap, int64_t now_ms) {
    // 手动模式短路
    if (!auto_policy_is_auto()) {
        return;
    }

    const auto_cfg_t *cfg = auto_policy_cfg();
    if (cfg == NULL || !cfg->auto_light_enable) {
        return;
    }

    const float on_pct  = cfg->light_on_pct;
    const float off_pct = cfg->light_off_pct;

    // 1 秒节流
    if ((now_ms - s_last_light_ms) < RULE_LIGHT_THROTTLE_MS) {
        return;
    }
    s_last_light_ms = now_ms;

    // 读数无效退出
    if (!snap->light_valid) {
        return;
    }

    // 第 2 层防护
    if (manual_guard_active(now_ms)) {
        return;
    }

    // 双阈值迟滞
    const float pct = snap->light_pct;
    const bool  on  = home_ctrl_is_on(DEV_LED_LIVING);

    if (!on && pct < on_pct) {
        ESP_LOGI(TAG, "[规则1] 光照 %.1f%% < %.1f%% → 开客厅灯", pct, on_pct);
        (void)home_ctrl_set_power(DEV_LED_LIVING, true, SRC_AUTO);
    } else if (on && pct > off_pct) {
        ESP_LOGI(TAG, "[规则1] 光照 %.1f%% > %.1f%% → 关客厅灯", pct, off_pct);
        (void)home_ctrl_set_power(DEV_LED_LIVING, false, SRC_AUTO);
    }
}

// 变化才写硬件
static void buzzer_apply(bool on, const char *reason) {
    if (home_ctrl_is_on(DEV_BUZZER) == on) {
        return;
    }

    ESP_LOGI(TAG, "%s → 蜂鸣器%s", reason, on ? "响" : "停");

    const esp_err_t err = home_ctrl_set_power(DEV_BUZZER, on, SRC_AUTO);
    if (err != ESP_OK) {
        // 失败不用回滚
        ESP_LOGW(TAG, "蜂鸣器%s失败: %s", on ? "开" : "关", esp_err_to_name(err));
    }
}

// 高温报警蜂鸣器
static void rule_buzzer(const sensor_snapshot_t *snap) {
    const auto_cfg_t *cfg = auto_policy_cfg();
    if (cfg == NULL || !cfg->auto_buzzer_enable) {
        // 停用不等于静音
        return;
    }

    const float on_c  = cfg->buzzer_on_c;
    const float off_c = cfg->buzzer_off_c;

    // 不在线强制停
    if (!snap->th_valid) {
        buzzer_apply(false, "[规则2] 温度传感器不在线，安全起见强制停");
        return;
    }

    bool muted = auto_policy_buzzer_muted();

    if (muted && snap->temperature <= off_c) {
        // 降温解除静音
        ESP_LOGI(TAG, "[规则2] 温度 %.1f℃ ≤ %.1f℃，自动解除静音锁", snap->temperature, off_c);
        auto_policy_clear_mute();
        muted = false;
    }

    // 迟滞 + 静音锁
    if (muted) {
        buzzer_apply(false, "[规则2] 静音锁生效中，保持静音");
    } else if (snap->temperature >= on_c) {
        buzzer_apply(true, "[规则2] 温度达到报警上沿，蜂鸣器响");
    } else if (snap->temperature <= off_c) {
        buzzer_apply(false, "[规则2] 温度降到报警下沿，蜂鸣器停");
    }
    // 迟滞区
}

// 同步雨滴阈值
static void sync_rain_threshold(void) {
    const auto_cfg_t *cfg = auto_policy_cfg();
    if (cfg == NULL) {
        return;
    }
    if (cfg->rain_pct != s_rain_synced_pct) {
        bsp_sensors_set_rain_threshold(cfg->rain_pct);
        s_rain_synced_pct = cfg->rain_pct;
        ESP_LOGI(TAG, "雨滴判定阈值同步给传感器: %.1f%%（需求③ 只影响显示与播报）", cfg->rain_pct);
    }
}

// 初始化规则引擎
esp_err_t home_auto_init(void) {
    if (s_inited) {
        // 幂等
        return ESP_OK;
    }

    // 1. 读配置
    (void)auto_policy_init();

    // 2. 同步雨滴阈值
    sync_rain_threshold();

    // 2b. 蜂鸣器极性
    {
        const auto_cfg_t *c2 = auto_policy_cfg();
        if (c2 != NULL) {
            const esp_err_t perr = bsp_buzzer_set_polarity_low(c2->buzzer_active_low);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "应用蜂鸣器极性失败(%s)，沿用编译期默认值", esp_err_to_name(perr));
            }
        }
    }

    // 3. 注册状态回调
    const esp_err_t cb_err = home_ctrl_register_cb(on_ctrl_changed, NULL);
    if (cb_err != ESP_OK) {
        ESP_LOGW(TAG, "注册状态回调失败(%s)：自动模式下的手动保护期失效，其余功能正常",
                 esp_err_to_name(cb_err));
    }

    // 3b. 注册门灯回调
    const esp_err_t hook_err = auto_policy_set_door_light_hook(on_door_lit, NULL);
    if (hook_err != ESP_OK) {
        ESP_LOGW(TAG, "注册门灯联动回调失败(%s)：开门亮灯会被光控很快覆盖", esp_err_to_name(hook_err));
    }

    s_inited = true;

    // 4. 汇总日志
    const auto_cfg_t *c = auto_policy_cfg();
    if (c != NULL) {
        ESP_LOGI(TAG, "init ok: 模式=%s, 光控(%.0f/%.0f%%, %s), 蜂鸣器(%.0f/%.0fC, %s), 开门亮灯=%s",
                 (c->mode == AUTO_MODE_AUTO) ? "自动" : "手动", c->light_on_pct, c->light_off_pct,
                 c->auto_light_enable ? "启用" : "停用", c->buzzer_on_c, c->buzzer_off_c,
                 c->auto_buzzer_enable ? "启用" : "停用", c->door_light_cascade ? "开" : "关");
    }
    // 永远返回 OK
    return ESP_OK;
}

// 主循环心跳
void home_auto_tick(const sensor_snapshot_t *snap) {
    // 不许阻塞
    if (snap == NULL) {
        return;
    }
    if (!s_inited) {
        return;
    }

    // 雨滴只同步阈值
    sync_rain_threshold();

    const int64_t now_ms = esp_timer_get_time() / 1000;

    // 规则1 光照
    rule_light(snap, now_ms);
    // 规则2 温度
    rule_buzzer(snap);
}
