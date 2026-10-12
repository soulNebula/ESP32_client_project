// c
// 四行排版与刷新
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "board_config.h"
#include "panel.h"
#include "panel_font.h"
#include "display_view.h"

static const char *TAG = "VIEW";

#define SCR_W 128
#define SCR_H 64

_Static_assert(PANEL_EXPECT_W == 128 && PANEL_EXPECT_H == 64,
               "display_view.c 的排版是按 128x64 写的，与 panel.c 不一致");

// 汉字 16px
#define ROW_H         16
#define ROW0_Y        0
#define ROW_WEATHER_Y 16
#define ROW_TEMP_Y    32
#define ROW_LIGHT_Y   48

#define ROW1_Y ROW_WEATHER_Y
#define ROW2_Y ROW_TEMP_Y
#define ROW3_Y ROW_LIGHT_Y

#define ROW_ERASE_H ROW_H

// 反白块 34px
#define MODE_BADGE_W 34
#define MODE_BADGE_H 16

// 与模式块同宽
#define DOOR_BADGE_W 34

_Static_assert(PANEL_FONT_SMALL_ACTIVE == 0,
               "display_view.c 的行距是按 16px 大字算的；"
               "要把 panel.c 的 PANEL_FONT_SMALL 改成 1，必须同时重排本文件的行坐标");

_Static_assert((ROW3_Y + ROW_H) <= PANEL_EXPECT_H, "最后一行超出了屏幕高度：调小 ROW3_Y 或者压缩行距");

#define VIEW_RAIN_TXT_ON "天气: Raining!"
// 没雨 = Sunny（英文，与 Raining! 对称；且"晴"字不在字库里）
#define VIEW_RAIN_TXT_OFF "天气: Sunny"

#define VIEW_CN_CELL_W    16
#define VIEW_ASCII_CELL_W 8

#define VIEW_CN_LABEL_W (2 * VIEW_CN_CELL_W + 2 * VIEW_ASCII_CELL_W)
#define VIEW_CN_LABEL_BYTES 8

#define VIEW_RAIN_STATUS_W(s) ((int)(sizeof(s) - 1u - VIEW_CN_LABEL_BYTES) * VIEW_ASCII_CELL_W)
#define VIEW_RAIN_SPAN_W(s)   (VIEW_CN_LABEL_W + VIEW_RAIN_STATUS_W(s))
#define VIEW_RAIN_MAX_W       SCR_W

// 按字节算偏大
_Static_assert(VIEW_RAIN_SPAN_W(VIEW_RAIN_TXT_ON) <= VIEW_RAIN_MAX_W,
               "天气行 '天气: Raining!' 超过屏宽 128px，会被裁掉："
               "要么缩短英文状态，要么改 panel.c 的字号");
_Static_assert(VIEW_RAIN_SPAN_W(VIEW_RAIN_TXT_OFF) <= VIEW_RAIN_MAX_W,
               "天气行 '天气: Sunny' 超过屏宽 128px，会被裁掉");

_Static_assert(CONFIG_APP_DISPLAY_REFRESH_MS == 3000,
               "需求②要求每 3 秒刷新：请把 APP_DISPLAY_REFRESH_MS 保持 3000，"
               "或在改需求后一并更新这条断言与本文件的注释");

// 定期整帧重画
#define VIEW_REFRESH_MS CONFIG_APP_DISPLAY_REFRESH_MS

static bool         s_hw_ok   = false;
static bool         s_started = false;
static TaskHandle_t s_task    = NULL;
static SemaphoreHandle_t s_lock = NULL;

// 提交的最新数据
static display_view_input_t s_pending;
static bool s_have = false;

typedef struct {
    // ×10 避开浮点
    int    temp_x10;
    int    hum_x10;
    int    light_x10;
    int    rain_x10;
    bool   th_valid;
    bool   rain_valid;
    bool   light_valid;
    bool   light_is_lux;
    bool   raining;
    bool   auto_mode;
    bool   door_open;
    bool   buzzer_on;
    bool   wifi_ok;
    bool   mqtt_ok;
    bool   light_on[VIEW_LIGHT_COUNT];
    char   ip[16];
    int8_t rssi;
} view_digest_t;

// 上次的摘要
static view_digest_t s_last;

// 置上就强刷
static volatile bool s_force = true;

// 反馈条窗口
static uint32_t s_voice_tick_seen = 0;
static int64_t  s_voice_until_ms  = 0;
// 上一拍在不在
static bool s_voice_showing = false;

// 屏没起来自动重试
#define VIEW_PANEL_RETRY_GAP_MS 10000
#define VIEW_PANEL_RETRY_MAX    6
static int s_panel_retry = 0;

// 转 ×10 整数
static int f2i10(float v) {
    // NaN 自比不等
    if (v != v) {
        return -100000;
    }
    if (v > 10000.0f) {
        return 100000;
    }
    if (v < -10000.0f) {
        return -100000;
    }
    return (int)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

static void make_digest(const display_view_input_t *in, view_digest_t *d) {
    memset(d, 0, sizeof(*d));
    d->temp_x10     = f2i10(in->temperature);
    d->hum_x10      = f2i10(in->humidity);
    d->light_x10    = f2i10(in->light_pct);
    d->rain_x10     = f2i10(in->rain_pct);
    d->th_valid     = in->th_valid;
    d->rain_valid   = in->rain_valid;
    d->light_valid  = in->light_valid;
    d->light_is_lux = in->light_is_lux;
    d->raining      = in->raining;
    d->auto_mode    = in->auto_mode;
    d->door_open    = in->door_open;
    d->buzzer_on    = in->buzzer_on;
    d->wifi_ok      = in->wifi_ok;
    d->mqtt_ok      = in->mqtt_ok;
    d->rssi         = in->rssi;
    for (int i = 0; i < VIEW_LIGHT_COUNT; i++) {
        d->light_on[i] = in->light_on[i];
    }
    // IP 只比对不显示
    strncpy(d->ip, in->ip, sizeof(d->ip) - 1);
    d->ip[sizeof(d->ip) - 1] = '\0';
}

static bool digest_same(const view_digest_t *a, const view_digest_t *b) {
    if (memcmp(a, b, sizeof(*a)) == 0) {
        return true;
    }
    return false;
}

static int view_text_w(const char *s) {
    return panel_text_width(s);
}

// 先擦再画
static void view_erase_row(int y, int h) {
    panel_fill_rect(0, y, SCR_W, h, PANEL_BLACK);
}

static bool view_blink_phase(void);

// 闪烁相位
static bool view_blink_phase(void) {
    return ((xTaskGetTickCount() / pdMS_TO_TICKS(500)) & 1u) != 0u;
}

static void view_render_topbar(const display_view_input_t *in) {
    view_erase_row(ROW0_Y, ROW_ERASE_H);

    const char *mode = in->auto_mode ? "自动" : "手动";
    panel_fill_rect(0, ROW0_Y, MODE_BADGE_W, MODE_BADGE_H, PANEL_WHITE);
    panel_draw_text_cn(1, ROW0_Y, mode, PANEL_BLACK, PANEL_TRANSPARENT);

    const int dx = MODE_BADGE_W + 2;
    if (in->door_open) {
        panel_fill_rect(dx - 1, ROW0_Y, DOOR_BADGE_W, MODE_BADGE_H, PANEL_WHITE);
        panel_draw_text_cn(dx, ROW0_Y, "门开", PANEL_BLACK, PANEL_TRANSPARENT);
    } else {
        panel_draw_text_cn(dx, ROW0_Y, "门关", PANEL_WHITE, PANEL_TRANSPARENT);
    }

    // W 档=信号强弱
    char wbuf[8];
    if (!in->wifi_ok) {
        snprintf(wbuf, sizeof(wbuf), "W-");
    } else if (!in->mqtt_ok) {
        snprintf(wbuf, sizeof(wbuf), "Wn");
    } else {
        int sig = 1;
        if (in->rssi >= -55) {
            sig = 4;
        } else if (in->rssi >= -65) {
            sig = 3;
        } else if (in->rssi >= -75) {
            sig = 2;
        }
        snprintf(wbuf, sizeof(wbuf), "W%d", sig);
    }
    panel_draw_text(SCR_W - view_text_w(wbuf), ROW0_Y, wbuf, PANEL_WHITE, PANEL_TRANSPARENT);
}

static void view_render_weather(const display_view_input_t *in) {
    view_erase_row(ROW_WEATHER_Y, ROW_ERASE_H);

    const char *rain_txt;
    bool        rain_invert = false;
    if (in->rain_valid && in->raining) {
        rain_txt    = VIEW_RAIN_TXT_ON;
        rain_invert = true;
    } else {
        // 读数不可信也算晴
        rain_txt = VIEW_RAIN_TXT_OFF;
    }

    const int w = view_text_w(rain_txt);
    const int x = (SCR_W - w) / 2;

    if (rain_invert) {
        panel_fill_rect(0, ROW_WEATHER_Y, SCR_W, ROW_H, PANEL_WHITE);
        panel_draw_text_cn(x, ROW_WEATHER_Y, rain_txt, PANEL_BLACK, PANEL_TRANSPARENT);
    } else {
        panel_draw_text_cn(x, ROW_WEATHER_Y, rain_txt, PANEL_WHITE, PANEL_TRANSPARENT);
    }
}

static void view_render_temp_hum(const display_view_input_t *in) {
    view_erase_row(ROW_TEMP_Y, ROW_ERASE_H);

    // 阈值没填就不告警
    const bool alert = in->th_valid && (in->temp_alert_c > 0.0f) && (in->temperature >= in->temp_alert_c);

    const bool phase  = view_blink_phase();
    const bool invert = alert && phase;
    // 只藏数字不藏湿度
    const bool hide_temp = alert && !phase;

    if (invert) {
        panel_fill_rect(0, ROW_TEMP_Y, SCR_W, ROW_H, PANEL_WHITE);
    }
    const uint16_t fg = invert ? PANEL_BLACK : PANEL_WHITE;

    char tbuf[24];
    if (!in->th_valid) {
        // 不可信不显示
        snprintf(tbuf, sizeof(tbuf), "--");
    } else {
        // 用 C 省 8px
        snprintf(tbuf, sizeof(tbuf), "%.1fC", (double)in->temperature);
    }

    char        blank[sizeof(tbuf)];
    const char *tdraw = tbuf;
    if (hide_temp) {
        // 用空格顶掉数字
        size_t i = 0;
        for (; tbuf[i] != '\0' && i < sizeof(blank) - 1u; i++) {
            blank[i] = ' ';
        }
        blank[i] = '\0';
        tdraw    = blank;
    }
    int x = panel_draw_text(0, ROW_TEMP_Y, tdraw, fg, PANEL_TRANSPARENT);

    char hbuf[24];
    if (!in->th_valid) {
        snprintf(hbuf, sizeof(hbuf), " --");
    } else {
        snprintf(hbuf, sizeof(hbuf), " %d%%", (int)(in->humidity + 0.5f));
    }
    const int hw = view_text_w(hbuf);
    int       hx = SCR_W - hw;
    if (hx < x + 8) {
        // 别和温度贴在一起
        hx = x + 8;
    }
    panel_draw_text(hx, ROW_TEMP_Y, hbuf, fg, PANEL_TRANSPARENT);

    // 光照挤在湿度左边
    char lbuf[24];
    if (!in->light_valid) {
        snprintf(lbuf, sizeof(lbuf), "L--");
    } else if (in->light_is_lux) {
        // 真实 lux
        snprintf(lbuf, sizeof(lbuf), "L%d", (int)(in->light_lux + 0.5f));
    } else {
        // 光敏等效百分比
        snprintf(lbuf, sizeof(lbuf), "L%d%%", (int)(in->light_pct + 0.5f));
    }
    const int lw = view_text_w(lbuf);
    int lx = hx - 8 - lw;
    if (lx < x + 8) {
        // 挤了宁可挨着
        lx = x + 8;
    }
    panel_draw_text(lx, ROW_TEMP_Y, lbuf, fg, PANEL_TRANSPARENT);
}

static void view_render_lights(const display_view_input_t *in) {
    view_erase_row(ROW_LIGHT_Y, ROW_ERASE_H);

    static const char *const kRoomNames[VIEW_LIGHT_COUNT] = {"客", "厨", "卧", "浴"};
    const int cell = SCR_W / VIEW_LIGHT_COUNT;

    for (int i = 0; i < VIEW_LIGHT_COUNT; i++) {
        const int   x  = i * cell;
        const bool  on = in->light_on[i];
        const char *st = on ? "开" : "关";

        if (on) {
            panel_fill_rect(x, ROW_LIGHT_Y, cell, ROW_H, PANEL_WHITE);
            panel_draw_text_cn(x + 1, ROW_LIGHT_Y, kRoomNames[i], PANEL_BLACK, PANEL_TRANSPARENT);
            panel_draw_text_cn(x + 17, ROW_LIGHT_Y, st, PANEL_BLACK, PANEL_TRANSPARENT);
        } else {
            panel_draw_text_cn(x + 1, ROW_LIGHT_Y, kRoomNames[i], PANEL_WHITE, PANEL_TRANSPARENT);
            panel_draw_text_cn(x + 17, ROW_LIGHT_Y, st, PANEL_WHITE, PANEL_TRANSPARENT);
        }
    }
}

// 别在这画东西
static void view_render_status(const display_view_input_t *in) {
    (void)in;
}

static void view_render_chrome(void) {
}

// 截断要落在字符边界
static void view_draw_text_fit(int x, int y, int w, int h, const char *utf8, uint16_t fg, uint16_t bg) {
    if (utf8 == NULL || utf8[0] == '\0') {
        return;
    }

    if (panel_text_width(utf8) <= w) {
        panel_draw_text_cn_center(x, y, w, h, utf8, fg, bg);
        return;
    }

    char         buf[48];
    const size_t n = strlen(utf8);
    for (size_t cut = n; cut > 0u; cut--) {
        const unsigned char c = (unsigned char)utf8[cut];
        if ((c & 0xC0u) == 0x80u) {
            continue;
        }
        snprintf(buf, sizeof(buf), "%.*s..", (int)cut, utf8);
        if (panel_text_width(buf) <= w) {
            panel_draw_text_cn_center(x, y, w, h, buf, fg, bg);
            return;
        }
    }

    // 放不下就不画
}

// 首行反白
static void view_render_voice_banner(const display_view_input_t *in) {
    panel_fill_rect(0, ROW1_Y, SCR_W, ROW_H * 2, PANEL_BLACK);

    panel_fill_rect(0, ROW1_Y, SCR_W, ROW_H, PANEL_WHITE);
    view_draw_text_fit(0, ROW1_Y, SCR_W, ROW_H, in->voice_line1, PANEL_BLACK, PANEL_TRANSPARENT);

    view_draw_text_fit(0, ROW2_Y, SCR_W, ROW_H, in->voice_line2, PANEL_WHITE, PANEL_TRANSPARENT);
}

// 画一整帧
static void view_render_snapshot(const display_view_input_t *in, bool full, bool voice_on) {
    panel_begin_frame();

    if (full) {
        panel_clear(PANEL_BLACK);
        view_render_chrome();
    }

    view_render_topbar(in);
    view_render_weather(in);

    if (voice_on) {
        view_render_temp_hum(in);
        view_render_lights(in);
        view_render_voice_banner(in);
    } else {
        view_render_temp_hum(in);
        view_render_lights(in);
    }
    view_render_status(in);

    panel_end_frame();
}

// 刷屏任务节拍
// 先更摘要再重画
static void view_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "view task started, tick=500ms, 数据刷新周期=%dms（需求②）", CONFIG_APP_DISPLAY_REFRESH_MS);

    bool last_blink = view_blink_phase();
    bool first      = true;
    int64_t last_retry_ms = 0;
    int64_t last_refresh_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!s_hw_ok) {
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (s_panel_retry < VIEW_PANEL_RETRY_MAX && (now_ms - last_retry_ms) >= VIEW_PANEL_RETRY_GAP_MS) {
                last_retry_ms = now_ms;
                s_panel_retry++;

                // 别直接调 init
                if (display_view_retry_panel() == ESP_OK) {
                    last_blink = view_blink_phase();
                    ESP_LOGI(TAG, "（这是第 %d 次自动重试，屏接入后无需复位）", s_panel_retry);
                } else if (s_panel_retry == VIEW_PANEL_RETRY_MAX) {
                    ESP_LOGW(TAG,
                             "屏自动重试 %d 次仍无应答，停止重试 —— "
                             "接好线后敲串口命令 panel-retry 即可（不用复位）",
                             VIEW_PANEL_RETRY_MAX);
                }
            }
            continue;
        }

        display_view_input_t in;
        bool                 have = false;
        if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            in   = s_pending;
            have = s_have;
            xSemaphoreGive(s_lock);
        }

        const bool blink_now  = view_blink_phase();
        const bool blink_edge = (blink_now != last_blink);
        last_blink            = blink_now;

        view_digest_t d;
        make_digest(&in, &d);

        // 新指令就重新计时
        const int64_t now_ms    = esp_timer_get_time() / 1000;
        const bool    voice_new = have && (in.voice_tick != s_voice_tick_seen);
        if (voice_new) {
            s_voice_tick_seen = in.voice_tick;
            s_voice_until_ms  = now_ms + DISPLAY_VIEW_VOICE_MS;
            ESP_LOGI(TAG, "语音反馈条: [%s] %s（显示 %d ms）", in.voice_line1, in.voice_line2,
                     DISPLAY_VIEW_VOICE_MS);
        }
        const bool voice_on   = (s_voice_tick_seen != 0u) && (now_ms < s_voice_until_ms);
        const bool voice_edge = (voice_on != s_voice_showing);
        s_voice_showing       = voice_on;

        const bool full         = s_force || first;
        const bool data_changed = !digest_same(&d, &s_last);
        const bool alert =
            have && in.th_valid && (in.temp_alert_c > 0.0f) && (in.temperature >= in.temp_alert_c);

        // 只在有数据时推进
        const bool throttled_now = (now_ms - last_refresh_ms) >= VIEW_REFRESH_MS;
        if (throttled_now && have) {
            last_refresh_ms = now_ms;
        }

        if (!full && !data_changed && !voice_edge && !(alert && blink_edge) && !(throttled_now && have)) {
            continue;
        }

        s_force = false;
        first   = false;

        if (full || data_changed || voice_edge) {
            view_render_snapshot(&in, full, voice_on);
        } else if (voice_on) {
            // 条子下不重画温湿度
        } else if (alert && blink_edge) {
            // 只重画温湿度行
            panel_begin_frame();
            view_render_temp_hum(&in);
            panel_end_frame();
        } else {
            view_render_snapshot(&in, false, voice_on);
        }

        s_last = d;
    }
}

// 起屏可选起任务
static esp_err_t display_view_init_internal(bool want_task) {
    if (s_started) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (!panel_ready()) {
        err = panel_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel_init failed (%s) —— 屏幕不可用，业务继续运行", esp_err_to_name(err));
        s_hw_ok = false;
    } else {
        s_hw_ok = true;
    }

    // 默认按不可信画
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.temp_alert_c  = 32.0f;
    s_pending.light_on_pct  = 20.0f;
    s_pending.light_off_pct = 45.0f;
    s_pending.buzzer_on_c   = 33.0f;
    s_pending.th_valid      = false;
    s_pending.rain_valid    = false;
    s_pending.light_valid   = false;
    s_have                  = false;
    s_force                 = true;
    memset(&s_last, 0, sizeof(s_last));

    const int pw = panel_width();
    const int ph = panel_height();
    if (pw != SCR_W || ph != SCR_H) {
        ESP_LOGW(TAG, "★ 尺寸不一致：排版按 %dx%d，panel 报告 %dx%d —— 画面会错位", SCR_W, SCR_H, pw, ph);
    }

    if (s_hw_ok) {
        // 首帧不弹反馈条
        panel_clear(PANEL_BLACK);
        view_render_snapshot(&s_pending, true, false);
        ESP_LOGI(TAG, "init ok: SSD1306 %dx%d I2C0 (SDA=GPIO%d SCL=GPIO%d)", pw, ph, (int)BSP_I2C_SDA_GPIO,
                 (int)BSP_I2C_SCL_GPIO);
        ESP_LOGI(TAG, "中文字库共 %d 字（ready=%d）", panel_font_cn_count(), (int)panel_font_ready());
    } else {
        ESP_LOGW(TAG, "屏不可用：只维护数据，不推屏");
    }

    s_started = true;

    if (want_task) {
        if (xTaskCreate(view_task, "view", 4096, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "view task create failed");
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGI(TAG, "屏已就绪，刷屏任务暂不启动（等开机自检用完屏幕）");
    }

    return ESP_OK;
}

// 只初始化不起任务
esp_err_t display_view_init_no_task(void) {
    return display_view_init_internal(false);
}

esp_err_t display_view_init(void) {
    return display_view_init_internal(true);
}

esp_err_t display_view_start(void) {
    if (!s_started) {
        ESP_LOGE(TAG, "display_view_start() 在 init 之前被调用");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (!s_hw_ok) {
        ESP_LOGW(TAG, "屏不可用，仍启动任务（只维护数据）");
    }

    if (xTaskCreate(view_task, "view", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "view task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void display_view_update(const display_view_input_t *in) {
    if (in == NULL || s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_pending = *in;
        s_have    = true;
        xSemaphoreGive(s_lock);
    }
}

void display_view_get_input(display_view_input_t *out) {
    if (out == NULL || s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_pending;
        xSemaphoreGive(s_lock);
    }
}

void display_view_force_redraw(void) {
    s_force = true;
}

void display_view_set_thresholds(float light_on_pct, float light_off_pct, float buzzer_on_c) {
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_pending.light_on_pct  = light_on_pct;
        s_pending.light_off_pct = light_off_pct;
        s_pending.buzzer_on_c   = buzzer_on_c;
        xSemaphoreGive(s_lock);
    }
    s_force = true;
}

// 重新初始化并接管
esp_err_t display_view_retry_panel(void) {
    const esp_err_t err = panel_init();
    if (err != ESP_OK) {
        // 失败别改状态
        return err;
    }

    s_hw_ok = true;
    // 强制补一帧
    s_force       = true;
    s_panel_retry = 0;
    ESP_LOGI(TAG, "★ 屏已就绪（重新初始化成功）：刷屏任务接管，下面由业务界面铺屏");
    return ESP_OK;
}

bool display_view_ready(void) {
    return s_started && s_hw_ok && panel_ready();
}
