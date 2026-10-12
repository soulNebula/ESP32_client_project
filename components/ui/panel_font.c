// c
// 字模查表
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "panel_font.h"

static const char *TAG = "FONT";

// 两张表都按码点升序

extern const panel_glyph_t panel_cn_glyphs[];
extern const int           panel_cn_glyph_count;

// 补充表就地在外部声明
extern const panel_glyph_t panel_cn_extra[];
extern const int           panel_cn_extra_count;

const uint8_t *panel_font_ascii_lookup(char ch) {
    // 先转无符号再查
    unsigned char u = (unsigned char)ch;
    if (u < 0x20u || u > 0x7Eu) {
        return NULL;
    }
    return panel_ascii_8x16[u - 0x20u];
}

// 二分找汉字
static const uint8_t *cn_bsearch(const panel_glyph_t *table, int count, uint32_t cp) {
    // 空表先挡掉
    if (table == NULL || count <= 0) {
        return NULL;
    }
    const panel_glyph_t *lo = table;
    const panel_glyph_t *hi = table + (count - 1);

    while (lo <= hi) {
        const panel_glyph_t *mid = lo + ((hi - lo) / 2);
        if (mid->unicode == cp) {
            return mid->data;
        }
        if (mid->unicode < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

// 按码点查汉字
const uint8_t *panel_font_cn_lookup(uint32_t codepoint) {
    // 重复时以基础表为准
    const uint8_t *hit = cn_bsearch(panel_cn_glyphs, panel_cn_glyph_count, codepoint);
    if (hit != NULL) {
        return hit;
    }
    return cn_bsearch(panel_cn_extra, panel_cn_extra_count, codepoint);
}

bool panel_font_utf8_next(const char *s, uint32_t *cp, int *adv) {
    if (s == NULL || cp == NULL || adv == NULL) {
        if (adv != NULL) {
            // adv 不能给 0
            *adv = 1;
        }
        if (cp != NULL) {
            *cp = 0;
        }
        return false;
    }

    const uint8_t *p  = (const uint8_t *)s;
    uint8_t        b0 = p[0];

    if (b0 == 0x00u) {
        *cp  = 0;
        *adv = 1;
        return false;
    }

    if (b0 < 0x80u) {
        *cp  = b0;
        *adv = 1;
        return true;
    }

    int      len   = 0;
    uint32_t value = 0;

    // 首字节定长度
    if ((b0 & 0xE0u) == 0xC0u) {
        len   = 2;
        value = (uint32_t)(b0 & 0x1Fu);
    } else if ((b0 & 0xF0u) == 0xE0u) {
        len   = 3;
        value = (uint32_t)(b0 & 0x0Fu);
    } else if ((b0 & 0xF8u) == 0xF0u) {
        len   = 4;
        value = (uint32_t)(b0 & 0x07u);
    } else {
        // 非法首字节
        *cp  = 0;
        *adv = 1;
        return false;
    }

    for (int i = 1; i < len; i++) {
        uint8_t bi = p[i];
        if (bi == 0x00u) {
            // 序列被截断
            *cp  = 0;
            *adv = 1;
            return false;
        }
        if ((bi & 0xC0u) != 0x80u) {
            *cp  = 0;
            *adv = 1;
            return false;
        }
        value = (value << 6) | (uint32_t)(bi & 0x3Fu);
    }

    // 防超长编码
    bool ok = true;
    if (len == 2 && value < 0x80u) {
        ok = false;
    } else if (len == 3 && value < 0x800u) {
        ok = false;
    } else if (len == 4 && (value < 0x10000u || value > 0x10FFFFu)) {
        ok = false;
    } else if (value >= 0xD800u && value <= 0xDFFFu) {
        ok = false;
    }

    if (!ok) {
        *cp  = 0;
        *adv = 1;
        return false;
    }

    *cp  = value;
    *adv = (int)len;
    return true;
}

// 字库共多少汉字
int panel_font_cn_count(void) {
    return panel_cn_glyph_count + panel_cn_extra_count;
}

// 两张表是否都齐
bool panel_font_ready(void) {
    // 缺补充表会全变豆腐块
    return (panel_cn_glyph_count > 0) && (panel_cn_extra_count > 0);
}

// 保住 TAG
static void __attribute__((unused)) font_log_self_check(void) {
    ESP_LOGD(TAG, "cn glyphs: base=%d extra=%d total=%d ready=%d", panel_cn_glyph_count, panel_cn_extra_count,
             panel_font_cn_count(), (int)panel_font_ready());
}
