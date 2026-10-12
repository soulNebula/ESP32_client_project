// c
// 模式与阈值存储
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_common.h"

// 手动/自动
typedef enum {
    AUTO_MODE_MANUAL = 0,
    AUTO_MODE_AUTO,
} auto_mode_t;

// 一个结构体全装下
// 改结构体要 +1
typedef struct {
    auto_mode_t mode;

    bool auto_light_enable;
    // 低于它开客厅灯
    float light_on_pct;
    // 高于它关客厅灯
    float light_off_pct;

    bool auto_buzzer_enable;
    // 到温响蜂鸣器
    float buzzer_on_c;
    // 降到它才停
    float buzzer_off_c;

    float temp_alert_c;

    // 雨滴判定
    float rain_pct;

    // 开门顺手亮客厅灯
    bool door_light_cascade;

    // 现场反了就改这个
    bool buzzer_active_low;
} auto_cfg_t;

// 存储整包
#define AUTO_CFG_VERSION 1

#define AUTO_NVS_NAMESPACE "sh_auto"
#define AUTO_NVS_KEY_CFG   "cfg"

// 旧默认 35/33
#define AUTO_OLD_BUZZER_ON_C  35.0f
#define AUTO_OLD_BUZZER_OFF_C 33.0f

// 编译期默认值
#define AUTO_DEF_LIGHT_ON_PCT  20.0f
#define AUTO_DEF_LIGHT_OFF_PCT 45.0f
#define AUTO_DEF_BUZZER_ON_C   32.5f
#define AUTO_DEF_BUZZER_OFF_C  32.0f
#define AUTO_DEF_TEMP_ALERT_C  32.0f
#define AUTO_DEF_RAIN_PCT      30.0f

// 铺默认再读配置
app_err_t auto_policy_init(void);

app_err_t auto_policy_save(void);

// 切模式并落盘
app_err_t auto_policy_set_mode(auto_mode_t mode);

auto_mode_t auto_policy_get_mode(void);

// 现在是否自动
bool auto_policy_is_auto(void);

const char *auto_policy_mode_name(auto_mode_t mode);

const char *auto_policy_mode_name_cn(auto_mode_t mode);

const auto_cfg_t *auto_policy_cfg(void);

// 恢复后落盘
app_err_t auto_policy_reset_defaults(void);

// 阈值键值读写
app_err_t auto_policy_set_threshold(const char *key, const char *val);

void auto_policy_dump(void);

int auto_policy_cfg_to_json(char *buf, int len);

void auto_policy_mute_buzzer(void);

void auto_policy_unmute_buzzer(void);

bool auto_policy_buzzer_muted(void);

// 降温自动解锁
void auto_policy_clear_mute(void);

// 门灯联动回调
typedef void (*auto_policy_door_light_fn)(void *user);

// NULL 表示注销
app_err_t auto_policy_set_door_light_hook(auto_policy_door_light_fn cb, void *user);

void auto_policy_notify_door_lit(void);
