// c
// 屏显视图模型
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 四路灯下标
typedef enum {
    // 客厅那路
    VIEW_LIGHT_LIVING = 0,
    // 厨房那路
    VIEW_LIGHT_KITCHEN,
    // 卧室那路
    VIEW_LIGHT_BEDROOM,
    // 浴室那路
    VIEW_LIGHT_BATH,
    VIEW_LIGHT_COUNT,
} view_light_t;

// 语音条亮 3 秒
#define DISPLAY_VIEW_VOICE_MS 3000

// 传给屏的一帧数据
// 无效显示 --
typedef struct {
    float temperature;
    // 相对湿度 %
    float humidity;
    bool th_valid;

    bool raining;
    // 雨滴湿度百分比
    float rain_pct;
    bool rain_valid;

    // 0~100
    float light_pct;
    bool light_valid;
    // 真实 lux
    bool  light_is_lux;
    float light_lux;

    // 自动为 true
    bool auto_mode;

    bool light_on[VIEW_LIGHT_COUNT];
    bool door_open;

    bool buzzer_on;
    bool buzzer_muted;

    bool wifi_ok;
    bool mqtt_ok;
    // 本机 IP
    char ip[16];
    // 信号 dBm
    int8_t rssi;

    // 低于它开客厅灯
    float light_on_pct;
    // 高于它关客厅灯
    float light_off_pct;
    // 到温响蜂鸣器
    float buzzer_on_c;

    float temp_alert_c;

    // tick 变了弹条
    // 识别到的指令
    char voice_line1[24];
    // 动作结果或回答
    char voice_line2[28];
    // 每次语音事件 +1
    uint32_t voice_tick;
} display_view_input_t;

// 起屏并开刷屏任务
esp_err_t display_view_init(void);

esp_err_t display_view_init_no_task(void);

esp_err_t display_view_start(void);

// 提交一帧数据
void display_view_update(const display_view_input_t *in);

void display_view_get_input(display_view_input_t *out);

// 立刻重画一帧
void display_view_force_redraw(void);

void display_view_set_thresholds(float light_on_pct, float light_off_pct, float buzzer_on_c);

// 屏掉了重来
esp_err_t display_view_retry_panel(void);

bool display_view_ready(void);

#ifdef __cplusplus
}
#endif
