// c
// 字库接口
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PANEL_FONT_CN_W 16
#define PANEL_FONT_CN_H 16
#define PANEL_FONT_ASCII_W 8
#define PANEL_FONT_ASCII_H 16

// 一个汉字 32 字节
#define PANEL_FONT_CN_BYTES 32
#define PANEL_FONT_ASCII_BYTES 16

// 只收可见 ASCII
// 查不到给 NULL
const uint8_t *panel_font_ascii_lookup(char ch);

// 查不到给 NULL
const uint8_t *panel_font_cn_lookup(uint32_t codepoint);

// 解 UTF-8 字
// 失败 adv 填 1
bool panel_font_utf8_next(const char *s, uint32_t *cp, int *adv);

int panel_font_cn_count(void);

bool panel_font_ready(void);
