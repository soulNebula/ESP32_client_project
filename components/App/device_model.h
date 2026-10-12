// c
// 设备状态接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEV_LED_LIVING = 0,
    DEV_LED_KITCHEN,
    DEV_LED_BEDROOM,
    DEV_LED_BATH,
    DEV_DOOR,
    DEV_BUZZER,
    DEV_MAX,
} device_id_t;

typedef enum {
    SRC_VOICE = 0,
    // 手机 / MQTT
    SRC_APP,
    // 板载按键
    SRC_BUTTON,
    SRC_CONSOLE,
    // 光照 / 开门联动
    SRC_AUTO,
} ctrl_source_t;

typedef struct {
    // 灯亮灭 / 门开关
    bool power;
    // 亮度 0~100
    int level;
} device_state_t;

// 初始全部关闭
esp_err_t home_ctrl_init(void);

void home_ctrl_reset_all(void);

// 自动模式拒灯控
// 写失败不改软件态
// 开门亮灯是安全照明
esp_err_t home_ctrl_set_power(device_id_t id, bool on, ctrl_source_t src);

// 灯是亮度门是开合
// 大于 0 算开着
esp_err_t home_ctrl_set_level(device_id_t id, int pct, ctrl_source_t src);

esp_err_t home_ctrl_toggle(device_id_t id, ctrl_source_t src);

// 只开四路灯
int home_ctrl_set_all_lights(bool on, ctrl_source_t src);

device_state_t home_ctrl_get(device_id_t id);

bool home_ctrl_is_on(device_id_t id);

void home_ctrl_get_all(device_state_t *out, int count);

const char *home_ctrl_dev_name(device_id_t id);

const char *home_ctrl_dev_name_cn(device_id_t id);

device_id_t home_ctrl_dev_from_name(const char *name);

const char *home_ctrl_src_name(ctrl_source_t src);

void home_ctrl_dump(void);

// 状态变了回调
// 回调里别阻塞
typedef void (*home_ctrl_cb_t)(device_id_t id, device_state_t st, ctrl_source_t src, void *user);

#define HOME_CTRL_MAX_CB 4

// 槽满给 NO_MEM
esp_err_t home_ctrl_register_cb(home_ctrl_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
