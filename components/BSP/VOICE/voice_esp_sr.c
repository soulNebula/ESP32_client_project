// c
// 唤醒词与命令词
#include "sdkconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

// 只用采样率
#include "board_config.h"
#include "voice.h"
// 唯一出口
#include "voice_internal.h"
#include "i2s_mic.h"
#include "voice_esp_sr.h"

static const char *TAG = "voice.sr";

// 12KB 防溢出
#define VOICE_SR_FEED_STACK 12288
#define VOICE_SR_TASK_STACK 12288

// 低于主循环 5
#define VOICE_SR_FEED_PRIO 4
#define VOICE_SR_TASK_PRIO 4

// 音频别上核 0
#define VOICE_SR_TASK_CORE 1

// 超时多是没接麦
#define VOICE_SR_READ_TIMEOUT_MS 1000

// 设大会变迟钝
#define VOICE_SR_FETCH_TIMEOUT_MS 100

// 等人说话 6 秒
#define VOICE_SR_MN_DURATION_MS 6000

// 5 秒一条日志
#define VOICE_SR_STATUS_GAP_US (5LL * 1000 * 1000)
// 告警限 30 秒
#define VOICE_SR_ZERO_LOG_GAP_US (30LL * 1000 * 1000)

// 安静阈值 32
#define VOICE_SR_SILENT_P2P 32

// 2.0 实测最稳
#define VOICE_SR_AFE_GAIN 2.0f

// 失败日志降频
#define VOICE_SR_FAIL_LOG_EVERY 100

// 采样点换毫秒
#define VOICE_SR_SAMPLES_TO_MS(n) ((int)(((int64_t)(n) * 1000LL) / BSP_I2S_MIC_SAMPLE_RATE))

// 命令词表项
typedef struct {
    // 中文说法
    const char *cn;
    // 拼音才是识别单元
    const char *pinyin;
    // 对应指令 id
    voice_cmd_t cmd;
} voice_sr_cmd_entry_t;

// 拼音字段上限
#define VOICE_SR_PINYIN_MAX 64

// 拼音错就不命中
static const voice_sr_cmd_entry_t s_cmds[] = {
    {"打开客厅灯","da kai ke ting deng",   VOICE_CMD_LED_LIVING_ON                       },
    {"关闭客厅灯",  "guan bi ke ting deng",  VOICE_CMD_LED_LIVING_OFF},
    {"打开厨房灯",  "da kai chu fang deng",  VOICE_CMD_LED_KITCHEN_ON},
    {"关闭厨房灯", "guan bi chu fang deng", VOICE_CMD_LED_KITCHEN_OFF},
    {"打开卧室灯",    "da kai wo shi deng",  VOICE_CMD_LED_BEDROOM_ON},
    {"关闭卧室灯",   "guan bi wo shi deng", VOICE_CMD_LED_BEDROOM_OFF},
    {"打开浴室灯",    "da kai yu shi deng",     VOICE_CMD_LED_BATH_ON},
    {"关闭浴室灯",   "guan bi yu shi deng",    VOICE_CMD_LED_BATH_OFF},

    // 全部灯不开门
    {"打开全部灯",   "da kai quan bu deng",      VOICE_CMD_LED_ALL_ON},
    {"关闭全部灯",  "guan bi quan bu deng",     VOICE_CMD_LED_ALL_OFF},

    {      "打开门",            "da kai men",       VOICE_CMD_DOOR_OPEN},
    {      "关上门",        "guan shang men",      VOICE_CMD_DOOR_CLOSE},

    // 不做关闭自动模式
    // 裸说法放第一位
    {   "自动模式",
     "zi dong mo shi;"
     "kai qi zi dong mo shi;"
     "da kai zi dong mo shi;"
     "qie huan dao zi dong mo shi",       VOICE_CMD_MODE_AUTO             },
    {   "手动模式",
     "shou dong mo shi;"
     "kai qi shou dong mo shi;"
     "da kai shou dong mo shi;"
     "qie huan dao shou dong mo shi",     VOICE_CMD_MODE_MANUAL           },

    {"关闭蜂鸣器",  "guan bi feng ming qi",      VOICE_CMD_BUZZER_OFF},
    {"打开蜂鸣器",   "da kai feng ming qi",       VOICE_CMD_BUZZER_ON},

    // 查询不出声
    {   "温度多少",       "wen du duo shao",      VOICE_CMD_QUERY_TEMP},
    {   "湿度多少",       "shi du duo shao",      VOICE_CMD_QUERY_HUMI},
    {"温湿度多少",   "wen shi du duo shao",       VOICE_CMD_QUERY_ALL},
    {"天气怎么样",   "tian qi zen me yang",   VOICE_CMD_QUERY_WEATHER},
    {   "光照多少",   "guang zhao duo shao",     VOICE_CMD_QUERY_LIGHT},
    {   "状态如何",      "zhuang tai ru he",    VOICE_CMD_QUERY_STATUS},
};

#define VOICE_SR_CMD_NUM (sizeof(s_cmds) / sizeof(s_cmds[0]))

// 条数必须正好
_Static_assert(sizeof(s_cmds) / sizeof(s_cmds[0]) == (size_t)VOICE_CMD_MAX - 1u,
               "s_cmds[] 必须正好覆盖 VOICE_CMD_NONE 之后的全部指令（本工程 22 条）");

// 加句式要调上限
_Static_assert(
    sizeof(s_cmds[0].pinyin) <= VOICE_SR_PINYIN_MAX && sizeof(s_cmds[1].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[2].pinyin) <= VOICE_SR_PINYIN_MAX && sizeof(s_cmds[3].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[4].pinyin) <= VOICE_SR_PINYIN_MAX && sizeof(s_cmds[5].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[6].pinyin) <= VOICE_SR_PINYIN_MAX && sizeof(s_cmds[7].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[8].pinyin) <= VOICE_SR_PINYIN_MAX && sizeof(s_cmds[9].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[10].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[11].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[12].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[13].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[14].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[15].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[16].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[17].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[18].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[19].pinyin) <= VOICE_SR_PINYIN_MAX &&
        sizeof(s_cmds[20].pinyin) <= VOICE_SR_PINYIN_MAX && sizeof(s_cmds[21].pinyin) <= VOICE_SR_PINYIN_MAX,
    "有 pinyin 字段超过 VOICE_SR_PINYIN_MAX：请同步调大上限");

static bool s_started = false;
// 模型与任务就绪
static volatile bool s_ready = false;
static volatile bool s_awake = false;

// 模型清单
static srmodel_list_t *s_models = NULL;
// 不 free
static char *s_wn_name = NULL;
static char *s_mn_name = NULL;
static char s_wake_word[64] = {0};

static const esp_afe_sr_iface_t *s_afe      = NULL;
static esp_afe_sr_data_t        *s_afe_data = NULL;
// 一帧多少点
static int s_feed_chunksize = 0;

static const esp_mn_iface_t *s_mn      = NULL;
static model_iface_data_t   *s_mn_data = NULL;

// 一次恰好一帧
static int16_t *s_feed_buf = NULL;
// 查栈水位
static TaskHandle_t s_feed_task_h = NULL;

// 计数跨任务读
static volatile uint32_t s_st_feed_ok = 0;
static volatile uint32_t s_st_feed_fail = 0;
// 两级判据别合
static volatile uint32_t s_st_zero = 0;
// 严格全 0 帧
static volatile uint32_t s_st_dead = 0;

// 先声明内部函数
static esp_err_t   voice_sr_pick_models(void);
static esp_err_t   voice_sr_register_commands(void);
static void        voice_sr_feed_task(void *arg);
static void        voice_sr_task(void *arg);
static const char *voice_sr_nth_phrase(const char *pinyin_field, unsigned idx);

// 取一句拼音
static const char *voice_sr_nth_phrase(const char *pinyin_field, unsigned idx) {
    if (pinyin_field == NULL) {
        return NULL;
    }

    const char *p = pinyin_field;
    unsigned    n = 0;

    while (*p != '\0') {
        const char *end = p;
        while (*end != '\0' && *end != ';') {
            end++;
        }

        // 空段不算一句
        if (end > p) {
            if (n == idx) {
                return p;
            }
            n++;
        }

        if (*end == '\0') {
            break;
        }
        p = end + 1;
    }

    return NULL;
}

// 挑模型，逐级回退
static esp_err_t voice_sr_pick_models(void) {
    if (s_models == NULL) {
        ESP_LOGE(TAG, "模型清单为空（esp_srmodel_init 没成功），无法挑选模型");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "model 分区里共有 %d 个模型：", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i, s_models->model_name[i]);
    }

    {
        static const char *const prefer[] = {"wn9s_", "wn9l_", "wn9_", "wn8_", "wn7_", NULL};
        for (int p = 0; prefer[p] != NULL && s_wn_name == NULL; p++) {
            for (int i = 0; i < s_models->num; i++) {
                if (strstr(s_models->model_name[i], prefer[p]) != NULL) {
                    s_wn_name = s_models->model_name[i];
                    break;
                }
            }
        }
    }
    if (s_wn_name == NULL) {
        ESP_LOGE(TAG, "model 分区里【找不到唤醒词模型（wn*）】—— 下一步这么做：");
        ESP_LOGE(TAG, "  1) menuconfig → ESP Speech Recognition → Wake word 勾一个"
                      "（本工程默认 wn9s_nihaoxiaozhi「你好小智」）；");
        ESP_LOGE(TAG, "  2) 删掉 sdkconfig 重新 set-target（sdkconfig.defaults 才生效）；");
        ESP_LOGE(TAG, "  3) 重新 idf.py flash，让 srmodels.bin 跟着一起烧进 model 分区。");
        return ESP_ERR_NOT_FOUND;
    }

    // 先中文再不限
    s_mn_name = esp_srmodel_filter(s_models, "mn", ESP_MN_CHINESE);
    if (s_mn_name == NULL) {
        s_mn_name = esp_srmodel_filter(s_models, "mn", NULL);
    }
    if (s_mn_name == NULL) {
        ESP_LOGE(TAG, "model 分区里【找不到命令词模型（mn*）】—— 下一步这么做：");
        ESP_LOGE(TAG, "  1) menuconfig → ESP Speech Recognition → "
                      "Chinese Speech Commands Model 勾一个（本工程默认 mn7_cn）；");
        ESP_LOGE(TAG, "  2) 确认没有勾 CONFIG_SR_MN_CN_NONE；");
        ESP_LOGE(TAG, "  3) 重新 idf.py flash（模型不进固件，必须烧 model 分区）。");
        return ESP_ERR_NOT_FOUND;
    }

    // 文本取不到没事
    char *ww = esp_srmodel_get_wake_words(s_models, s_wn_name);
    if (ww != NULL) {
        strncpy(s_wake_word, ww, sizeof(s_wake_word) - 1u);
        s_wake_word[sizeof(s_wake_word) - 1u] = '\0';
        // 必须 free
        free(ww);
    } else {
        strncpy(s_wake_word, "(未知，看模型名)", sizeof(s_wake_word) - 1u);
        s_wake_word[sizeof(s_wake_word) - 1u] = '\0';
    }

    ESP_LOGI(TAG, "★ 唤醒词模型 = %s，要喊的词 = 「%s」", s_wn_name, s_wake_word);
    ESP_LOGI(TAG, "★ 命令词模型 = %s", s_mn_name);
    return ESP_OK;
}

// 三步顺序不能变
static esp_err_t voice_sr_register_commands(void) {
    if (s_mn == NULL || s_mn_data == NULL) {
        ESP_LOGE(TAG, "MultiNet 还没初始化，无法注册命令词");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_mn_commands_alloc(s_mn, s_mn_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mn_commands_alloc 失败: %s", esp_err_to_name(err));
        return err;
    }

    int ok  = 0;
    int bad = 0;
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        // 要补 '\0'
        char one[VOICE_SR_PINYIN_MAX];

        for (unsigned k = 0;; k++) {
            const char *ph = voice_sr_nth_phrase(s_cmds[i].pinyin, k);
            if (ph == NULL) {
                break;
            }

            // 再扫一次找结尾
            const char *end = ph;
            while (*end != '\0' && *end != ';') {
                end++;
            }

            const size_t n = (size_t)(end - ph);
            if (n >= sizeof(one)) {
                // 宁可报警别截断
                ESP_LOGW(TAG, "「%s」的第 %u 句拼音太长（%u 字节 ≥ 缓冲 %u），已跳过", s_cmds[i].cn, k + 1,
                         (unsigned)n, (unsigned)sizeof(one));
                bad++;
                continue;
            }

            memcpy(one, ph, n);
            one[n] = '\0';

            // 交拼音不是中文
            const esp_err_t e = esp_mn_commands_add((int)s_cmds[i].cmd, one);
            if (e == ESP_OK) {
                ok++;
            } else {
                bad++;
                ESP_LOGW(TAG, "命令词注册失败：「%s」(%s) id=%d -> %s", s_cmds[i].cn, one, (int)s_cmds[i].cmd,
                         esp_err_to_name(e));
            }
        }
    }

    // 必须 update
    esp_mn_error_t *mn_err = esp_mn_commands_update();
    if (mn_err != NULL && mn_err->num > 0) {
        ESP_LOGW(TAG, "有 %d 条命令词【无法被 MultiNet 解析】（这些说法不会被识别）：", (int)mn_err->num);
        for (int i = 0; i < mn_err->num; i++) {
            ESP_LOGW(TAG, "  %s", mn_err->phrases[i]->string);
        }
        ESP_LOGW(TAG, "  多半是拼音写错（音节要对、用空格分开），"
                      "对照 bsp_voice_sr_print_commands() 打的表逐条核对。");
    }

    ESP_LOGI(TAG,
             "命令词注册完成：成功 %d 句 / 失败 %d 句，共 %d 条指令"
             "（id 就是 voice_cmd_t 枚举值，一条指令可挂多句同义说法）",
             ok, bad, (int)VOICE_SR_CMD_NUM);

    return (ok > 0) ? ESP_OK : ESP_FAIL;
}

// 喂帧不能停
static void voice_sr_feed_task(void *arg) {
    (void)arg;

    // 先等 300ms
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "喂帧任务启动：每帧 %d 点（%d ms @%dHz），目标 %d 帧/秒", s_feed_chunksize,
             VOICE_SR_SAMPLES_TO_MS(s_feed_chunksize), BSP_I2S_MIC_SAMPLE_RATE,
             BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize);

    // 安静提示降频
    int64_t last_quiet_note_us = 0;

    for (;;) {
        size_t          got = 0;
        const esp_err_t err =
            bsp_voice_mic_read(s_feed_buf, (size_t)s_feed_chunksize, &got, VOICE_SR_READ_TIMEOUT_MS);

        if (got < (size_t)s_feed_chunksize) {
            s_st_feed_fail++;
            if ((s_st_feed_fail == 1u) || ((s_st_feed_fail % VOICE_SR_FAIL_LOG_EVERY) == 0u)) {
                ESP_LOGW(TAG,
                         "读麦克风未凑满一帧 %u 次（%s，本次 %u/%d 点）："
                         "麦克风可能没接好，语音识别暂不可用（系统其它功能不受影响）",
                         (unsigned)s_st_feed_fail, esp_err_to_name(err), (unsigned)got, s_feed_chunksize);
            }
            // 让出 CPU
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 峰峰 0 是断线
        bsp_voice_mic_level_t lv;
        bsp_voice_mic_get_level(&lv);
        if (lv.peak_to_peak <= VOICE_SR_SILENT_P2P) {
            s_st_zero++;
            const int64_t now = esp_timer_get_time();

            if (lv.peak_to_peak == 0) {
                // 偶发全 0 是毛刺
                s_st_dead++;
            } else if (now - last_quiet_note_us > VOICE_SR_ZERO_LOG_GAP_US) {
                // 别打成告警
                last_quiet_note_us = now;
                ESP_LOGD(TAG, "环境安静（峰峰值=%d <= %d）：不是故障，只是这段没人说话", lv.peak_to_peak,
                         VOICE_SR_SILENT_P2P);
            }
        }

        const int feed_ret = s_afe->feed(s_afe_data, s_feed_buf);
        if (feed_ret < 0) {
            s_st_feed_fail++;
            if ((s_st_feed_fail == 1u) || ((s_st_feed_fail % VOICE_SR_FAIL_LOG_EVERY) == 0u)) {
                ESP_LOGW(TAG, "AFE feed() 失败 %u 次，重置 AFE 环形缓冲（读写指针错位）",
                         (unsigned)s_st_feed_fail);
            }
            // 失败就重置缓冲
            s_afe->reset_buffer(s_afe_data);
        } else {
            s_st_feed_ok++;
        }
    }
}

// 周期状态日志
static void voice_sr_log_status(int64_t win_us, uint32_t w_feeds, uint32_t w_zeros, uint32_t w_dead,
                                uint32_t w_frames, uint32_t w_null) {
    bsp_voice_mic_level_t lv;
    bsp_voice_mic_get_level(&lv);

    const int feed_rate  = (win_us > 0) ? (int)((int64_t)w_feeds * 1000000 / win_us) : 0;
    const int fetch_rate = (win_us > 0) ? (int)((int64_t)w_frames * 1000000 / win_us) : 0;

    // 过半全 0 断线
    if (w_feeds > 0u && (w_dead * 2u) >= w_feeds) {
        ESP_LOGW(TAG,
                 "本窗口 %u/%u 帧是【严格全 0】音频（峰峰值=0）：这是接线/供电问题的特征 —— "
                 "① INMP441 的 SD 脚没接/接错 ② 没供电（VDD 要 3V3，不要 5V）"
                 "③ L/R 没接 GND。串口敲 `mic` 看实时电平可当场确认。",
                 (unsigned)w_dead, (unsigned)w_feeds);

        // 数据跑到右声道
        if (bsp_voice_mic_data_in_other_slot()) {
            ESP_LOGW(TAG, "★ 但【另一个 I²S 时隙有信号】→ 数据跑到右声道了："
                          "把 INMP441 的 L/R 接到 GND（悬空或接 3V3 都会这样），"
                          "不用查供电和 SD。");
        }
    } else if (w_dead > 0u) {
        ESP_LOGD(TAG, "本窗口有 %u 个孤立全 0 帧（共 %u 帧）：I²S 缓冲切换的正常毛刺，不是故障",
                 (unsigned)w_dead, (unsigned)w_feeds);
    }

    // 不用 %f 打
    const int db_abs = (lv.db_x10 < 0) ? -lv.db_x10 : lv.db_x10;

    ESP_LOGI(TAG,
             "[状态] feed %u 帧/%d 帧每秒（目标 %d，每帧 %d 点）"
             " | fetch %u 次/%d 次每秒（每次 %d 点，空 %u 次）"
             " | 本窗口安静帧=%u（其中严格全 0=%u）"
             " | 累计：喂失败=%u 全 0 帧=%u"
             " | rms=%d peak=%d 峰峰=%d dBFS=%s%d.%d"
             " | 唤醒=%s 就绪=%s"
             " | 栈余 feed=%uB 识别=%uB",
             (unsigned)w_feeds, feed_rate, BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize, s_feed_chunksize,
             (unsigned)w_frames, fetch_rate, s_afe->get_fetch_chunksize(s_afe_data), (unsigned)w_null,
             (unsigned)w_zeros, (unsigned)w_dead, (unsigned)s_st_feed_fail, (unsigned)s_st_dead, lv.rms,
             lv.peak, lv.peak_to_peak, (lv.db_x10 < 0) ? "-" : "+", db_abs / 10, db_abs % 10,
             s_awake ? "是" : "否", s_ready ? "是" : "否",
             // 水位乘 word
             (unsigned)((s_feed_task_h != NULL)
                            ? uxTaskGetStackHighWaterMark(s_feed_task_h) * sizeof(StackType_t)
                            : 0u),
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

// 识别主循环
static void voice_sr_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "识别任务启动：先喊「%s」，看到日志『已唤醒』之后再说命令词", s_wake_word);
    ESP_LOGI(TAG, "命令词共 %d 条，串口敲 help-voice 可打印完整列表", (int)VOICE_SR_CMD_NUM);

    uint32_t w_frames = 0;
    uint32_t w_null = 0;
    // 取快照算增量
    uint32_t last_feed_seen = 0;
    uint32_t last_zero_seen = 0;
    uint32_t last_dead_seen = 0;
    int64_t  last_status_us = esp_timer_get_time();

    ESP_LOGI(TAG, "识别任务进入主循环：阻塞式 fetch，超时 %d ms", VOICE_SR_FETCH_TIMEOUT_MS);

    // 先等第一帧
    for (int wait_ms = 0; wait_ms < 1000 && s_st_feed_ok == 0u; wait_ms += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    for (;;) {
        afe_fetch_result_t *res =
            s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(VOICE_SR_FETCH_TIMEOUT_MS));

        // 日志放循环开头
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_status_us > VOICE_SR_STATUS_GAP_US) {
            const uint32_t feed_now = s_st_feed_ok;
            const uint32_t zero_now = s_st_zero;
            const uint32_t dead_now = s_st_dead;
            // 回绕也安全
            const uint32_t w_feeds = feed_now - last_feed_seen;
            const uint32_t w_zeros = zero_now - last_zero_seen;
            const uint32_t w_dead  = dead_now - last_dead_seen;
            last_feed_seen         = feed_now;
            last_zero_seen         = zero_now;
            last_dead_seen         = dead_now;

            voice_sr_log_status(now_us - last_status_us, w_feeds, w_zeros, w_dead, w_frames, w_null);
            last_status_us = now_us;
            w_frames       = 0;
            w_null         = 0;
        }

        if (res == NULL || res->ret_value == -1) {
            // 空拍正常
            w_null++;
            continue;
        }
        w_frames++;

        // 唤醒命中
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (!s_awake) {
                s_awake = true;
                ESP_LOGI(TAG, "★ 已唤醒（唤醒词=「%s」）—— 请说命令词，%d 秒内有效", s_wake_word,
                         VOICE_SR_MN_DURATION_MS / 1000);
                // 只发事件不做动作
                bsp_voice_notify_wake();
            } else {
                // 续上窗口
                ESP_LOGI(TAG, "再次听到唤醒词「%s」，命令词窗口重新计时", s_wake_word);
            }
            // 清残留防串轮
            s_mn->clean(s_mn_data);
        }

        if (!s_awake) {
            continue;
        }

        const esp_mn_state_t mn_state = s_mn->detect(s_mn_data, res->data);

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            if (r != NULL && r->num > 0) {
                // id 就是枚举值
                const int cid = r->command_id[0];
                // 也不用 %f
                const int prob_x1000 = (int)(r->prob[0] * 1000.0f + 0.5f);

                if (cid > (int)VOICE_CMD_NONE && cid < (int)VOICE_CMD_MAX) {
                    ESP_LOGI(TAG, "命令词命中：「%s」 (conf=%d.%03d) -> %s",
                             bsp_voice_cmd_cn((voice_cmd_t)cid), prob_x1000 / 1000, prob_x1000 % 1000,
                             bsp_voice_cmd_name((voice_cmd_t)cid));
                    // 打的是注册串
                    ESP_LOGI(TAG, "命中短语(注册串): %s", r->string);

                    // 次选候选要留着
                    if (r->num > 1) {
                        for (int i = 1; i < r->num && i < ESP_MN_RESULT_MAX_NUM; i++) {
                            const int alt_id    = r->command_id[i];
                            const int alt_x1000 = (int)(r->prob[i] * 1000.0f + 0.5f);
                            ESP_LOGI(TAG, "  次选候选[%d]: 「%s」 conf=%d.%03d -> %s", i,
                                     (alt_id > (int)VOICE_CMD_NONE && alt_id < (int)VOICE_CMD_MAX)
                                         ? bsp_voice_cmd_cn((voice_cmd_t)alt_id)
                                         : "?",
                                     alt_x1000 / 1000, alt_x1000 % 1000,
                                     (alt_id > (int)VOICE_CMD_NONE && alt_id < (int)VOICE_CMD_MAX)
                                         ? bsp_voice_cmd_name((voice_cmd_t)alt_id)
                                         : "越界");
                        }
                    }

                    // 同 say 链路
                    bsp_voice_dispatch((voice_cmd_t)cid);
                } else {
                    // 越界就不执行
                    ESP_LOGW(TAG, "识别到未知 command_id=%d（应在 1~%d 之间），忽略不执行", cid,
                             (int)VOICE_CMD_MAX - 1);
                }
            } else {
                ESP_LOGW(TAG, "MultiNet 报 DETECTED 但结果为空，忽略本轮");
            }

            // 认完接着听
            s_mn->clean(s_mn_data);

        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "命令词等待超时（%d 秒），回到休眠，需要重新喊「%s」",
                     VOICE_SR_MN_DURATION_MS / 1000, s_wake_word);
            s_awake = false;
        } else {
            // 还在听
        }
    }
}

// 启动顺序不能变
esp_err_t bsp_voice_sr_start(void) {
    if (s_started) {
        ESP_LOGD(TAG, "already started，忽略重复调用");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "========== 启动 ESP-SR 离线识别（INMP441 + WakeNet + MultiNet7）==========");
    ESP_LOGI(TAG, "⚠ INMP441 是 I²S 不是 I²C：SCK/WS/SD 三根信号线 + L/R 接 GND + VDD 3V3");

    // ① 模型清单
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init(\"model\") 失败 —— model 分区里没有模型。下一步这么做：");
        ESP_LOGE(TAG, "  1) 确认 partitions-16MiB.csv 里有 `model` 分区（本工程有，4MB）；");
        ESP_LOGE(TAG, "  2) 用 idf.py flash 烧录：它会把 srmodels.bin 一起烧进 model 分区，"
                      "只烧 app 是【不带模型】的；");
        ESP_LOGE(TAG, "  3) 确认 menuconfig 里选了唤醒词模型与中文命令词模型，"
                      "否则 srmodels.bin 是空的。");
        return ESP_ERR_NOT_FOUND;
    }

    // ② 挑模型
    esp_err_t err = voice_sr_pick_models();
    if (err != ESP_OK) {
        // 日志已在里面
        return err;
    }

    // ③ 麦克风
    err = bsp_voice_mic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "I²S 麦克风初始化失败 (%s)：语音识别不可用，"
                 "但串口注入 / 屏幕 / 手机 MQTT / 自动联动全部照常工作",
                 esp_err_to_name(err));
        return err;
    }

    // ④ 改配置
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init 失败（多半是内部内存/PSRAM 不足或模型不匹配）");
        return ESP_FAIL;
    }

    afe_cfg->wakenet_init = true;
    // 必须指定模型名
    afe_cfg->wakenet_model_name = s_wn_name;
    // 误唤醒多换 95
    afe_cfg->wakenet_mode = DET_MODE_90;
    // 没喇叭关 AEC
    afe_cfg->aec_init = false;
    // 单麦关波束
    afe_cfg->se_init = false;
    // 降噪会吃掉语音
    afe_cfg->ns_init = false;
    // VAD 是前提
    afe_cfg->vad_init = true;
    // 判最松别切小声
    afe_cfg->vad_mode = VAD_MODE_0;
    afe_cfg->afe_mode = AFE_MODE_HIGH_PERF;
    // 大块放 PSRAM
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    // 与识别任务同核
    afe_cfg->afe_perferred_core     = VOICE_SR_TASK_CORE;
    afe_cfg->afe_perferred_priority = VOICE_SR_TASK_PRIO;
    // 增益见宏注释
    afe_cfg->afe_linear_gain = VOICE_SR_AFE_GAIN;

    // 以打印的配置为准
    afe_config_check(afe_cfg);
    afe_config_print(afe_cfg);

    s_afe = esp_afe_handle_from_config(afe_cfg);
    if (s_afe == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config 返回 NULL（AFE 类型/模式不支持？）");
        afe_config_free(afe_cfg);
        return ESP_FAIL;
    }

    s_afe_data = s_afe->create_from_config(afe_cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE 创建失败（多半是 PSRAM 不足）：确认 sdkconfig 里"
                      "CONFIG_SPIRAM=y 且 CONFIG_SPIRAM_MODE_OCT=y");
        afe_config_free(afe_cfg);
        s_afe = NULL;
        return ESP_ERR_NO_MEM;
    }

    // ⑤ 要 free
    afe_config_free(afe_cfg);
    afe_cfg = NULL;

    // ⑥ 喂帧缓冲
    s_feed_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    if (s_feed_chunksize <= 0) {
        ESP_LOGE(TAG, "AFE 报出的 feed_chunksize=%d 非法，放弃启动", s_feed_chunksize);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE 就绪：feed=%d 点/帧（%d ms）/ fetch=%d 点/帧 / %dHz / %d 通道 / 目标 %d 帧每秒",
             s_feed_chunksize, VOICE_SR_SAMPLES_TO_MS(s_feed_chunksize),
             s_afe->get_fetch_chunksize(s_afe_data), s_afe->get_samp_rate(s_afe_data),
             s_afe->get_channel_num(s_afe_data), BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize);
    s_afe->print_pipeline(s_afe_data);

    // 放 PSRAM
    s_feed_buf = (int16_t *)heap_caps_malloc((size_t)s_feed_chunksize * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_feed_buf == NULL) {
        // 退回内部堆
        s_feed_buf = (int16_t *)malloc((size_t)s_feed_chunksize * sizeof(int16_t));
    }
    if (s_feed_buf == NULL) {
        ESP_LOGE(TAG, "喂帧缓冲分配失败（需要 %d 字节）", (int)((size_t)s_feed_chunksize * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    // ⑦ 命令词引擎
    s_mn = esp_mn_handle_from_name(s_mn_name);
    if (s_mn == NULL) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name(\"%s\") 失败：该模型名不被 MultiNet 支持", s_mn_name);
        return ESP_FAIL;
    }

    s_mn_data = s_mn->create(s_mn_name, VOICE_SR_MN_DURATION_MS);
    if (s_mn_data == NULL) {
        ESP_LOGE(TAG, "MultiNet 实例创建失败（多半是内存/PSRAM 不足，窗口 %d ms）", VOICE_SR_MN_DURATION_MS);
        return ESP_ERR_NO_MEM;
    }

    // ⑧ 注册命令词
    err = voice_sr_register_commands();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "命令词注册失败：识别引擎起来了但认不出命令，"
                      "串口注入与其它功能照常");
        return err;
    }

    // ⑨ 先喂帧后识别
    if (xTaskCreatePinnedToCore(voice_sr_feed_task, "voice_feed", VOICE_SR_FEED_STACK, NULL,
                                VOICE_SR_FEED_PRIO, &s_feed_task_h, VOICE_SR_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建喂帧任务失败（栈 %d 字节）：内部内存不足？", VOICE_SR_FEED_STACK);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(voice_sr_task, "voice_sr", VOICE_SR_TASK_STACK, NULL, VOICE_SR_TASK_PRIO,
                                NULL, VOICE_SR_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建识别任务失败（栈 %d 字节）：内部内存不足？", VOICE_SR_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    s_ready   = true;

    ESP_LOGI(TAG, "========== ESP-SR 启动成功 ==========");
    ESP_LOGI(TAG, "★ 请喊「%s」，然后说命令词（例如「打开客厅灯」）", s_wake_word);
    ESP_LOGI(TAG, "   完整命令表：串口敲 help-voice（会调用 bsp_voice_sr_print_commands）");
    ESP_LOGI(TAG, "   剩余内部堆 %u 字节 / PSRAM 剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

// 就绪≠麦克风正常
bool bsp_voice_sr_ready(void) {
    return s_ready;
}

// 屏幕轮询这个
bool bsp_voice_sr_awake(void) {
    return s_awake;
}

// 唤醒词文本
const char *bsp_voice_sr_wake_word(void) {
    return (s_wake_word[0] != '\0') ? s_wake_word : "(未启动)";
}

// 命令词模型名
const char *bsp_voice_sr_mn_model(void) {
    return (s_mn_name != NULL) ? s_mn_name : "(未加载)";
}

// 打印命令表
void bsp_voice_sr_print_commands(void) {
    // 句式数≠指令数
    unsigned phrases = 0;
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        for (unsigned k = 0; voice_sr_nth_phrase(s_cmds[i].pinyin, k) != NULL; k++) {
            phrases++;
        }
    }

    printf("\n");
    printf("============ ESP-SR 离线语音识别 —— 命令词表 ============\n");
    printf("  唤醒词： 「%s」（必须先喊它，看到日志『已唤醒』之后再说命令词）\n", bsp_voice_sr_wake_word());
    printf("  唤醒词模型：%s\n", (s_wn_name != NULL) ? s_wn_name : "(未加载)");
    printf("  命令词模型：%s\n", bsp_voice_sr_mn_model());
    printf("  识别状态：%s\n", s_ready ? "已就绪" : "未就绪（看日志里 voice.sr 的报错）");
    printf("  音频通路：INMP441(I²S%d) SCK=GPIO%d WS=GPIO%d SD=GPIO%d @%dHz 单声道\n", (int)BSP_I2S_MIC_PORT,
           (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO,
           BSP_I2S_MIC_SAMPLE_RATE);
    printf("  规模：%u 条指令 / %u 个口语句式（同一条指令可以有多种说法）\n", (unsigned)VOICE_SR_CMD_NUM,
           phrases);
    printf("-------------------------------------------------------\n");
    printf("  %-4s %-10s %-30s %s\n", "序号", "中文命令词", "拼音(实际识别单元)", "对应指令");
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        // 每种说法一行
        unsigned    k  = 0;
        const char *ph = voice_sr_nth_phrase(s_cmds[i].pinyin, k);
        for (; ph != NULL; ph = voice_sr_nth_phrase(s_cmds[i].pinyin, ++k)) {
            if (k == 0) {
                printf("  %-4u %-10s %-30s %s\n", (unsigned)(i + 1), s_cmds[i].cn, ph,
                       bsp_voice_cmd_name(s_cmds[i].cmd));
            } else {
                printf("  %-4s %-10s %-30s %s\n", "", "", ph, "↳ 同一指令的另一种说法");
            }
        }
    }
    printf("-------------------------------------------------------\n");
    printf("  ⚠ 本方案只有麦克风、没有喇叭：控制类指令正常执行；\n");
    printf("    查询类指令（温度多少 / 温湿度多少 / 状态如何…）会走完业务链路但【不会出声】。\n");
    printf("  ⚠ 命令词靠拼音识别：改说法必须同步改 s_cmds[] 里的拼音，只改中文无效。\n");
    printf("  ⚠ 需求①：说「开启自动模式」或「手动模式」→ 模式立即切换，"
           "屏幕顶栏徽章同步显示「自动 / 手动」。\n");
    printf("=======================================================\n\n");
}
