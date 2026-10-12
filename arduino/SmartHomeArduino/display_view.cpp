// c
// 四行排版与刷新

#include <stdio.h>
#include <string.h>

#include "display_view.h"
#include "panel.h"
#include "panel_text.h"

#include "app_config.h"
#include "bsp_config.h"

static const char *TAG = "VIEW";

#define SCR_W BSP_PANEL_WIDTH
#define SCR_H BSP_PANEL_HEIGHT

// 汉字 16px
#define ROW_H   16
#define ROW0_Y  0
#define ROW1_Y  16
#define ROW2_Y  32
#define ROW3_Y  48

// 反白块 34px
#define MODE_BADGE_W 34
#define MODE_BADGE_H 16

// 与模式块同宽
#define DOOR_BADGE_W 34

#define VIEW_RAIN_TXT_ON  "天气: Raining!"
#define VIEW_RAIN_TXT_OFF "天气: Sunny"

// 上次的摘要
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

static bool                  s_started;
static bool                  s_hw_ok;
static display_view_input_t  s_pending;
static bool                  s_have;
static view_digest_t         s_last;

// 置上就强刷
static volatile bool s_force = true;

// 反馈条窗口
static uint32_t s_voice_tick_seen;
static uint32_t s_voice_until_ms;
// 上一拍在不在
static bool s_voice_showing;

// 屏没起来自动重试
#define VIEW_PANEL_RETRY_GAP_MS 10000
#define VIEW_PANEL_RETRY_MAX    6
static int     s_panel_retry;
static uint32_t s_last_retry_ms;
static uint32_t s_last_tick_ms;
static uint32_t s_last_refresh_ms;

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
    snprintf(d->ip, sizeof(d->ip), "%s", in->ip);
}

static bool digest_same(const view_digest_t *a, const view_digest_t *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

// 先擦再画
static void view_erase_row(int y, int h) {
    panel_fill_rect(0, y, SCR_W, h, PANEL_BLACK);
}

// 闪烁相位
static bool view_blink_phase(void) {
    return ((app_millis() / 500u) & 1u) != 0u;
}

static void view_render_topbar(const display_view_input_t *in) {
    view_erase_row(ROW0_Y, ROW_H);

    const char *mode = in->auto_mode ? "自动" : "手动";
    panel_fill_rect(0, ROW0_Y, MODE_BADGE_W, MODE_BADGE_H, PANEL_WHITE);
    (void)panel_draw_text_cn(1, ROW0_Y, mode, PANEL_BLACK, PANEL_TRANSPARENT);

    const int dx = MODE_BADGE_W + 2;
    if (in->door_open) {
        panel_fill_rect(dx - 1, ROW0_Y, DOOR_BADGE_W, MODE_BADGE_H, PANEL_WHITE);
        (void)panel_draw_text_cn(dx, ROW0_Y, "门开", PANEL_BLACK, PANEL_TRANSPARENT);
    } else {
        (void)panel_draw_text_cn(dx, ROW0_Y, "门关", PANEL_WHITE, PANEL_TRANSPARENT);
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
    (void)panel_draw_text(SCR_W - panel_text_width(wbuf), ROW0_Y, wbuf, PANEL_WHITE, PANEL_TRANSPARENT);
}

static void view_render_weather(const display_view_input_t *in) {
    view_erase_row(ROW1_Y, ROW_H);

    const char *rain_txt;
    bool        rain_invert = false;
    if (in->rain_valid && in->raining) {
        rain_txt    = VIEW_RAIN_TXT_ON;
        rain_invert = true;
    } else {
        // 读数不可信也算晴
        rain_txt = VIEW_RAIN_TXT_OFF;
    }

    const int w = panel_text_width(rain_txt);
    const int x = (SCR_W - w) / 2;

    if (rain_invert) {
        panel_fill_rect(0, ROW1_Y, SCR_W, ROW_H, PANEL_WHITE);
        (void)panel_draw_text_cn(x, ROW1_Y, rain_txt, PANEL_BLACK, PANEL_TRANSPARENT);
    } else {
        (void)panel_draw_text_cn(x, ROW1_Y, rain_txt, PANEL_WHITE, PANEL_TRANSPARENT);
    }
}

static void view_render_temp_hum(const display_view_input_t *in) {
    view_erase_row(ROW2_Y, ROW_H);

    // 阈值没填就不告警
    const bool alert = in->th_valid && (in->temp_alert_c > 0.0f) && (in->temperature >= in->temp_alert_c);

    const bool phase  = view_blink_phase();
    const bool invert = alert && phase;
    // 只藏数字不藏湿度
    const bool hide_temp = alert && !phase;

    if (invert) {
        panel_fill_rect(0, ROW2_Y, SCR_W, ROW_H, PANEL_WHITE);
    }
    const int fg = invert ? PANEL_BLACK : PANEL_WHITE;

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
    int x = panel_draw_text(0, ROW2_Y, tdraw, fg, PANEL_TRANSPARENT);

    char hbuf[24];
    if (!in->th_valid) {
        snprintf(hbuf, sizeof(hbuf), " --");
    } else {
        snprintf(hbuf, sizeof(hbuf), " %d%%", (int)(in->humidity + 0.5f));
    }
    const int hw = panel_text_width(hbuf);
    int       hx = SCR_W - hw;
    if (hx < x + 8) {
        // 别和温度贴在一起
        hx = x + 8;
    }
    (void)panel_draw_text(hx, ROW2_Y, hbuf, fg, PANEL_TRANSPARENT);

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
    const int lw = panel_text_width(lbuf);
    int       lx = hx - 8 - lw;
    if (lx < x + 8) {
        // 挤了宁可挨着
        lx = x + 8;
    }
    (void)panel_draw_text(lx, ROW2_Y, lbuf, fg, PANEL_TRANSPARENT);
}

static void view_render_lights(const display_view_input_t *in) {
    view_erase_row(ROW3_Y, ROW_H);

    static const char *const kRoomNames[VIEW_LIGHT_COUNT] = {"客", "厨", "卧", "浴"};
    const int cell = SCR_W / VIEW_LIGHT_COUNT;

    for (int i = 0; i < VIEW_LIGHT_COUNT; i++) {
        const int   x  = i * cell;
        const bool  on = in->light_on[i];
        const char *st = on ? "开" : "关";

        if (on) {
            panel_fill_rect(x, ROW3_Y, cell, ROW_H, PANEL_WHITE);
            (void)panel_draw_text_cn(x + 1, ROW3_Y, kRoomNames[i], PANEL_BLACK, PANEL_TRANSPARENT);
            (void)panel_draw_text_cn(x + 17, ROW3_Y, st, PANEL_BLACK, PANEL_TRANSPARENT);
        } else {
            (void)panel_draw_text_cn(x + 1, ROW3_Y, kRoomNames[i], PANEL_WHITE, PANEL_TRANSPARENT);
            (void)panel_draw_text_cn(x + 17, ROW3_Y, st, PANEL_WHITE, PANEL_TRANSPARENT);
        }
    }
}

// 截断要落在字符边界
static void view_draw_text_fit(int x, int y, int w, int h, const char *utf8, int fg, int bg) {
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
    }

    view_render_topbar(in);
    view_render_weather(in);
    view_render_temp_hum(in);
    view_render_lights(in);

    if (voice_on) {
        // 压在下面两行上
        view_render_voice_banner(in);
    }

    panel_end_frame();
}

// 起屏
app_err_t display_view_init(void) {
    if (s_started) {
        return APP_OK;
    }

    if (!panel_ready()) {
        s_hw_ok = (panel_init() == APP_OK);
    } else {
        s_hw_ok = true;
    }

    // 默认按不可信画
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.temp_alert_c  = BSP_DEF_TEMP_ALERT_C;
    s_pending.light_on_pct  = BSP_DEF_LIGHT_ON_PCT;
    s_pending.light_off_pct = BSP_DEF_LIGHT_OFF_PCT;
    s_pending.buzzer_on_c   = BSP_DEF_BUZZER_ON_C;
    s_pending.th_valid      = false;
    s_pending.rain_valid    = false;
    s_pending.light_valid   = false;
    s_have                  = false;
    s_force                 = true;
    memset(&s_last, 0, sizeof(s_last));

    s_started = true;

    if (s_hw_ok) {
        // 首帧不弹反馈条
        view_render_snapshot(&s_pending, true, false);
        LOGI(TAG, "init ok: SSD1306 %dx%d（SDA=GPIO%d SCL=GPIO%d）", SCR_W, SCR_H, BSP_I2C_SDA_GPIO,
             BSP_I2C_SCL_GPIO);
    } else {
        LOGW(TAG, "屏不可用：只维护数据，不推屏（配件没到也能跑）");
    }

    return APP_OK;
}

// 提交一帧数据
void display_view_update(const display_view_input_t *in) {
    if (in == NULL) {
        return;
    }
    s_pending = *in;
    s_have    = true;
}

void display_view_get_input(display_view_input_t *out) {
    if (out == NULL) {
        return;
    }
    *out = s_pending;
}

// 立刻重画一帧
void display_view_force_redraw(void) {
    s_force = true;
}

// 屏掉了重来
app_err_t display_view_retry_panel(void) {
    const app_err_t err = panel_retry();
    if (err != APP_OK) {
        // 失败别改状态
        return err;
    }

    s_hw_ok       = true;
    // 强制补一帧
    s_force       = true;
    s_panel_retry = 0;
    LOGI(TAG, "★ 屏已就绪（重新初始化成功）：下面由业务界面铺屏");
    return APP_OK;
}

bool display_view_ready(void) {
    return s_started && s_hw_ok && panel_ready();
}

// 主循环里轮询，到点才推屏
void display_view_tick(void) {
    if (!s_started) {
        return;
    }

    const uint32_t now = app_millis();

    // 500ms 一拍
    if ((now - s_last_tick_ms) < 500u) {
        return;
    }
    s_last_tick_ms = now;

    // 屏没起来自动重试
    if (!s_hw_ok) {
        if (s_panel_retry < VIEW_PANEL_RETRY_MAX && (now - s_last_retry_ms) >= VIEW_PANEL_RETRY_GAP_MS) {
            s_last_retry_ms = now;
            s_panel_retry++;

            if (display_view_retry_panel() == APP_OK) {
                LOGI(TAG, "（这是第 %d 次自动重试，屏接入后无需复位）", s_panel_retry);
            } else if (s_panel_retry == VIEW_PANEL_RETRY_MAX) {
                LOGW(TAG,
                     "屏自动重试 %d 次仍无应答，停止重试 —— "
                     "接好线后敲串口命令 panel-retry 即可（不用复位）",
                     VIEW_PANEL_RETRY_MAX);
            }
        }
        return;
    }

    static bool last_blink = false;
    static bool first      = true;

    view_digest_t d;
    make_digest(&s_pending, &d);

    // 新指令就重新计时
    const bool voice_new = s_have && (s_pending.voice_tick != s_voice_tick_seen);
    if (voice_new) {
        s_voice_tick_seen = s_pending.voice_tick;
        s_voice_until_ms  = now + DISPLAY_VIEW_VOICE_MS;
        LOGI(TAG, "语音反馈条: [%s] %s（显示 %d ms）", s_pending.voice_line1, s_pending.voice_line2,
             DISPLAY_VIEW_VOICE_MS);
    }
    const bool voice_on   = (s_voice_tick_seen != 0u) && (now < s_voice_until_ms);
    const bool voice_edge = (voice_on != s_voice_showing);
    s_voice_showing       = voice_on;

    const bool blink_now  = view_blink_phase();
    const bool blink_edge = (blink_now != last_blink);
    last_blink            = blink_now;

    const bool full         = s_force || first;
    const bool data_changed = !digest_same(&d, &s_last);
    const bool alert =
        s_have && s_pending.th_valid && (s_pending.temp_alert_c > 0.0f) &&
        (s_pending.temperature >= s_pending.temp_alert_c);

    // 只在有数据时推进
    const bool throttled_now = (now - s_last_refresh_ms) >= APP_DISPLAY_REFRESH_MS;
    if (throttled_now && s_have) {
        s_last_refresh_ms = now;
    }

    if (!full && !data_changed && !voice_edge && !(alert && blink_edge) && !(throttled_now && s_have)) {
        return;
    }

    s_force = false;
    first   = false;

    if (full || data_changed || voice_edge) {
        view_render_snapshot(&s_pending, full, voice_on);
    } else if (alert && blink_edge) {
        // 只重画温湿度行
        panel_begin_frame();
        view_render_temp_hum(&s_pending);
        panel_end_frame();
    } else {
        view_render_snapshot(&s_pending, false, voice_on);
    }

    s_last = d;
}
