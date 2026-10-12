// c
// SSD1306 驱动与绘图
//
// 自绘驱动，不依赖 U8g2 之类的第三方库：客户端装好 ESP32 开发板支持后
// 直接点“上传”就能编，不用再装任何库。

#include <string.h>

#include "panel.h"
#include "panel_font.h"
#include "panel_text.h"

#include "app_config.h"
#include "bsp_config.h"
#include "i2c_bus.h"

static const char *TAG = "PANEL";

// 只认这一种屏
#define PANEL_W BSP_PANEL_WIDTH
#define PANEL_H BSP_PANEL_HEIGHT

// 64 行分 8 页
#define PANEL_PAGES (PANEL_H / 8)

// 单色显存 1KB
#define PANEL_FB_BYTES ((int)PANEL_W * PANEL_PAGES)

// 1bpp 无灰阶
#define PANEL_FONT_SMALL 0

#if PANEL_FONT_SMALL
#define PANEL_CN_CELL_W 8
#define PANEL_CN_CELL_H 8
#else
#define PANEL_CN_CELL_W    PANEL_FONT_CN_W
#define PANEL_CN_CELL_H    PANEL_FONT_CN_H
#endif

#define OLED_CTRL_CMD  0x00u
#define OLED_CTRL_DATA 0x40u

// 0x8D 漏了全黑
static const uint8_t s_init_cmds[] = {
    0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40, 0x81, 0xCF, 0xA1, 0xA6,
    0xA8, 0x3F, 0xA4, 0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
    0x40, 0x8D, 0x14, 0xAF,
};

// 页式显存 1KB
static uint8_t s_page[PANEL_PAGES][PANEL_W];

// 脏页闭区间
#define PANEL_DIRTY_NONE 0xFFu
static uint8_t s_dirty_top    = PANEL_DIRTY_NONE;
static uint8_t s_dirty_bottom = PANEL_DIRTY_NONE;

// 实测地址
static uint8_t s_addr = BSP_I2C_ADDR_SSD1306;
static bool    s_ready;
static bool    s_display_on = true;

// 总线失败只报一次
static bool s_warned_io;

// 非黑即亮
static inline bool color_is_on(int color) {
    return (color != PANEL_BLACK);
}

// 行范围折脏页
static inline void panel_dirty_rows(int y0, int y1) {
    if (y0 > y1) {
        const int t = y0;
        y0          = y1;
        y1          = t;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > (PANEL_H - 1)) {
        y1 = PANEL_H - 1;
    }
    if (y0 > y1) {
        // 整段都在屏外
        return;
    }

    const uint8_t p0 = (uint8_t)(y0 / 8);
    const uint8_t p1 = (uint8_t)(y1 / 8);

    if (s_dirty_top == PANEL_DIRTY_NONE) {
        s_dirty_top    = p0;
        s_dirty_bottom = p1;
        return;
    }
    if (p0 < s_dirty_top) {
        s_dirty_top = p0;
    }
    if (p1 > s_dirty_bottom) {
        s_dirty_bottom = p1;
    }
}

// 命令拼一包发
static app_err_t oled_send_cmds(const uint8_t *cmds, int n) {
    if (cmds == NULL || n <= 0) {
        return APP_ERR_INVALID_ARG;
    }

    uint8_t buf[1 + sizeof(s_init_cmds)];
    if ((size_t)(n + 1) > sizeof(buf)) {
        return APP_ERR_INVALID_ARG;
    }

    buf[0] = OLED_CTRL_CMD;
    memcpy(&buf[1], cmds, (size_t)n);

    return bsp_i2c_write(s_addr, buf, n + 1);
}

// 命令数据要连续
static app_err_t oled_flush_page(uint8_t page) {
    if (page >= PANEL_PAGES) {
        return APP_ERR_INVALID_ARG;
    }

    const uint8_t addr_cmds[3] = {
        (uint8_t)(0xB0u | page),
        0x00,
        0x10,
    };
    app_err_t err = oled_send_cmds(addr_cmds, (int)sizeof(addr_cmds));
    if (err != APP_OK) {
        return err;
    }

    uint8_t buf[1 + PANEL_W];
    buf[0] = OLED_CTRL_DATA;
    memcpy(&buf[1], s_page[page], PANEL_W);
    return bsp_i2c_write(s_addr, buf, (int)sizeof(buf));
}

// 失败重试一次
static app_err_t oled_flush_pages(int p0, int p1) {
    if (!s_ready) {
        return APP_ERR_INVALID_STATE;
    }
    if (p0 < 0) {
        p0 = 0;
    }
    if (p1 > (PANEL_PAGES - 1)) {
        p1 = PANEL_PAGES - 1;
    }

    for (int p = p0; p <= p1; p++) {
        app_err_t err = oled_flush_page((uint8_t)p);

        if (err != APP_OK) {
            // 歇 2ms 重试
            delay(2);
            err = oled_flush_page((uint8_t)p);
        }

        if (err != APP_OK) {
            // 失败留给下一帧
            if (!s_warned_io) {
                s_warned_io = true;
                LOGW(TAG,
                     "OLED 第 %d 页推送失败(重试后仍失败): %s —— "
                     "脏页标记已保留，下一帧会自动重推；之后同类错误不再重复打印",
                     p, app_err_text(err));
            }
            return err;
        }
    }
    return APP_OK;
}

// 两个地址都试
static app_err_t oled_probe_addr(uint8_t *out_addr) {
    static const uint8_t candidates[] = {BSP_I2C_ADDR_SSD1306, BSP_I2C_ADDR_SSD1306_ALT};

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (bsp_i2c_probe(candidates[i])) {
            *out_addr = candidates[i];
            return APP_OK;
        }
    }
    return APP_ERR_NOT_FOUND;
}

// 可重复调用重发
app_err_t panel_init(void) {
    // I2C 总线
    const app_err_t bus_err = bsp_i2c_init();
    if (bus_err != APP_OK) {
        LOGE(TAG, "I2C 总线初始化失败: %s —— 屏幕不可用", app_err_text(bus_err));
        s_ready = false;
        return bus_err;
    }

    // 探测并挂载
    uint8_t addr = 0;
    if (oled_probe_addr(&addr) != APP_OK) {
        LOGE(TAG,
             "没找到 SSD1306（试过 0x%02X / 0x%02X）—— "
             "查 SDA=GPIO%d / SCL=GPIO%d 接线、模块供电，以及地址跳线",
             BSP_I2C_ADDR_SSD1306, BSP_I2C_ADDR_SSD1306_ALT, BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
        s_ready = false;
        return APP_ERR_NOT_FOUND;
    }
    s_addr = addr;
    if (s_addr != BSP_I2C_ADDR_SSD1306) {
        LOGW(TAG, "SSD1306 在 0x%02X（默认是 0x%02X）—— 已按实际地址使用", s_addr, BSP_I2C_ADDR_SSD1306);
    }

    // 上电序列
    const app_err_t err = oled_send_cmds(s_init_cmds, (int)sizeof(s_init_cmds));
    if (err != APP_OK) {
        LOGE(TAG, "OLED 初始化序列发送失败: %s", app_err_text(err));
        s_ready = false;
        return err;
    }

    // 清屏防雪花
    memset(s_page, 0x00, PANEL_FB_BYTES);
    s_ready      = true;
    s_display_on = true;
    s_warned_io  = false;

    (void)oled_flush_pages(0, PANEL_PAGES - 1);
    s_dirty_top    = PANEL_DIRTY_NONE;
    s_dirty_bottom = PANEL_DIRTY_NONE;

    LOGI(TAG, "SSD1306 init done (%dx%d, %d 页, I2C 0x%02X, SDA=GPIO%d SCL=GPIO%d)", PANEL_W, PANEL_H,
         PANEL_PAGES, s_addr, BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
    LOGI(TAG, "中文字库共 %d 字（ready=%d）", panel_font_cn_count(), (int)panel_font_ready());
    return APP_OK;
}

// 重发初始化，屏掉了不用复位
app_err_t panel_retry(void) {
    return panel_init();
}

bool panel_ready(void) {
    return s_ready;
}

int panel_width(void) {
    return PANEL_W;
}

int panel_height(void) {
    return PANEL_H;
}

// 落笔
static inline void px(int x, int y, int color) {
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) {
        return;
    }

    const int page = y / 8;
    const uint8_t bit = (uint8_t)(1u << (y % 8));

    if (color_is_on(color)) {
        s_page[page][x] |= bit;
    } else {
        s_page[page][x] = (uint8_t)(s_page[page][x] & (uint8_t)~bit);
    }
}

// 先清底再画字
static void blit_glyph(int x, int y, const uint8_t *glyph, int w, int h, int fg, int bg) {
    if (glyph == NULL) {
        return;
    }
    if (y < 0 || y >= PANEL_H) {
        return;
    }

    for (int col = 0; col < w; col++) {
        const int gx = x + col;
        if (gx < 0 || gx >= PANEL_W) {
            continue;
        }

        for (int row = 0; row < h; row++) {
            const int gy = y + row;
            if (gy < 0 || gy >= PANEL_H) {
                continue;
            }

            // 先列后页，bit0 在最上面
            const uint8_t byte = glyph[(row / 8) * w + col];
            const bool    on   = ((byte >> (row % 8)) & 0x01u) != 0;

            // 透明就只画亮点，不擦底
            if (on) {
                px(gx, gy, fg);
            } else if (bg != PANEL_TRANSPARENT) {
                px(gx, gy, bg);
            }
        }
    }

    panel_dirty_rows(y, y + h - 1);
}

// 画一个 ASCII 字符（8x16）
static void draw_ascii_raw(int x, int y, char ch, int fg, int bg) {
    const uint8_t *g = panel_font_ascii_lookup(ch);
    if (g == NULL) {
        // 认不出也要占位
        if (bg != PANEL_TRANSPARENT) {
            panel_fill_rect(x, y, PANEL_FONT_ASCII_W, PANEL_FONT_ASCII_H, bg);
        }
        return;
    }
    blit_glyph(x, y, g, PANEL_FONT_ASCII_W, PANEL_FONT_ASCII_H, fg, bg);
}

// 画一个汉字（16x16）
static void draw_cn_raw(int x, int y, uint32_t cp, int fg, int bg, int scale) {
    if (scale < 1) {
        scale = 1;
    }

    const uint8_t *g = panel_font_cn_lookup(cp);
    if (g == NULL) {
        // 豆腐块，让人一眼看出字库缺字
        panel_draw_rect(x, y, PANEL_CN_CELL_W * scale, PANEL_CN_CELL_H * scale, color_is_on(fg) ? fg : bg);
        return;
    }

    for (int col = 0; col < PANEL_FONT_CN_W; col++) {
        for (int row = 0; row < PANEL_FONT_CN_H; row++) {
            const uint8_t byte = g[(row / 8) * PANEL_FONT_CN_W + col];
            const bool    on   = ((byte >> (row % 8)) & 0x01u) != 0;

            // 放大就把点铺成 scale×scale 的块
            const int bx = x + col * scale;
            const int by = y + row * scale;

            if (on) {
                panel_fill_rect(bx, by, scale, scale, fg);
            } else if (bg != PANEL_TRANSPARENT) {
                panel_fill_rect(bx, by, scale, scale, bg);
            }
        }
    }

    panel_dirty_rows(y, y + PANEL_CN_CELL_H * scale - 1);
}

void panel_clear(int color) {
    memset(s_page, color_is_on(color) ? 0xFF : 0x00, PANEL_FB_BYTES);
    panel_dirty_rows(0, PANEL_H - 1);
}

void panel_fill_rect(int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) {
        return;
    }

    // 夹到屏内，别让越界参数把循环拖爆
    int x0 = x;
    int y0 = y;
    int x1 = x + w - 1;
    int y1 = y + h - 1;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (PANEL_W - 1)) {
        x1 = PANEL_W - 1;
    }
    if (y1 > (PANEL_H - 1)) {
        y1 = PANEL_H - 1;
    }
    if (x0 > x1 || y0 > y1) {
        return;
    }

    // 逐页置位，比逐点快
    for (int py = y0; py <= y1;) {
        const int page = py / 8;
        const int ptop = page * 8;
        // 本页里要盖的行
        const int lo = (y0 > ptop) ? (y0 - ptop) : 0;
        const int hi = (y1 < (ptop + 7)) ? (y1 - ptop) : 7;

        uint8_t mask = 0;
        for (int b = lo; b <= hi; b++) {
            mask |= (uint8_t)(1u << b);
        }

        for (int cx = x0; cx <= x1; cx++) {
            if (color_is_on(color)) {
                s_page[page][cx] |= mask;
            } else {
                s_page[page][cx] = (uint8_t)(s_page[page][cx] & (uint8_t)~mask);
            }
        }

        py = ptop + 8;
    }

    panel_dirty_rows(y0, y1);
}

void panel_draw_hline(int x, int y, int w, int color) {
    panel_fill_rect(x, y, w, 1, color);
}

void panel_draw_vline(int x, int y, int h, int color) {
    panel_fill_rect(x, y, 1, h, color);
}

void panel_draw_rect(int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) {
        return;
    }

    panel_draw_hline(x, y, w, color);
    panel_draw_hline(x, y + h - 1, w, color);
    panel_draw_vline(x, y, h, color);
    panel_draw_vline(x + w - 1, y, h, color);
}

// 画 ASCII，返回结束 x
int panel_draw_text(int x, int y, const char *s, int fg, int bg) {
    if (s == NULL) {
        return x;
    }

    int cx = x;
    for (const char *p = s; *p != '\0'; p++) {
        draw_ascii_raw(cx, y, *p, fg, bg);
        cx += PANEL_FONT_ASCII_W;
    }
    return cx;
}

// 画中文，返回结束 x
int panel_draw_text_cn(int x, int y, const char *utf8, int fg, int bg) {
    return panel_draw_text_cn_scaled(x, y, utf8, fg, bg, 1);
}

// 放大 scale 倍
int panel_draw_text_cn_scaled(int x, int y, const char *utf8, int fg, int bg, int scale) {
    if (utf8 == NULL) {
        return x;
    }
    if (scale < 1) {
        scale = 1;
    }

    int cx = x;

    for (const char *p = utf8; *p != '\0';) {
        uint32_t  cp  = 0;
        int       adv = 1;
        const bool ok = panel_font_utf8_next(p, &cp, &adv);
        p += adv;

        if (!ok) {
            // 坏字节跳过，别死循环
            continue;
        }

        if (cp < 0x80u) {
            draw_ascii_raw(cx, y, (char)cp, fg, bg);
            cx += PANEL_FONT_ASCII_W * scale;
        } else {
            draw_cn_raw(cx, y, cp, fg, bg, scale);
            cx += PANEL_CN_CELL_W * scale;
        }
    }
    return cx;
}

int panel_text_width(const char *utf8) {
    if (utf8 == NULL) {
        return 0;
    }

    int w = 0;

    for (const char *p = utf8; *p != '\0';) {
        uint32_t  cp  = 0;
        int       adv = 1;
        const bool ok = panel_font_utf8_next(p, &cp, &adv);
        p += adv;

        if (!ok) {
            continue;
        }
        w += (cp < 0x80u) ? PANEL_FONT_ASCII_W : PANEL_CN_CELL_W;
    }
    return w;
}

void panel_draw_text_cn_center(int x, int y, int w, int h, const char *utf8, int fg, int bg) {
    if (utf8 == NULL) {
        return;
    }

    const int tw = panel_text_width(utf8);
    const int tx = x + (w - tw) / 2;
    const int ty = y + (h - PANEL_FONT_CN_H) / 2;

    (void)panel_draw_text_cn(tx, ty, utf8, fg, bg);
}

// 一帧的开始与结束
void panel_begin_frame(void) {
    // 点位标记阶段，收尾时统一推
}

void panel_end_frame(void) {
    if (!s_ready) {
        return;
    }
    if (s_dirty_top == PANEL_DIRTY_NONE) {
        // 没脏页就不推
        return;
    }

    const app_err_t err = oled_flush_pages(s_dirty_top, s_dirty_bottom);
    if (err == APP_OK) {
        s_dirty_top    = PANEL_DIRTY_NONE;
        s_dirty_bottom = PANEL_DIRTY_NONE;
    }
}

void panel_flush_all(void) {
    if (!s_ready) {
        return;
    }

    (void)oled_flush_pages(0, PANEL_PAGES - 1);
    s_dirty_top    = PANEL_DIRTY_NONE;
    s_dirty_bottom = PANEL_DIRTY_NONE;
}

// 只发 DISPOFF/DISPON
void panel_display_test(bool on) {
    if (!s_ready) {
        LOGW(TAG, "屏不可用，disp 测试跳过");
        return;
    }

    const uint8_t   cmd = on ? 0xAFu : 0xAEu;
    const app_err_t err = oled_send_cmds(&cmd, 1);
    if (err == APP_OK) {
        s_display_on = on;
        LOGI(TAG, "已发 %s：屏全黑 ⇔ 控制器收到了命令（OLED 无背光）", on ? "DISPON" : "DISPOFF");
    } else {
        LOGW(TAG, "显示开关命令失败: %s", app_err_text(err));
    }
}

// OLED 无背光，改成闪烁
void panel_blink_backlight(int times, int on_ms, int off_ms) {
    if (!s_ready) {
        LOGW(TAG, "屏不可用，闪烁测试跳过");
        return;
    }
    if (times <= 0) {
        times = 3;
    }
    if (on_ms <= 0) {
        on_ms = 400;
    }
    if (off_ms <= 0) {
        off_ms = 400;
    }

    LOGI(TAG, "显示闪烁 %d 次（确认屏受软件控制；OLED 无背光）", times);

    for (int i = 0; i < times; i++) {
        panel_display_test(false);
        delay(off_ms);
        panel_display_test(true);
        delay(on_ms);
    }

    s_display_on = true;
}

// 纯色轮播加边框
void panel_selftest(int hold_ms) {
    if (!s_ready) {
        LOGW(TAG, "屏不可用，自检跳过");
        return;
    }
    if (hold_ms <= 0) {
        hold_ms = 700;
    }

    LOGI(TAG, "======== 屏幕自检开始（每屏 %d ms）========", hold_ms);

    // 1 全亮
    LOGI(TAG, "  1/5 全亮");
    panel_clear(PANEL_WHITE);
    panel_flush_all();
    delay(hold_ms);

    // 2 全灭
    LOGI(TAG, "  2/5 全灭");
    panel_clear(PANEL_BLACK);
    panel_flush_all();
    delay(hold_ms);

    // 3 边框
    LOGI(TAG, "  3/5 边框");
    panel_clear(PANEL_BLACK);
    panel_draw_rect(0, 0, PANEL_W, PANEL_H, PANEL_WHITE);
    panel_flush_all();
    delay(hold_ms);

    // 4 交叉线
    LOGI(TAG, "  4/5 交叉线");
    for (int i = 0; i < PANEL_W; i++) {
        px(i, i * PANEL_H / PANEL_W, PANEL_WHITE);
        px(i, PANEL_H - 1 - (i * PANEL_H / PANEL_W), PANEL_WHITE);
    }
    panel_flush_all();
    delay(hold_ms);

    // 5 棋盘
    LOGI(TAG, "  5/5 棋盘");
    panel_clear(PANEL_BLACK);
    for (int y = 0; y < PANEL_H; y += 8) {
        for (int x = 0; x < PANEL_W; x += 8) {
            if (((x / 8) + (y / 8)) & 1) {
                panel_fill_rect(x, y, 8, 8, PANEL_WHITE);
            }
        }
    }
    panel_flush_all();
    delay(hold_ms);

    LOGI(TAG, "======== 自检结束（画面交回业务界面）========");
}
