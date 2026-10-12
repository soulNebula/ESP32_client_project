// c
// 字库查表与 UTF-8 解码
//
// ★ 本文件是【唯一】包含 panel_font_data.h 的地方 —— 那张字模表是 static 的，
//   被第二个 .cpp 包含就会在 flash 里多存一份。

#include <string.h>

#include "panel_font.h"

// 字模表按码点升序排好了
#include "panel_font_data.h"

static const char *TAG = "FONT";

// 只收可见 ASCII
// 查不到给 NULL
const uint8_t *panel_font_ascii_lookup(char ch) {
    const unsigned char c = (unsigned char)ch;
    if (c < 0x20u || c > 0x7Eu) {
        return NULL;
    }
    return PANEL_ASCII_8X16[c - 0x20u];
}

// 查不到给 NULL
const uint8_t *panel_font_cn_lookup(uint32_t codepoint) {
    int lo = 0;
    int hi = PANEL_CN_GLYPH_COUNT - 1;

    // 二分，表是升序的
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t cp = PANEL_CN_GLYPHS[mid].unicode;

        if (cp == codepoint) {
            return PANEL_CN_GLYPHS[mid].data;
        }
        if (cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

// 解 UTF-8 字
// 失败 adv 填 1
bool panel_font_utf8_next(const char *s, uint32_t *cp, int *adv) {
    if (s == NULL || cp == NULL || adv == NULL) {
        return false;
    }

    const unsigned char *p = (const unsigned char *)s;
    const unsigned char  c = p[0];

    if (c < 0x80u) {
        // ASCII
        *cp  = c;
        *adv = 1;
        return true;
    }

    // 首字节的高位有几个 1 就有几字节
    int n = 0;
    if ((c & 0xE0u) == 0xC0u) {
        n     = 2;
        *cp   = (uint32_t)(c & 0x1Fu);
    } else if ((c & 0xF0u) == 0xE0u) {
        n     = 3;
        *cp   = (uint32_t)(c & 0x0Fu);
    } else if ((c & 0xF8u) == 0xF0u) {
        n     = 4;
        *cp   = (uint32_t)(c & 0x07u);
    } else {
        // 续字节跑到了开头
        *cp  = 0;
        *adv = 1;
        return false;
    }

    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            // 截断了
            *cp  = 0;
            *adv = 1;
            return false;
        }
        *cp = (*cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }

    *adv = n;
    return true;
}

int panel_font_cn_count(void) {
    return PANEL_CN_GLYPH_COUNT;
}

bool panel_font_ready(void) {
    return PANEL_CN_GLYPH_COUNT > 0;
}
