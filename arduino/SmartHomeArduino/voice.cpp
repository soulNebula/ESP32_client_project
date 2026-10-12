// c
// 语音指令表与播报
//
// 板载识别用 arduino-esp32 自带的 ESP_SR 库（WakeNet 唤醒 + MultiNet 命令词），
// 和 ESP-IDF 版是同一套模型，所以说法与识别结果完全一致。
//
// ⚠ 用 ESP_SR 必须先把开发板设置改成：
//     Tools → Partition Scheme → "ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"
//   这个方案会把 srmodels.bin 烧到 0xC10000，App 分区上限 3MB（本工程够用）。
//   不想用板载识别就把 app_config.h 的 APP_USE_ESP_SR 改成 0：编译一样过，
//   只是"喊不动"，改用手机 App 或串口 say <短名> 下发指令。

#include <stdio.h>
#include <string.h>

#include "voice.h"
#include "app_config.h"
#include "bsp_config.h"
#include "device_model.h"

static const char *TAG = "VOICE";

// 播报文本长度
#define VOICE_SPEAK_MAX 96
// 上一次播报留档
#define VOICE_LAST_MAX 64

// 命令词表上限（含唤醒词占位）
#define VOICE_CMD_TABLE_MAX 48

// 唤醒词
#define VOICE_WAKE_WORD "你好小智"

// 唤醒后多久内说命令
#define VOICE_CMD_TIMEOUT_MS 6000

// INMP441 数据在 32bit 槽里
#define VOICE_MIC_BITS I2S_DATA_BIT_WIDTH_32BIT

// 没认出来时的占位
#define VOICE_WAKE_PLACEHOLDER "（本版没有板载唤醒词）"

typedef struct {
    // 规范短名
    const char *name;
    // 别名 \n 分隔
    const char *aliases;
    // 中文说法
    const char *cn;
} voice_cmd_entry_t;

// 指令对照表
// 顺序必须与 voice.h 的枚举一一对应
static const voice_cmd_entry_t s_cmd_table[] = {
    // 0
    {           "none",     "nop\nnoop", "（无指令 / 空）"},
    // 1
    {  "led_living_on",     "living_on",       "打开客厅灯"},
    // 2
    { "led_living_off",    "living_off",       "关闭客厅灯"},
    // 3
    { "led_kitchen_on",     "kitchen_on",       "打开厨房灯"},
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
static_assert(sizeof(s_cmd_table) / sizeof(s_cmd_table[0]) == (size_t)VOICE_CMD_MAX,
              "s_cmd_table[] 条目数必须等于 VOICE_CMD_MAX（与 voice.h 枚举一一对应）");
static_assert((int)VOICE_CMD_MAX - (int)VOICE_CMD_QUERY_STATUS == 1,
              "s_cmd_table[] 末项必须对应 VOICE_CMD_QUERY_STATUS，枚举顺序被改动过？");

static bsp_voice_cb_t s_cb;
static void          *s_cb_user;

static bool s_inited;
static bool s_native_ok;
// 唤醒后的窗口期
static bool s_awake;

// 上一次播报留档
static char s_last_spoken[VOICE_LAST_MAX];

// 自己认过的指令
static char s_last_token[32];

// 越界给 none
static const voice_cmd_entry_t *voice_entry_of(voice_cmd_t cmd) {
    if ((int)cmd < 0 || (int)cmd >= (int)VOICE_CMD_MAX) {
        return &s_cmd_table[VOICE_CMD_NONE];
    }
    return &s_cmd_table[(int)cmd];
}

// 别名表用 \n 分隔
static bool voice_alias_match(const char *aliases, const char *token) {
    if (aliases == NULL || token == NULL) {
        return false;
    }

    const char *p = aliases;
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        const size_t n  = (end != NULL) ? (size_t)(end - p) : strlen(p);

        if (strlen(token) == n && strncmp(p, token, n) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return false;
}

// 全角标点转半角
// 屏上只认 ASCII 标点
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

// 上报一条指令
static void voice_emit_cmd(voice_cmd_t cmd, const char *raw) {
    snprintf(s_last_token, sizeof(s_last_token), "%s", s_cmd_table[(int)cmd].name);
    LOGI(TAG, "识别: %s（%s）", s_last_token, s_cmd_table[(int)cmd].cn);

    if (s_cb != NULL) {
        s_cb(VOICE_EVENT_CMD, cmd, raw, s_cb_user);
    }
}

// 板载识别这一段

#if APP_USE_ESP_SR

#include "ESP_SR.h"

// 板载识别用
static bool s_sr_started;

// 命令词表：0 号是唤醒词，1 起对应 voice_cmd_t
static sr_cmd_t s_sr_cmds[VOICE_CMD_TABLE_MAX];

// 填命令词表
// 每个说法一条，command_id 用同一个
static size_t voice_build_sr_cmds(void) {
    size_t n = 0;

    // 0 号放唤醒词
    s_sr_cmds[n].command_id = 0;
    snprintf(s_sr_cmds[n].str, SR_CMD_STR_LEN_MAX, "%s", VOICE_WAKE_WORD);
    n++;

    for (int i = 1; i < (int)VOICE_CMD_MAX && n < VOICE_CMD_TABLE_MAX; i++) {
        s_sr_cmds[n].command_id = i;
        snprintf(s_sr_cmds[n].str, SR_CMD_STR_LEN_MAX, "%s", s_cmd_table[i].cn);
        n++;

        // 别名说法也要能说
        const char *p = s_cmd_table[i].aliases;
        while (p != NULL && *p != '\0' && n < VOICE_CMD_TABLE_MAX) {
            const char *end = strchr(p, '\n');
            const size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);

            if (len > 0) {
                s_sr_cmds[n].command_id = i;
                snprintf(s_sr_cmds[n].str, SR_CMD_STR_LEN_MAX, "%.*s", (int)len, p);
                n++;
            }
            if (end == NULL) {
                break;
            }
            p = end + 1;
        }
    }

    LOGI(TAG, "命令词表共 %u 条（含唤醒词 1 条）", (unsigned)n);
    return n;
}

// ESP_SR 的回调
static void voice_sr_event(sr_event_t event, int command_id, int phrase_id) {
    switch (event) {
    case SR_EVENT_WAKEWORD:
        s_awake = true;
        LOGI(TAG, "语音助手被唤醒（%d ms 内说命令）", VOICE_CMD_TIMEOUT_MS);
        if (s_cb != NULL) {
            s_cb(VOICE_EVENT_WAKE, VOICE_CMD_NONE, NULL, s_cb_user);
        }
        // 唤醒后转命令词模式
        if (!ESP_SR.setMode(SR_MODE_COMMAND)) {
            LOGW(TAG, "切到命令词模式失败");
        }
        break;

    case SR_EVENT_COMMAND:
        s_awake = false;
        if (command_id > 0 && command_id < (int)VOICE_CMD_MAX && phrase_id >= 0 &&
            phrase_id < (int)VOICE_CMD_TABLE_MAX) {
            voice_emit_cmd((voice_cmd_t)command_id, s_sr_cmds[phrase_id].str);
        } else {
            LOGW(TAG, "命令 ID %d / 词条 %d 越界，忽略", command_id, phrase_id);
        }
        // 说完回唤醒词模式
        if (!ESP_SR.setMode(SR_MODE_WAKEWORD)) {
            LOGW(TAG, "切回唤醒词模式失败");
        }
        break;

    case SR_EVENT_TIMEOUT:
        // 没听清就当没这回事
        if (s_awake) {
            LOGI(TAG, "没等到命令，回唤醒词模式");
            s_awake = false;
        }
        (void)ESP_SR.setMode(SR_MODE_WAKEWORD);
        break;

    default:
        break;
    }
}

// 起麦克风和识别
static bool voice_start_native(void) {
    // INMP441 走标准 I2S 输入
    static I2SClass i2s;
    i2s.setPins(BSP_I2S_MIC_SCK_GPIO, BSP_I2S_MIC_WS_GPIO, -1, BSP_I2S_MIC_SD_GPIO);

    // 16k 立体声，左声道接 GND 时有效数据在前一槽
    if (!i2s.begin(I2S_MODE_STD, APP_VOICE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
        LOGE(TAG, "I2S 麦克风起不来（SCK=GPIO%d WS=GPIO%d SD=GPIO%d）", BSP_I2S_MIC_SCK_GPIO,
             BSP_I2S_MIC_WS_GPIO, BSP_I2S_MIC_SD_GPIO);
        LOGE(TAG, "  → 查 VDD 是否 3V3、L/R 是否接 GND、三根线是否接反");
        return false;
    }

    const size_t n = voice_build_sr_cmds();

    ESP_SR.onEvent(voice_sr_event);

    // "MN" = 一路麦克风 + 一路没用
    if (!ESP_SR.begin(i2s, s_sr_cmds, n, SR_CHANNELS_STEREO, SR_MODE_WAKEWORD, "MN")) {
        LOGE(TAG, "ESP_SR 起不来 —— 最常见原因：分区方案没选对");
        LOGE(TAG, "  → Tools → Partition Scheme 必须选 \"ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)\"");
        LOGE(TAG, "  → 选完【重新烧录一次】（模型要烧进 model 分区）");
        return false;
    }

    s_sr_started = true;
    return true;
}

#else

// 关掉时空跑
static bool voice_start_native(void) {
    return false;
}

// 板载识别这一段收尾
#endif

// 没麦也算成功
app_err_t bsp_voice_init(void) {
    if (s_inited) {
        return APP_OK;
    }

    memset(s_last_spoken, 0, sizeof(s_last_spoken));
    memset(s_last_token, 0, sizeof(s_last_token));

    s_inited = true;

#if APP_USE_ESP_SR
    s_native_ok = voice_start_native();
    if (s_native_ok) {
        LOGI(TAG, "板载识别已启动：先喊「%s」，%d 秒内说命令", VOICE_WAKE_WORD, VOICE_CMD_TIMEOUT_MS / 1000);
        LOGI(TAG, "说法表见串口命令 help-voice");
    } else {
        LOGW(TAG, "板载识别不可用 —— 手机 App 与串口 say <短名> 照常，走的是同一条下游");
    }
#else
    s_native_ok = false;
    LOGW(TAG, "板载识别被 APP_USE_ESP_SR=0 关掉");
    LOGI(TAG, "手机 App 下发指令 / 串口 say <短名> 都走【和真语音完全同一条下游】");
#endif

    return APP_OK;
}

// 只有一个回调
app_err_t bsp_voice_register_cb(bsp_voice_cb_t cb, void *user) {
    s_cb      = cb;
    s_cb_user = user;

    LOGD(TAG, "语音回调%s", (cb != NULL) ? "已注册" : "已注销");
    return APP_OK;
}

// 短名或别名转枚举
voice_cmd_t bsp_voice_token_to_cmd(const char *token) {
    if (token == NULL || token[0] == '\0') {
        return VOICE_CMD_NONE;
    }

    for (int i = 0; i < (int)VOICE_CMD_MAX; i++) {
        if (strcmp(s_cmd_table[i].name, token) == 0 || voice_alias_match(s_cmd_table[i].aliases, token)) {
            return (voice_cmd_t)i;
        }
    }
    return VOICE_CMD_NONE;
}

// 走真实语音同链路
app_err_t bsp_voice_inject(const char *token) {
    const voice_cmd_t cmd = bsp_voice_token_to_cmd(token);
    if (cmd == VOICE_CMD_NONE) {
        LOGW(TAG, "不认识的指令 \"%s\"（help-voice 看全部）", (token != NULL) ? token : "(null)");
        return APP_ERR_NOT_FOUND;
    }

    LOGI(TAG, "注入指令: %s", s_cmd_table[(int)cmd].name);
    voice_emit_cmd(cmd, s_cmd_table[(int)cmd].name);
    return APP_OK;
}

const char *bsp_voice_cmd_name(voice_cmd_t cmd) {
    return voice_entry_of(cmd)->name;
}

// 只有这一张表
const char *bsp_voice_cmd_cn(voice_cmd_t cmd) {
    return voice_entry_of(cmd)->cn;
}

// 播报文本，不出声
app_err_t bsp_voice_speak(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return APP_ERR_INVALID_ARG;
    }

    LOGI(TAG, "播报: %s", text);

    voice_store_last_spoken(text);
    return APP_OK;
}

app_err_t bsp_voice_speak_temp(float temp_c, bool valid) {
    if (!valid) {
        return bsp_voice_speak("温度传感器没有连接");
    }

    char text[VOICE_SPEAK_MAX];
    snprintf(text, sizeof(text), "当前温度%.1f度", (double)temp_c);
    return bsp_voice_speak(text);
}

app_err_t bsp_voice_speak_humi(float humi, bool valid) {
    if (!valid) {
        return bsp_voice_speak("湿度传感器没有连接");
    }

    char text[VOICE_SPEAK_MAX];
    snprintf(text, sizeof(text), "当前湿度%.0f%%", (double)humi);
    return bsp_voice_speak(text);
}

// 温湿度一句播完
app_err_t bsp_voice_speak_temp_humi(float temp_c, float humi, bool valid) {
    if (!valid) {
        return bsp_voice_speak("温湿度传感器没有连接");
    }

    char text[VOICE_SPEAK_MAX];
    snprintf(text, sizeof(text), "当前温度%.1f度，湿度%.0f%%", (double)temp_c, (double)humi);
    return bsp_voice_speak(text);
}

app_err_t bsp_voice_speak_weather(bool raining, bool valid) {
    if (!valid) {
        return bsp_voice_speak("雨滴传感器没有连接");
    }

    return bsp_voice_speak(raining ? "正在下雨" : "没有下雨");
}

app_err_t bsp_voice_speak_light(float light_pct, bool valid) {
    if (!valid) {
        return bsp_voice_speak("光照传感器没有连接");
    }

    return bsp_voice_speak((light_pct < 20.0f) ? "当前光照偏暗" : "光照充足");
}

// 播报模式
// true=自动
app_err_t bsp_voice_speak_mode(bool auto_mode) {
    return bsp_voice_speak(auto_mode ? "已切换到自动模式" : "已切换到手动模式");
}

// 打印全部指令
void bsp_voice_dump_cmds(void) {
    LOGI(TAG, "==== 可用语音指令（共 %d 条；短名可直接 say <短名> 注入）====", (int)VOICE_CMD_MAX - 1);

    for (int i = 1; i < (int)VOICE_CMD_MAX; i++) {
        LOGI(TAG, "  %-16s  %s", s_cmd_table[i].name, s_cmd_table[i].cn);
    }

    LOGI(TAG, "======================================================");
    if (s_native_ok) {
        LOGI(TAG, "板载识别: 已开启，先喊「%s」", VOICE_WAKE_WORD);
    } else {
        LOGI(TAG, "板载识别: 未开启（手机 App 或串口 say 下发，效果一样）");
    }
}

// 没麦也算就绪
bool bsp_voice_ready(void) {
    return s_inited;
}

// 没启动返回占位
const char *bsp_voice_wake_word(void) {
#if APP_USE_ESP_SR
    return s_native_ok ? VOICE_WAKE_WORD : VOICE_WAKE_PLACEHOLDER;
#else
    return VOICE_WAKE_PLACEHOLDER;
#endif
}

const char *bsp_voice_last_spoken(void) {
    return s_last_spoken;
}

bool bsp_voice_is_awake(void) {
    return s_awake;
}

// 外部识别结果进来
void bsp_voice_feed_text(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }

    // 当一句原始识别结果上报
    if (s_cb != NULL) {
        s_cb(VOICE_EVENT_RAW, VOICE_CMD_NONE, text, s_cb_user);
    }

    // 能对上就当指令
    (void)bsp_voice_inject(text);
}

// 板载识别到底起没起来
bool bsp_voice_native_ready(void) {
    return s_native_ok;
}
