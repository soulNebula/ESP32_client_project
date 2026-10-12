// c
// 字库接口
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_FONT_CN_W 16
#define PANEL_FONT_CN_H 16
#define PANEL_FONT_ASCII_W 8
#define PANEL_FONT_ASCII_H 16

// 一个汉字 32 字节
#define PANEL_FONT_CN_BYTES 32
#define PANEL_FONT_ASCII_BYTES 16

typedef struct {
    uint32_t unicode;
    // 先 page 再列
    uint8_t data[PANEL_FONT_CN_BYTES];
} panel_glyph_t;

// 下标=码-0x20
extern const uint8_t panel_ascii_8x16[95][PANEL_FONT_ASCII_BYTES];

// 按码点排的汉字表
extern const panel_glyph_t panel_cn_glyphs[];

// 汉字个数
extern const int panel_cn_glyph_count;

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

#ifdef __cplusplus
}
#endif
