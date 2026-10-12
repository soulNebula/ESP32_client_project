// c
// SSD1306 驱动与绘图
#include "sdkconfig.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"

#include "board_config.h"
#include "i2c_bus.h"
#include "panel.h"
#include "panel_font.h"

static const char *TAG = "PANEL";

#define PANEL_W         128
#define PANEL_H         64
#define PANEL_CTRL_NAME "SSD1306"

// 64 行分 8 页
#define PANEL_PAGES (PANEL_H / 8)

// 单色显存 1KB
#define PANEL_FB_BYTES ((size_t)PANEL_W * (size_t)PANEL_PAGES)

// 1bpp 无灰阶
#ifndef PANEL_FONT_SMALL
// 0 大字 1 小字
#define PANEL_FONT_SMALL 0
#endif

// 两处定义必须同步
_Static_assert(PANEL_FONT_SMALL == PANEL_FONT_SMALL_ACTIVE,
               "panel.c 的 PANEL_FONT_SMALL 与 panel.h 的 PANEL_FONT_SMALL_ACTIVE 不一致："
               "改字号必须两处一起改，并重排 display_view.c 的行坐标");

#if PANEL_FONT_SMALL
#define PANEL_CN_CELL_W 8
#define PANEL_CN_CELL_H 8
#define PANEL_ASCII_CELL_W 4
#define PANEL_ASCII_CELL_H 8
#else
#define PANEL_CN_CELL_W    PANEL_FONT_CN_W
#define PANEL_CN_CELL_H    PANEL_FONT_CN_H
#define PANEL_ASCII_CELL_W PANEL_FONT_ASCII_W
#define PANEL_ASCII_CELL_H PANEL_FONT_ASCII_H
// 字号收尾
#endif

#define OLED_CTRL_CMD  0x00u
#define OLED_CTRL_DATA 0x40u

// 单次传输上限
#define OLED_MAX_XFER 256u

// 命令缓冲大小
#define OLED_CMD_BUF 8u

// 0x8D 漏了全黑
static const uint8_t s_init_cmds[] = {
    0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40, 0x81, 0xCF, 0xA1, 0xA6, 0xA8, 0x3F,
    0xA4, 0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40, 0x8D, 0x14, 0xAF,
};

// 页式显存 1KB
static uint8_t s_page[PANEL_PAGES][PANEL_W];

// 脏页闭区间
#define PANEL_DIRTY_NONE 0xFFu
static uint8_t s_dirty_top    = PANEL_DIRTY_NONE;
static uint8_t s_dirty_bottom = PANEL_DIRTY_NONE;

static i2c_master_dev_handle_t s_dev = NULL;
// 实测地址
static uint8_t s_addr = BSP_I2C_ADDR_SSD1306;
static bool s_ready = false;
static bool s_display_on = true;

// 总线失败只报一次
static bool s_warned_io = false;

// 非黑即亮
static inline bool color_is_on(uint16_t color) {
    return (color != 0x0000u);
}

// 行范围折脏页
static inline void panel_dirty_rows(int y0, int y1) {
    if (y0 > y1) {
        int t = y0;
        y0    = y1;
        y1    = t;
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
static esp_err_t oled_send_cmds(const uint8_t *cmds, size_t n) {
    if (s_dev == NULL || cmds == NULL || n == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((n + 1u) > OLED_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buf[OLED_MAX_XFER];
    buf[0] = OLED_CTRL_CMD;
    memcpy(&buf[1], cmds, n);

    return bsp_i2c_write(s_dev, buf, n + 1u, 0);
}

// 命令数据要连续
static esp_err_t oled_flush_page(uint8_t page) {
    if (s_dev == NULL || page >= PANEL_PAGES) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t addr_cmds[3] = {
        (uint8_t)(0xB0u | page),
        0x00,
        0x10,
    };
    esp_err_t err = oled_send_cmds(addr_cmds, sizeof(addr_cmds));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1 + PANEL_W];
    buf[0] = OLED_CTRL_DATA;
    memcpy(&buf[1], s_page[page], PANEL_W);
    return bsp_i2c_write(s_dev, buf, sizeof(buf), 0);
}

// 失败重试一次
static esp_err_t oled_flush_pages(int p0, int p1) {
    if (!s_ready || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (p0 < 0) {
        p0 = 0;
    }
    if (p1 > (PANEL_PAGES - 1)) {
        p1 = PANEL_PAGES - 1;
    }

    for (int p = p0; p <= p1; p++) {
        esp_err_t err = oled_flush_page((uint8_t)p);

        if (err != ESP_OK) {
            // 歇 2ms 重试
            vTaskDelay(pdMS_TO_TICKS(2));
            err = oled_flush_page((uint8_t)p);
        }

        if (err != ESP_OK) {
            // 失败留给下一帧
            if (!s_warned_io) {
                s_warned_io = true;
                ESP_LOGW(TAG,
                         "OLED 第 %d 页推送失败(重试后仍失败): %s —— "
                         "脏页标记已保留，下一帧会自动重推；之后同类错误不再重复打印",
                         p, esp_err_to_name(err));
            }
            return err;
        }
    }
    return ESP_OK;
}

// 关显示即熄屏
void panel_backlight(bool on) {
    if (!s_ready || s_dev == NULL) {
        return;
    }
    const uint8_t   cmd = on ? 0xAFu : 0xAEu;
    const esp_err_t err = oled_send_cmds(&cmd, 1);
    if (err == ESP_OK) {
        s_display_on = on;
    } else {
        ESP_LOGW(TAG, "显示开关命令失败: %s", esp_err_to_name(err));
    }
}

// 两个地址都试
static esp_err_t oled_probe_addr(uint8_t *out_addr) {
    static const uint8_t candidates[] = {BSP_I2C_ADDR_SSD1306, BSP_I2C_ADDR_SSD1306_ALT};

    // 先查扫描缓存
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (bsp_i2c_present(candidates[i])) {
            *out_addr = candidates[i];
            return ESP_OK;
        }
    }

    // 缓存没有再实测
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (i2c_master_probe(bsp_i2c_bus(), candidates[i], BSP_I2C_TIMEOUT_MS) == ESP_OK) {
            *out_addr = candidates[i];
            ESP_LOGW(TAG, "扫描缓存里没有 0x%02X，但实测存在 —— 已按实测地址使用", candidates[i]);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

// 可重复调用重发
esp_err_t panel_init(void) {
    // I2C 总线
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s —— 屏幕不可用", esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    // 探测并挂载
    if (s_dev == NULL) {
        uint8_t addr = 0;
        if (oled_probe_addr(&addr) != ESP_OK) {
            ESP_LOGE(TAG,
                     "没找到 SSD1306（试过 0x%02X / 0x%02X）—— "
                     "查 SDA=GPIO%d / SCL=GPIO%d 接线、模块供电，以及地址跳线",
                     BSP_I2C_ADDR_SSD1306, BSP_I2C_ADDR_SSD1306_ALT, (int)BSP_I2C_SDA_GPIO,
                     (int)BSP_I2C_SCL_GPIO);
            s_ready = false;
            return ESP_ERR_NOT_FOUND;
        }
        s_addr = addr;

        err = bsp_i2c_add_device(s_addr, BSP_I2C_FREQ_HZ, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "挂载 OLED(0x%02X) 失败: %s", s_addr, esp_err_to_name(err));
            s_dev   = NULL;
            s_ready = false;
            return err;
        }
        if (s_addr != BSP_I2C_ADDR_SSD1306) {
            ESP_LOGW(TAG, "SSD1306 在 0x%02X（默认是 0x%02X）—— 已按实际地址使用", s_addr,
                     BSP_I2C_ADDR_SSD1306);
        }
    }

    // 上电序列
    err = oled_send_cmds(s_init_cmds, sizeof(s_init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED 初始化序列发送失败: %s", esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    // 清屏防雪花
    memset(s_page, 0x00, PANEL_FB_BYTES);
    s_ready      = true;
    s_display_on = true;

    oled_flush_pages(0, PANEL_PAGES - 1);
    s_dirty_top    = PANEL_DIRTY_NONE;
    s_dirty_bottom = PANEL_DIRTY_NONE;

    ESP_LOGI(TAG, "SSD1306 init done (%dx%d, %d 页, I2C 0x%02X, SDA=GPIO%d SCL=GPIO%d)", PANEL_W, PANEL_H,
             PANEL_PAGES, s_addr, (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    return ESP_OK;
}

int panel_width(void) {
    return PANEL_W;
}

int panel_height(void) {
    return PANEL_H;
}

// 屏可用吗
bool panel_ready(void) {
    return s_ready;
}

// 整屏填充并推屏
void panel_clear(uint16_t color) {
    memset(s_page, color_is_on(color) ? 0xFF : 0x00, PANEL_FB_BYTES);
    s_dirty_top    = 0;
    s_dirty_bottom = PANEL_PAGES - 1;
    panel_flush_all();
}

// 全屏推送，少用
void panel_flush_all(void) {
    if (!s_ready) {
        return;
    }
    // 成功才清脏标记
    if (oled_flush_pages(0, PANEL_PAGES - 1) == ESP_OK) {
        s_dirty_top    = PANEL_DIRTY_NONE;
        s_dirty_bottom = PANEL_DIRTY_NONE;
    }
}

// 自检卡图形
static void panel_draw_selftest_card(uint16_t frame, uint16_t bar) {
    panel_clear(PANEL_BLACK);

    panel_fill_rect(0, 0, PANEL_W, 8, bar);

    panel_draw_rect(4, 12, PANEL_W - 8, PANEL_H - 16, frame);

    for (int x = 4; x < PANEL_W - 4; x++) {
        const int span = PANEL_H - 16 - 1;
        const int y1   = 12 + (int)((long)(x - 4) * span / (PANEL_W - 8));
        const int y2   = 12 + span - (int)((long)(x - 4) * span / (PANEL_W - 8));
        panel_draw_pixel(x, y1, frame);
        panel_draw_pixel(x, y2, frame);
    }
}

// 逐张图案自检
void panel_selftest(int hold_ms) {
    if (!s_ready) {
        ESP_LOGE(TAG, "自检跳过：屏幕没有初始化成功（先查 SDA=GPIO%d / SCL=GPIO%d 接线和模块供电）",
                 (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
        return;
    }
    if (hold_ms <= 0) {
        hold_ms = 400;
    }

    ESP_LOGI(TAG, "屏幕自检开始：全亮→全灭→左半→右半→上半→下半→棋盘→自检卡，每屏 %dms", hold_ms);
    ESP_LOGI(TAG, "  判读：左右反=改 0xA1→0xA0；上下反=改 0xC8→0xC0；条纹=页/列映射错位");

    ESP_LOGI(TAG, "  自检画面 1/8: 全亮（整屏点亮，检查有没有整页/整列不亮）");
    panel_clear(PANEL_WHITE);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 2/8: 全灭（检查残影/坏点）");
    panel_clear(PANEL_BLACK);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 3/8: 左半亮");
    panel_clear(PANEL_BLACK);
    panel_fill_rect(0, 0, PANEL_W / 2, PANEL_H, PANEL_WHITE);
    panel_flush_all();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 4/8: 右半亮");
    panel_clear(PANEL_BLACK);
    panel_fill_rect(PANEL_W / 2, 0, PANEL_W / 2, PANEL_H, PANEL_WHITE);
    panel_flush_all();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 5/8: 上半亮");
    panel_clear(PANEL_BLACK);
    panel_fill_rect(0, 0, PANEL_W, PANEL_H / 2, PANEL_WHITE);
    panel_flush_all();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 6/8: 下半亮");
    panel_clear(PANEL_BLACK);
    panel_fill_rect(0, PANEL_H / 2, PANEL_W, PANEL_H / 2, PANEL_WHITE);
    panel_flush_all();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 7/8: 棋盘格（行列映射错位会变成条纹）");
    panel_clear(PANEL_BLACK);
    for (int y = 0; y < PANEL_H; y += 4) {
        for (int x = 0; x < PANEL_W; x += 4) {
            if (((x / 4) + (y / 4)) % 2 == 0) {
                panel_fill_rect(x, y, 4, 4, PANEL_WHITE);
            }
        }
    }
    panel_flush_all();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "  自检画面 8/8: 边框+对角交叉线+顶部色条（看边框是否完整、线是否直）");
    panel_draw_selftest_card(PANEL_WHITE, PANEL_WHITE);
    panel_flush_all();
    vTaskDelay(pdMS_TO_TICKS(hold_ms * 3));

    panel_clear(PANEL_BLACK);
    ESP_LOGI(TAG, "屏幕自检结束（画面已清黑，随后由业务界面接管）");
}

// 闪显示确认可控
void panel_blink_backlight(int times, int on_ms, int off_ms) {
    if (!s_ready) {
        ESP_LOGE(TAG, "显示闪烁跳过：屏幕没有初始化成功");
        return;
    }
    if (times <= 0) {
        times = 3;
    }
    if (on_ms <= 0) {
        on_ms = 300;
    }
    if (off_ms <= 0) {
        off_ms = 300;
    }

    ESP_LOGI(TAG, "显示闪烁 %d 次（亮 %dms / 灭 %dms）—— 看得见闪说明 OLED 受软件控制", times, on_ms, off_ms);

    for (int i = 0; i < times; i++) {
        panel_backlight(false);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
        panel_backlight(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
    }
    panel_backlight(true);
    ESP_LOGI(TAG, "显示闪烁结束（已恢复常亮）");
}

// 测显示开关命令
void panel_display_test(bool on) {
    if (!s_ready || s_dev == NULL) {
        ESP_LOGE(TAG, "显示开关测试跳过：屏幕没有初始化成功");
        return;
    }
    ESP_LOGW(TAG, "显示开关测试：发 %s（%s）", on ? "DISPON(0xAF)" : "DISPOFF(0xAE)",
             on ? "应恢复显示" : "整屏应立刻全黑 —— 没变化就说明命令没进控制器");
    panel_backlight(on);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// 清脏标记不清显存
void panel_begin_frame(void) {
    s_dirty_top    = PANEL_DIRTY_NONE;
    s_dirty_bottom = PANEL_DIRTY_NONE;
}

// 只推本帧脏页
void panel_end_frame(void) {
    if (!s_ready) {
        return; // 屏不可用
    }
    if (s_dirty_top == PANEL_DIRTY_NONE) {
        return; // 无改动
    }

    const esp_err_t err = oled_flush_pages((int)s_dirty_top, (int)s_dirty_bottom);

    // 成功才清脏页
    if (err == ESP_OK) {
        s_dirty_top    = PANEL_DIRTY_NONE;
        s_dirty_bottom = PANEL_DIRTY_NONE;
    }
}

// 写像素进显存
static inline void fb_set_pixel(int x, int y, bool on) {
    const uint8_t page = (uint8_t)(y >> 3);
    const uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) {
        s_page[page][x] |= mask;
    } else {
        s_page[page][x] = (uint8_t)(s_page[page][x] & (uint8_t)~mask);
    }
}

// 画点，越界忽略
void panel_draw_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) {
        return; // 越界忽略
    }
    fb_set_pixel(x, y, color_is_on(color));
    panel_dirty_rows(y, y);
}

// 必须先裁剪
void panel_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }

    // 水平裁剪
    if (x < 0) {
        w += x; // 从宽度扣除
        x = 0;
    }
    if (x >= PANEL_W || w <= 0) {
        return; // 整块在右边屏外
    }
    if (x + w > PANEL_W) {
        w = PANEL_W - x; // 截到屏边
    }

    // 垂直裁剪
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y >= PANEL_H || h <= 0) {
        return;
    }
    if (y + h > PANEL_H) {
        h = PANEL_H - y;
    }

    const bool on = color_is_on(color);
    for (int r = 0; r < h; r++) {
        const int yy = y + r;
        for (int c = 0; c < w; c++) {
            fb_set_pixel(x + c, yy, on);
        }
    }

    panel_dirty_rows(y, y + h - 1);
}

// 画矩形边框
void panel_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    panel_fill_rect(x, y, w, 1, color);
    if (h > 1) {
        panel_fill_rect(x, y + h - 1, w, 1, color);
    }
    if (h > 2) {
        panel_fill_rect(x, y + 1, 1, h - 2, color);
        if (w > 1) {
            panel_fill_rect(x + w - 1, y + 1, 1, h - 2, color);
        }
    }
}

void panel_draw_hline(int x, int y, int w, uint16_t color) {
    panel_fill_rect(x, y, w, 1, color);
}

void panel_draw_vline(int x, int y, int h, uint16_t color) {
    panel_fill_rect(x, y, 1, h, color);
}

// 按行扫画实心圆
void panel_fill_circle(int cx, int cy, int r, uint16_t color) {
    if (r <= 0) {
        panel_draw_pixel(cx, cy, color);
        return;
    }

    for (int dy = -r; dy <= r; dy++) {
        // 整数开方免浮点
        int v = r * r - dy * dy;
        if (v < 0) {
            continue;
        }
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= v) {
            dx++;
        }
        panel_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

// 透明底留原内容
static void draw_glyph(int x, int y, const uint8_t *data, int w, int h, uint16_t fg, uint16_t bg) {
    if (data == NULL || w <= 0 || h <= 0) {
        return;
    }
    const bool transparent = (bg == PANEL_TRANSPARENT);
    const bool fg_on       = color_is_on(fg);
    const bool bg_on       = color_is_on(bg);

#if PANEL_FONT_SMALL
    // 小字按倍数降采样
    const bool is_cn = (w >= PANEL_FONT_CN_W);
    const int  cw    = is_cn ? PANEL_CN_CELL_W : PANEL_ASCII_CELL_W;
    const int  ch    = is_cn ? PANEL_CN_CELL_H : PANEL_ASCII_CELL_H;
    // 横向倍率
    const int fx = (cw > 0) ? (w / cw) : 1;
    // 纵向倍率
    const int fy = (ch > 0) ? (h / ch) : 1;

    for (int ty = 0; ty < ch; ty++) {
        for (int tx = 0; tx < cw; tx++) {
            bool lit = false;

            // 源像素 OR 合成
            for (int sy = ty * fy; sy < (ty + 1) * fy && !lit; sy++) {
                for (int sx = tx * fx; sx < (tx + 1) * fx; sx++) {
                    if (sx >= w || sy >= h) {
                        continue;
                    }
                    // 字模逐列式
                    const int     page = sy >> 3;
                    const int     bit  = sy & 7;
                    const uint8_t byte = data[page * w + sx];
                    if ((byte & (1u << bit)) != 0u) {
                        lit = true;
                        break;
                    }
                }
            }

            if (!lit && transparent) {
                continue; // 透明底跳过
            }
            fb_set_pixel(x + tx, y + ty, lit ? fg_on : bg_on);
        }
    }

    panel_dirty_rows(y, y + ch - 1);
#else
    // 大字直接渲染
    const int pages = (h + 7) / 8; // 16 高两页

    for (int p = 0; p < pages; p++) {
        for (int c = 0; c < w; c++) {
            const uint8_t bits = data[p * w + c];
            if (bits == 0 && transparent) {
                continue; // 空列跳过
            }
            for (int b = 0; b < 8; b++) {
                const bool lit = (bits & (1u << b)) != 0;
                if (!lit && transparent) {
                    continue; // 透明底跳过
                }
                fb_set_pixel(x + c, y + p * 8 + b, lit ? fg_on : bg_on);
            }
        }
    }

    panel_dirty_rows(y, y + h - 1);
// 字号分支收尾
#endif
}

// 画 ASCII 串
int panel_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg) {
    if (str == NULL) {
        return x;
    }
    for (const char *p = str; *p != '\0'; p++) {
        const uint8_t *g = panel_font_ascii_lookup(*p);
        if (g != NULL) {
            draw_glyph(x, y, g, PANEL_FONT_ASCII_W, PANEL_FONT_ASCII_H, fg, bg);
        }
        x += PANEL_ASCII_CELL_W;
    }
    return x;
}

// 串占多少像素宽
int panel_text_width(const char *utf8) {
    if (utf8 == NULL) {
        return 0;
    }
    int         w   = 0;
    uint32_t    cp  = 0;
    int         adv = 0;
    const char *p   = utf8;
    while (*p != '\0') {
        if (!panel_font_utf8_next(p, &cp, &adv)) {
            break; // 非法 UTF-8
        }
        w += (cp < 0x80u) ? PANEL_ASCII_CELL_W : PANEL_CN_CELL_W;
        p += adv;
    }
    return w;
}

// 画中英混排串
int panel_draw_text_cn(int x, int y, const char *utf8, uint16_t fg, uint16_t bg) {
    if (utf8 == NULL) {
        return x;
    }

    uint32_t    cp  = 0;
    int         adv = 0;
    const char *p   = utf8;

    while (*p != '\0') {
        if (!panel_font_utf8_next(p, &cp, &adv)) {
            break; // 非法 UTF-8
        }

        if (cp < 0x80u) {
            // ASCII 字模
            const uint8_t *g = panel_font_ascii_lookup((char)cp);
            if (g != NULL) {
                draw_glyph(x, y, g, PANEL_FONT_ASCII_W, PANEL_FONT_ASCII_H, fg, bg);
            }
            x += PANEL_ASCII_CELL_W;
        } else {
            // 汉字字库
            const uint8_t *g = panel_font_cn_lookup(cp);
            if (g != NULL) {
                draw_glyph(x, y, g, PANEL_FONT_CN_W, PANEL_FONT_CN_H, fg, bg);
            } else {
                // 缺字画方框
                panel_draw_rect(x, y, PANEL_CN_CELL_W, PANEL_CN_CELL_H, fg);
            }
            x += PANEL_CN_CELL_W;
        }

        p += adv;
    }

    return x;
}

// 矩形内居中画
void panel_draw_text_cn_center(int x, int y, int w, int h, const char *utf8, uint16_t fg, uint16_t bg) {
    if (utf8 == NULL || w <= 0 || h <= 0) {
        return;
    }

    const int text_w = panel_text_width(utf8);

    const int tx = x + (w - text_w) / 2;
    const int ty = y + (h - PANEL_CN_CELL_H) / 2;

    panel_draw_text_cn(tx, ty, utf8, fg, bg);
}

// 字模整数倍放大
static void draw_glyph_scaled(int x, int y, const uint8_t *data, int w, int h, uint16_t fg, uint16_t bg,
                              int cw, int ch) {
    if (data == NULL || w <= 0 || h <= 0) {
        return;
    }
    const bool transparent = (bg == PANEL_TRANSPARENT);
    const bool fg_on       = color_is_on(fg);
    const bool bg_on       = color_is_on(bg);
    // 横向倍率
    const int sx_step = (cw > 0) ? (cw / w) : 1;
    // 纵向倍率
    const int sy_step = (ch > 0) ? (ch / h) : 1;

    for (int sy = 0; sy < h; sy++) {
        const int page = sy >> 3;
        const int bit  = sy & 7;
        for (int sx = 0; sx < w; sx++) {
            const bool lit = (data[page * w + sx] & (1u << bit)) != 0u;
            if (!lit && transparent) {
                continue;
            }
            for (int dy = 0; dy < sy_step; dy++) {
                for (int dx = 0; dx < sx_step; dx++) {
                    fb_set_pixel(x + sx * sx_step + dx, y + sy * sy_step + dy, lit ? fg_on : bg_on);
                }
            }
        }
    }

    panel_dirty_rows(y, y + ch - 1);
}

// 放大画串，费行数
int panel_draw_text_cn_scaled(int x, int y, const char *utf8, uint16_t fg, uint16_t bg, int scale) {
    if (utf8 == NULL || scale < 1) {
        return x;
    }
    if (scale == 1) {
        return panel_draw_text_cn(x, y, utf8, fg, bg);
    }

    uint32_t    cp  = 0;
    int         adv = 0;
    const char *p   = utf8;

    while (*p != '\0') {
        if (!panel_font_utf8_next(p, &cp, &adv)) {
            break;
        }

        if (cp < 0x80u) {
            const uint8_t *g = panel_font_ascii_lookup((char)cp);
            if (g != NULL) {
                draw_glyph_scaled(x, y, g, PANEL_FONT_ASCII_W, PANEL_FONT_ASCII_H, fg, bg,
                                  PANEL_ASCII_CELL_W * scale, PANEL_ASCII_CELL_H * scale);
            }
            x += PANEL_ASCII_CELL_W * scale;
        } else {
            const uint8_t *g = panel_font_cn_lookup(cp);
            if (g != NULL) {
                draw_glyph_scaled(x, y, g, PANEL_FONT_CN_W, PANEL_FONT_CN_H, fg, bg, PANEL_CN_CELL_W * scale,
                                  PANEL_CN_CELL_H * scale);
            } else {
                panel_draw_rect(x, y, PANEL_CN_CELL_W * scale, PANEL_CN_CELL_H * scale, fg);
            }
            x += PANEL_CN_CELL_W * scale;
        }

        p += adv;
    }

    return x;
}
