// c
// OLED 驱动与画图
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_common.h"

// 单色两态
#define PANEL_BLACK       0
#define PANEL_WHITE       1
#define PANEL_TRANSPARENT 2

// 挂 U8g2 的 SSD1306 128x64
app_err_t panel_init(void);

bool panel_ready(void);

int panel_width(void);

int panel_height(void);

// 一帧的开始与结束
void panel_begin_frame(void);

void panel_end_frame(void);

void panel_clear(int color);

void panel_flush_all(void);

void panel_fill_rect(int x, int y, int w, int h, int color);

void panel_draw_hline(int x, int y, int w, int color);

void panel_draw_vline(int x, int y, int h, int color);

void panel_draw_rect(int x, int y, int w, int h, int color);

// 画 ASCII，返回结束 x
int panel_draw_text(int x, int y, const char *s, int fg, int bg);

// 画中文，返回结束 x
int panel_draw_text_cn(int x, int y, const char *utf8, int fg, int bg);

// 放大 scale 倍
int panel_draw_text_cn_scaled(int x, int y, const char *utf8, int fg, int bg, int scale);

void panel_draw_text_cn_center(int x, int y, int w, int h, const char *utf8, int fg, int bg);

// 一行字的像素宽度，实现在 panel_text.h
int panel_text_width(const char *utf8);

// 中文字库几个字
int panel_font_cn_count(void);

bool panel_font_ready(void);

// 重发初始化，屏掉了不用复位
app_err_t panel_retry(void);

// 只发 DISPOFF/DISPON
void panel_display_test(bool on);

// 纯色轮播加边框
void panel_selftest(int hold_ms);

// OLED 无背光，改成闪烁
void panel_blink_backlight(int times, int on_ms, int off_ms);
