// c
// 指令分发与注入

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "voice.h"
#include "voice_internal.h"
#include "i2s_mic.h"
#include "voice_esp_sr.h"

static const char *TAG = "voice";

// 误触发就调大
#define VOICE_RATE_LIMIT_MS 300

#define VOICE_SPEAK_MAX 96

typedef struct {
    // 规范短名
    const char *name;
    // 别名 \n 分隔
    const char *aliases;
    // 中文说法
    const char *cn;
} voice_cmd_entry_t;

// 指令对照表
static const voice_cmd_entry_t s_cmd_table[] = {
    // 0
    {           "none",     "nop\nnoop", "（无指令 / 空）"},
    // 1
    {  "led_living_on",     "living_on",       "打开客厅灯"},
    // 2
    { "led_living_off",    "living_off",       "关闭客厅灯"},
    // 3
    { "led_kitchen_on",    "kitchen_on",       "打开厨房灯"},
    // 4
    {"led_kitchen_off",   "kitchen_off",       "关闭厨房灯"},
    // 5
    { "led_bedroom_on",    "bedroom_on",       "打开卧室灯"},
    // 6
    {"led_bedroom_off",   "bedroom_off",       "关闭卧室灯"},
    // 7
    {    "led_bath_on",       "bath_on",       "打开浴室灯"},
    // 8
    {   "led_bath_off",      "bath_off",       "关闭浴室灯"},
    // 9
    {     "led_all_on",        "all_on",       "打开全部灯"},
    // 10
    {    "led_all_off",       "all_off",       "关闭全部灯"},
    // 11
    {      "door_open",     "open_door",             "打开门"},
    // 12
    {     "door_close",    "close_door",             "关上门"},
    // 13
    {      "mode_auto",     "auto_mode",          "自动模式"},
    // 14
    {    "mode_manual",   "manual_mode",          "手动模式"},
    // 15
    {     "buzzer_off",   "buzzer_mute",       "关闭蜂鸣器"},
    // 16
    {      "buzzer_on", "buzzer_unmute",       "打开蜂鸣器"},
    // 17
    {     "query_temp",    "temp_query",          "温度多少"},
    // 18
    {     "query_humi",    "humi_query",          "湿度多少"},
    // 19
    {      "query_all",     "all_query",       "温湿度多少"},
    // 20
    {  "query_weather",       "weather",       "天气怎么样"},
    // 21
    {    "query_light",   "light_query",          "光照多少"},
    // 22
    {   "query_status",        "status",          "状态如何"},
};

// 错位会执行错动作
_Static_assert(sizeof(s_cmd_table) / sizeof(s_cmd_table[0]) == (size_t)VOICE_CMD_MAX,
               "s_cmd_table[] 条目数必须等于 VOICE_CMD_MAX（与 voice.h 枚举一一对应）");
_Static_assert((int)VOICE_CMD_MAX - (int)VOICE_CMD_QUERY_STATUS == 1,
               "s_cmd_table[] 末项必须对应 VOICE_CMD_QUERY_STATUS，枚举顺序被改动过？");
_Static_assert(VOICE_SPEAK_MAX <= 256, "TTS 文本上限过大，无谓地放大栈占用");

// 回调只一槽
static bsp_voice_cb_t s_cb;
static void *s_cb_user;
static bool s_inited;

// 上次执行时刻
static portMUX_TYPE s_rate_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_cmd_last_us[VOICE_CMD_MAX];

// 屏幕用播报副本
static char s_last_spoken[VOICE_SPEAK_MAX];

static const voice_cmd_entry_t *voice_entry_of(voice_cmd_t cmd);
static void                     voice_store_last_spoken(const char *text);

// 越界返 none
static const voice_cmd_entry_t *voice_entry_of(voice_cmd_t cmd) {
    if ((int)cmd < 0 || (int)cmd >= (int)VOICE_CMD_MAX) {
        return &s_cmd_table[VOICE_CMD_NONE];
    }
    return &s_cmd_table[(int)cmd];
}

static bool voice_alias_match(const char *aliases, const char *token) {
    if (aliases == NULL || token == NULL) {
        return false;
    }

    const char *p = aliases;
    while (*p != '\0') {
        const char *line_end = p;
        while (*line_end != '\0' && *line_end != '\n') {
            line_end++;
        }

        const size_t len = (size_t)(line_end - p);
        if (len > 0 && strlen(token) == len && strncmp(p, token, len) == 0) {
            return true;
        }

        if (*line_end == '\0') {
            break;
        }
        p = line_end + 1;
    }
    return false;
}

const char *bsp_voice_cmd_cn(voice_cmd_t cmd) {
    return voice_entry_of(cmd)->cn;
}

// 防抖是否命中
// 抑制不刷新时刻
static bool voice_rate_limited(voice_cmd_t cmd) {
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return false;
    }

    const int64_t now      = esp_timer_get_time();
    const int64_t limit_us = (int64_t)VOICE_RATE_LIMIT_MS * 1000;
    bool          limited  = false;

    portENTER_CRITICAL(&s_rate_mux);
    if ((now - s_cmd_last_us[(int)cmd]) < limit_us) {
        limited = true;
    } else {
        s_cmd_last_us[(int)cmd] = now;
    }
    portEXIT_CRITICAL(&s_rate_mux);

    return limited;
}

// 分发一条指令
// 只许从这里上报
void bsp_voice_dispatch(voice_cmd_t cmd) {
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return;
    }

    const char *name = bsp_voice_cmd_name(cmd);

    // 防抖先于日志
    if (voice_rate_limited(cmd)) {
        ESP_LOGD(TAG, "cmd %s 在 %dms 内重复出现，防抖丢弃", name, VOICE_RATE_LIMIT_MS);
        return;
    }

    // 日志格式别改
    ESP_LOGI(TAG, "cmd: %s", name);

    if (s_cb != NULL) {
        // 回调里别阻塞
        s_cb(VOICE_EVENT_CMD, cmd, NULL, s_cb_user);
    } else {
        ESP_LOGD(TAG, "cmd %s 已识别，但上层还没注册回调（bsp_voice_register_cb）", name);
    }
}

// 只发唤醒事件
void bsp_voice_notify_wake(void) {
    ESP_LOGI(TAG, "已唤醒（唤醒词「%s」），等命令词", bsp_voice_sr_wake_word());

    if (s_cb != NULL) {
        s_cb(VOICE_EVENT_WAKE, VOICE_CMD_NONE, NULL, s_cb_user);
    }
}

// 没麦克风也不报错
esp_err_t bsp_voice_init(void) {
    if (s_inited) {
        ESP_LOGD(TAG, "already inited，忽略重复调用");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "语音输入 = INMP441(I²S%d, SCK=GPIO%d WS=GPIO%d SD=GPIO%d, %dHz) + 板载 ESP-SR",
             (int)BSP_I2S_MIC_PORT, (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO,
             (int)BSP_I2S_MIC_SD_GPIO, BSP_I2S_MIC_SAMPLE_RATE);

    const esp_err_t mic_err = bsp_voice_mic_init();
    if (mic_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "I²S 麦克风初始化失败（%s）：语音输入不可用，"
                 "串口 `say <指令>` 与手机 MQTT 注入仍然可用",
                 esp_err_to_name(mic_err));
    } else if (!bsp_voice_mic_ready()) {
        ESP_LOGW(TAG, "I²S 通道未就绪：检查 INMP441 的 VDD/GND/L-R 与三根信号线");
    }

    const esp_err_t sr_err = bsp_voice_sr_start();
    if (sr_err == ESP_OK) {
        ESP_LOGI(TAG, "识别就绪：先喊「%s」，再说是哪条命令（串口 help-voice 看全部）",
                 bsp_voice_sr_wake_word());
    } else {
        ESP_LOGW(TAG,
                 "ESP-SR 启动失败（%s）：语音控制暂不可用，"
                 "但系统其它功能（串口 say 注入 / 手机 / 屏幕 / 自动联动）全部照常",
                 esp_err_to_name(sr_err));
    }

    s_inited = true;
    return ESP_OK;
}

// 后注册的覆盖
esp_err_t bsp_voice_register_cb(bsp_voice_cb_t cb, void *user) {
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user;
    return ESP_OK;
}

// 注入一条指令
// 短名大小写敏感
esp_err_t bsp_voice_inject(const char *token) {
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const voice_cmd_t cmd = bsp_voice_token_to_cmd(token);
    if (cmd == VOICE_CMD_NONE) {
        return ESP_ERR_NOT_FOUND;
    }

    bsp_voice_dispatch(cmd);
    return ESP_OK;
}

voice_cmd_t bsp_voice_token_to_cmd(const char *token) {
    if (token == NULL) {
        return VOICE_CMD_NONE;
    }

    for (int i = 0; i < (int)VOICE_CMD_MAX; i++) {
        if (strcmp(s_cmd_table[i].name, token) == 0 || voice_alias_match(s_cmd_table[i].aliases, token)) {
            return (voice_cmd_t)i;
        }
    }
    return VOICE_CMD_NONE;
}

const char *bsp_voice_cmd_name(voice_cmd_t cmd) {
    return voice_entry_of(cmd)->name;
}

// 播报文本，不出声
esp_err_t bsp_voice_speak(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "播报: %s", text);

    voice_store_last_spoken(text);
    return ESP_OK;
}

// 全角标点转半角
static void voice_store_last_spoken(const char *text) {
    size_t n = 0;
    for (const char *p = text; *p != '\0' && n + 1u < sizeof(s_last_spoken); p++) {
        switch ((unsigned char)p[0]) {
        // 全角首字节 EF
        case 0xEF:
            // 全角逗号
            if ((unsigned char)p[1] == 0xBC && (unsigned char)p[2] == 0x8C) {
                s_last_spoken[n++] = ',';
                p += 2;
                continue;
            }
            // 全角冒号
            if ((unsigned char)p[1] == 0xBC && (unsigned char)p[2] == 0x9A) {
                s_last_spoken[n++] = ':';
                p += 2;
                continue;
            }
            // 全角句号
            if ((unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x82) {
                s_last_spoken[n++] = '.';
                p += 2;
                continue;
            }
            break;
        default:
            break;
        }
        s_last_spoken[n++] = *p;
    }
    s_last_spoken[n] = '\0';
}

const char *bsp_voice_last_spoken(void) {
    return s_last_spoken;
}

// 播报温度
esp_err_t bsp_voice_speak_temp(float temp_c, bool valid) {
    if (!valid) {
        return bsp_voice_speak("温度传感器没有连接");
    }

    char text[VOICE_SPEAK_MAX];
    snprintf(text, sizeof(text), "当前温度%.1f度", (double)temp_c);
    return bsp_voice_speak(text);
}

// 播报湿度
esp_err_t bsp_voice_speak_humi(float humi, bool valid) {
    if (!valid) {
        return bsp_voice_speak("湿度传感器没有连接");
    }

    char text[VOICE_SPEAK_MAX];
    snprintf(text, sizeof(text), "当前湿度%.0f%%", (double)humi);
    return bsp_voice_speak(text);
}

// 温湿度一句播完
esp_err_t bsp_voice_speak_temp_humi(float temp_c, float humi, bool valid) {
    if (!valid) {
        return bsp_voice_speak("温湿度传感器没有连接");
    }

    char text[VOICE_SPEAK_MAX];
    snprintf(text, sizeof(text), "当前温度%.1f度，湿度%.0f%%", (double)temp_c, (double)humi);
    return bsp_voice_speak(text);
}

// 播报天气
esp_err_t bsp_voice_speak_weather(bool raining, bool valid) {
    if (!valid) {
        return bsp_voice_speak("雨滴传感器没有连接");
    }

    return bsp_voice_speak(raining ? "正在下雨" : "没有下雨");
}

// 播报光照
esp_err_t bsp_voice_speak_light(float light_pct, bool valid) {
    if (!valid) {
        return bsp_voice_speak("光照传感器没有连接");
    }

    return bsp_voice_speak((light_pct < 20.0f) ? "当前光照偏暗" : "光照充足");
}

// 播报模式
// true=自动
esp_err_t bsp_voice_speak_mode(bool auto_mode) {
    return bsp_voice_speak(auto_mode ? "已切换到自动模式" : "已切换到手动模式");
}

// 打印全部指令
void bsp_voice_dump_cmds(void) {
    ESP_LOGI(TAG, "==== 可用语音指令（共 %d 条；短名可直接 bsp_voice_inject(\"短名\") 注入）====",
             (int)VOICE_CMD_MAX - 1);

    for (int i = 1; i < (int)VOICE_CMD_MAX; i++) {
        ESP_LOGI(TAG, "  %-16s  %s", s_cmd_table[i].name, s_cmd_table[i].cn);
    }

    bsp_voice_sr_print_commands();
}

bool bsp_voice_ready(void) {
    return s_inited;
}

// 取麦克风电平
bool bsp_voice_mic_level(int *rms, int *peak, int *dbfs_x10) {
    if (!bsp_voice_mic_ready()) {
        return false;
    }

    bsp_voice_mic_level_t lv;
    bsp_voice_mic_get_level(&lv);

    if (rms != NULL) {
        *rms = lv.rms;
    }
    if (peak != NULL) {
        *peak = lv.peak;
    }
    if (dbfs_x10 != NULL) {
        *dbfs_x10 = lv.db_x10;
    }

    return lv.read_count > 0;
}

bool bsp_voice_mic_data_in_other_slot(void) {
    if (!bsp_voice_mic_ready()) {
        return false;
    }

    bsp_voice_mic_level_t lv;
    bsp_voice_mic_get_level(&lv);

    // 右声道有明显信号
    return (lv.peak <= 32) && (lv.peak_other > 200);
}

const char *bsp_voice_wake_word(void) {
    return bsp_voice_sr_wake_word();
}

bool bsp_voice_is_awake(void) {
    return bsp_voice_sr_awake();
}
