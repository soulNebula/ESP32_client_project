// c
// 语音指令表
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 指令表
typedef enum {
    VOICE_CMD_NONE = 0,

    VOICE_CMD_LED_LIVING_ON,
    VOICE_CMD_LED_LIVING_OFF,
    VOICE_CMD_LED_KITCHEN_ON,
    VOICE_CMD_LED_KITCHEN_OFF,
    VOICE_CMD_LED_BEDROOM_ON,
    VOICE_CMD_LED_BEDROOM_OFF,
    VOICE_CMD_LED_BATH_ON,
    VOICE_CMD_LED_BATH_OFF,

    // 不顺手开门
    VOICE_CMD_LED_ALL_ON,
    VOICE_CMD_LED_ALL_OFF,

    // 联动客厅灯
    VOICE_CMD_DOOR_OPEN,
    VOICE_CMD_DOOR_CLOSE,

    VOICE_CMD_MODE_AUTO,
    VOICE_CMD_MODE_MANUAL,

    // 静音锁
    VOICE_CMD_BUZZER_OFF,
    VOICE_CMD_BUZZER_ON,

    VOICE_CMD_QUERY_TEMP,
    VOICE_CMD_QUERY_HUMI,
    VOICE_CMD_QUERY_ALL,
    VOICE_CMD_QUERY_WEATHER,
    VOICE_CMD_QUERY_LIGHT,
    // 温湿度+天气
    VOICE_CMD_QUERY_STATUS,

    VOICE_CMD_MAX,
} voice_cmd_t;

// 上报事件
typedef enum {
    // 只上报
    VOICE_EVENT_WAKE = 0,
    VOICE_EVENT_CMD,
    // 预留，调试用
    VOICE_EVENT_RAW,
} voice_event_t;

// 回调里别阻塞
typedef void (*bsp_voice_cb_t)(voice_event_t ev, voice_cmd_t cmd, const char *raw, void *user);

// 没麦也算成功
esp_err_t bsp_voice_init(void);

// 只有一个回调
esp_err_t bsp_voice_register_cb(bsp_voice_cb_t cb, void *user);

// 走真实语音同链路
esp_err_t bsp_voice_inject(const char *token);

voice_cmd_t bsp_voice_token_to_cmd(const char *token);

const char *bsp_voice_cmd_name(voice_cmd_t cmd);

// 只有这一张表
const char *bsp_voice_cmd_cn(voice_cmd_t cmd);

// 没喇叭只打日志
esp_err_t bsp_voice_speak(const char *text);

esp_err_t bsp_voice_speak_temp(float temp_c, bool valid);

esp_err_t bsp_voice_speak_humi(float humi, bool valid);

// 合成一条
esp_err_t bsp_voice_speak_temp_humi(float temp_c, float humi, bool valid);

esp_err_t bsp_voice_speak_weather(bool raining, bool valid);

esp_err_t bsp_voice_speak_light(float light_pct, bool valid);

esp_err_t bsp_voice_speak_mode(bool auto_mode);

// 调试台用
void bsp_voice_dump_cmds(void);

// 没麦也算就绪
bool bsp_voice_ready(void);

// 没启动返回占位
const char *bsp_voice_wake_word(void);

// 全角标点变方块
const char *bsp_voice_last_spoken(void);

// 恒 0 查接线
bool bsp_voice_mic_level(int *rms, int *peak, int *dbfs_x10);

// 判断 L/R 接反
bool bsp_voice_mic_data_in_other_slot(void);

bool bsp_voice_is_awake(void);

#ifdef __cplusplus
}
#endif
