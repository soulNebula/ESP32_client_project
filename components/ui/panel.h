// c
// 单色屏接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 屏宽像素
#define PANEL_EXPECT_W 128
// 屏高像素
#define PANEL_EXPECT_H 64

// 改 1 变小字
#define PANEL_FONT_SMALL_ACTIVE 0

#define PANEL_RGB565(r8, g8, b8) ((uint16_t)((((r8) & 0xF8) << 8) | (((g8) & 0xFC) << 3) | ((b8) >> 3)))

// 单色屏上只有它算灭
#define PANEL_BLACK PANEL_RGB565(0, 0, 0)
#define PANEL_WHITE PANEL_RGB565(255, 255, 255)
// 非黑即亮
#define PANEL_RED      PANEL_RGB565(255, 0, 0)
#define PANEL_GREEN    PANEL_RGB565(0, 255, 0)
#define PANEL_BLUE     PANEL_RGB565(0, 0, 255)
#define PANEL_YELLOW   PANEL_RGB565(255, 255, 0)
#define PANEL_ORANGE   PANEL_RGB565(255, 128, 0)
#define PANEL_CYAN     PANEL_RGB565(0, 255, 255)
#define PANEL_GRAY     PANEL_RGB565(128, 128, 128)
#define PANEL_DARKGRAY PANEL_RGB565(48, 48, 48)
#define PANEL_NAVY     PANEL_RGB565(0, 0, 128)

// 可重复调用
esp_err_t panel_init(void);

int panel_width(void);

// 屏高
int panel_height(void);

// 关了真全黑
void panel_backlight(bool on);

bool panel_ready(void);

// 整屏操作

// 立即推屏
void panel_clear(uint16_t color);

void panel_flush_all(void);

void panel_begin_frame(void);

// 只发改过的页
void panel_end_frame(void);

// 画在显存上

// 出屏忽略
void panel_draw_pixel(int x, int y, uint16_t color);

void panel_fill_rect(int x, int y, int w, int h, uint16_t color);

void panel_draw_rect(int x, int y, int w, int h, uint16_t color);

void panel_draw_hline(int x, int y, int w, uint16_t color);

void panel_draw_vline(int x, int y, int h, uint16_t color);

void panel_fill_circle(int cx, int cy, int r, uint16_t color);

// ASCII 8×16
int panel_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg);

// 缺字画方框
int panel_draw_text_cn(int x, int y, const char *utf8, uint16_t fg, uint16_t bg);

// 不填背景
#define PANEL_TRANSPARENT 0xFFFFu

// 别写死字宽
int panel_text_width(const char *utf8);

// 只支持整数倍
int panel_draw_text_cn_scaled(int x, int y, const char *utf8, uint16_t fg, uint16_t bg, int scale);

void panel_draw_text_cn_center(int x, int y, int w, int h, const char *utf8, uint16_t fg, uint16_t bg);

// 开机自检用

// 会阻塞不刷界面
void panel_selftest(int hold_ms);

// 闪整屏不动显存
void panel_blink_backlight(int times, int on_ms, int off_ms);

// 只发开关命令
void panel_display_test(bool on);

#ifdef __cplusplus
}
#endif
