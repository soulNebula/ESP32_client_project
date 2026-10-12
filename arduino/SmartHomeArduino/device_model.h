// c
// 设备状态与硬件出口
#pragma once
#include <stdbool.h>
#include "app_common.h"

// 状态真源里的设备
typedef enum {
    // 客厅那路
    DEV_LED_LIVING = 0,
    // 厨房那路
    DEV_LED_KITCHEN,
    // 卧室那路
    DEV_LED_BEDROOM,
    // 浴室那路
    DEV_LED_BATH,
    // 门舵机
    DEV_DOOR,
    // 蜂鸣器
    DEV_BUZZER,
    DEV_MAX,
} device_id_t;

// 谁下的命令
typedef enum {
    SRC_VOICE = 0,
    SRC_APP,
    SRC_BUTTON,
    SRC_CONSOLE,
    // 自动联动不算手动
    SRC_AUTO,
} ctrl_source_t;

// 开关与亮度
typedef struct {
    bool power;
    // 0~100
    int level;
} device_state_t;

// 回调槽位数
#define HOME_CTRL_MAX_CB 6

// 回调里别阻塞
typedef void (*home_ctrl_cb_t)(device_id_t id, device_state_t st, ctrl_source_t src, void *user);

// 灯预置满亮度
app_err_t home_ctrl_init(void);

// 复位保留满亮度
void home_ctrl_reset_all(void);

// AUTO 要豁免
app_err_t home_ctrl_set_power(device_id_t id, bool on, ctrl_source_t src);

// level>0 即开
app_err_t home_ctrl_set_level(device_id_t id, int pct, ctrl_source_t src);

// 复用设置
app_err_t home_ctrl_toggle(device_id_t id, ctrl_source_t src);

// 逐盏设，别批量
int home_ctrl_set_all_lights(bool on, ctrl_source_t src);

// 非法给全零
device_state_t home_ctrl_get(device_id_t id);

// 非法返回关
bool home_ctrl_is_on(device_id_t id);

// 屏幕上报用
void home_ctrl_get_all(device_state_t *out, int count);

const char *home_ctrl_dev_name(device_id_t id);

const char *home_ctrl_dev_name_cn(device_id_t id);

// 不接受 all
device_id_t home_ctrl_dev_from_name(const char *name);

const char *home_ctrl_src_name(ctrl_source_t src);

// 先拷再打印
void home_ctrl_dump(void);

// NULL 即注销
app_err_t home_ctrl_register_cb(home_ctrl_cb_t cb, void *user);
