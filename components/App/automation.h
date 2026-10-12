// c
// 模式与阈值接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 手动还是自动
typedef enum {
    AUTO_MODE_MANUAL = 0,
    AUTO_MODE_AUTO = 1,
} auto_mode_t;

// 改结构体要 +1
#define AUTO_CFG_VERSION 3

#define AUTO_NVS_NAMESPACE "sh_auto"
#define AUTO_NVS_KEY_CFG   "cfg"

// 出厂默认
// 暗过这条开灯
#define AUTO_DEF_LIGHT_ON_PCT 20.0f
// 亮过这条关灯
#define AUTO_DEF_LIGHT_OFF_PCT 45.0f
// 温度≥32.5℃ 响（用户指定）
#define AUTO_DEF_BUZZER_ON_C 32.5f
// ≤32.0℃ 停（保留 0.5℃ 死区，别等于上面那条）
// ⚠ 32.0 与屏幕闪烁阈值 temp_alert_c 同值，但两者是独立配置，别改混
#define AUTO_DEF_BUZZER_OFF_C 32.0f
// ≥32℃ 闪数字
#define AUTO_DEF_TEMP_ALERT_C 32.0f

// 值等于旧默认才升
// 旧默认 35℃ 响
#define AUTO_OLD_BUZZER_ON_C 35.0f
// 旧默认 33℃ 停
#define AUTO_OLD_BUZZER_OFF_C 33.0f
// >30 算下雨
#define AUTO_DEF_RAIN_PCT 30.0f

// 模式 + 阈值配置
typedef struct {
    auto_mode_t mode;

    bool auto_light_enable;
    float light_on_pct;
    // 必须比 on 大
    float light_off_pct;

    bool auto_buzzer_enable;
    float buzzer_on_c;
    // 必须比 on 小
    float buzzer_off_c;

    // ≥这条闪数字
    float temp_alert_c;

    // 过这条算下雨
    float rain_pct;

    // 开门顺手点灯
    bool door_light_cascade;

    // 拉低才响，可切
    bool buzzer_active_low;
} auto_cfg_t;

esp_err_t auto_policy_init(void);

// 顺手落 NVS
esp_err_t auto_policy_set_mode(auto_mode_t mode);

auto_mode_t auto_policy_get_mode(void);

bool auto_policy_is_auto(void);

const char *auto_policy_mode_name(auto_mode_t mode);

const char *auto_policy_mode_name_cn(auto_mode_t mode);

// 只读别强转
const auto_cfg_t *auto_policy_cfg(void);

esp_err_t auto_policy_reset_defaults(void);

// 上调先发 on
esp_err_t auto_policy_set_threshold(const char *key, const char *val);

void auto_policy_dump(void);

// 防截断，不发半截
int auto_policy_cfg_to_json(char *buf, int len);

esp_err_t auto_policy_save(void);

// 是锁不是一次动作
void auto_policy_mute_buzzer(void);

void auto_policy_unmute_buzzer(void);

bool auto_policy_buzzer_muted(void);

void auto_policy_clear_mute(void);

// 开门级联点灯回调
typedef void (*auto_policy_door_light_fn)(void *user);

// NULL 即注销
esp_err_t auto_policy_set_door_light_hook(auto_policy_door_light_fn cb, void *user);

// 非阻塞，没注册就空跑
void auto_policy_notify_door_lit(void);

#ifdef __cplusplus
}
#endif

// 策略表到此结束
