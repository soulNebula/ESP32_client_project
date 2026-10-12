// c
// 模式与阈值存储

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Preferences.h"

#include "automation.h"

static const char *TAG = "POLICY";

// NVS 记号
#define AUTO_CFG_MAGIC 0x53484131u

// 存储整包
typedef struct {
    uint32_t magic;
    // 改结构体要 +1
    uint16_t version;
    // 防新旧错位
    uint16_t size;
    auto_cfg_t cfg;
} auto_blob_t;

// 不能悄悄截断
static_assert(sizeof(auto_cfg_t) < 65535, "auto_cfg_t 太大，blob 的 size 字段装不下");

// 默认低电平触发（现场实测本模块默认高电平=停、拉低才响）
#define APP_DEF_BUZZER_ACTIVE_LOW 1

// 默认自动模式
#define APP_DEF_MODE_AUTO 1

// 静态初值不能省
static auto_cfg_t s_cfg = {
    .mode               = AUTO_MODE_AUTO,
    .auto_light_enable  = true,
    .light_on_pct       = AUTO_DEF_LIGHT_ON_PCT,
    .light_off_pct      = AUTO_DEF_LIGHT_OFF_PCT,
    .auto_buzzer_enable = true,
    .buzzer_on_c        = AUTO_DEF_BUZZER_ON_C,
    .buzzer_off_c       = AUTO_DEF_BUZZER_OFF_C,
    .temp_alert_c       = AUTO_DEF_TEMP_ALERT_C,
    .rain_pct           = AUTO_DEF_RAIN_PCT,
    .door_light_cascade = true,
    // 跟随编译期宏
    .buzzer_active_low = (APP_DEF_BUZZER_ACTIVE_LOW != 0),
};

static bool s_buzzer_muted;
static bool s_inited;

// 不分大小写比字符串
static bool str_ieq(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool parse_bool(const char *s, bool *out) {
    if (str_ieq(s, "1") || str_ieq(s, "true") || str_ieq(s, "on") || str_ieq(s, "yes")) {
        *out = true;
        return true;
    }
    if (str_ieq(s, "0") || str_ieq(s, "false") || str_ieq(s, "off") || str_ieq(s, "no")) {
        *out = false;
        return true;
    }
    return false;
}

// 有垃圾就拒收
static bool parse_float(const char *s, float *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }

    char      *end = NULL;
    const float v  = strtof(s, &end);
    if (end == s) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    // 容忍 20%
    if (*end == '%') {
        end++;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    // NaN 一律拒收
    if (isnan(v) || isinf(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_mode(const char *s, auto_mode_t *out) {
    if (str_ieq(s, "auto") || str_ieq(s, "1") || str_ieq(s, "true") || str_ieq(s, "on") || str_ieq(s, "yes")) {
        *out = AUTO_MODE_AUTO;
        return true;
    }
    if (str_ieq(s, "manual") || str_ieq(s, "0") || str_ieq(s, "false") || str_ieq(s, "off") ||
        str_ieq(s, "no")) {
        *out = AUTO_MODE_MANUAL;
        return true;
    }
    return false;
}

// 定点格式化，别用浮点
static const char *fmt1(char *dst, size_t n, float v) {
    if (!(v >= 0.0f)) {
        v = 0.0f;
    }
    if (v > 9999.0f) {
        v = 9999.0f;
    }
    const int t = (int)(v * 10.0f + 0.5f);
    (void)snprintf(dst, n, "%d.%d", t / 10, t % 10);
    return dst;
}

// 采纳前先体检
static bool cfg_sane(const auto_cfg_t *c) {
    if (c->mode != AUTO_MODE_AUTO && c->mode != AUTO_MODE_MANUAL) {
        return false;
    }
    if (!(c->light_on_pct > 0.0f && c->light_on_pct < 100.0f)) {
        return false;
    }
    if (!(c->light_off_pct > c->light_on_pct && c->light_off_pct < 100.0f)) {
        return false;
    }
    if (!(c->buzzer_on_c > 0.0f && c->buzzer_on_c < 80.0f)) {
        return false;
    }
    if (!(c->buzzer_off_c > 0.0f && c->buzzer_off_c < c->buzzer_on_c)) {
        return false;
    }
    if (!(c->temp_alert_c > 0.0f && c->temp_alert_c < 80.0f)) {
        return false;
    }
    if (!(c->rain_pct > 0.0f && c->rain_pct < 100.0f)) {
        return false;
    }
    return true;
}

// 填编译期默认值
static void cfg_set_defaults(auto_cfg_t *c) {
    memset(c, 0, sizeof(*c));

    c->mode              = APP_DEF_MODE_AUTO ? AUTO_MODE_AUTO : AUTO_MODE_MANUAL;
    c->auto_light_enable = true;
    c->light_on_pct      = AUTO_DEF_LIGHT_ON_PCT;
    c->light_off_pct     = AUTO_DEF_LIGHT_OFF_PCT;

    c->auto_buzzer_enable = true;
    c->buzzer_on_c        = AUTO_DEF_BUZZER_ON_C;
    c->buzzer_off_c       = AUTO_DEF_BUZZER_OFF_C;

    c->temp_alert_c = AUTO_DEF_TEMP_ALERT_C;

    c->rain_pct = AUTO_DEF_RAIN_PCT;

    c->door_light_cascade = true;

    c->buzzer_active_low = (APP_DEF_BUZZER_ACTIVE_LOW != 0);
}

static app_err_t cfg_reject(const char *key, const char *val, const char *why) {
    LOGW(TAG, "cfg %s %s 被拒: %s", key, val, why);
    return APP_ERR_INVALID_STATE;
}

// 0.05℃ 内算相等
static bool cfg_temp_is(float a, float b) {
    const float d = a - b;
    return (d < 0.05f) && (d > -0.05f);
}

// 旧默认 35/33
static void auto_load_migrate_legacy_alarm(void) {
    const bool legacy = cfg_temp_is(s_cfg.buzzer_on_c, AUTO_OLD_BUZZER_ON_C) &&
                        cfg_temp_is(s_cfg.buzzer_off_c, AUTO_OLD_BUZZER_OFF_C);
    if (!legacy) {
        return;
    }
    if (cfg_temp_is(s_cfg.buzzer_on_c, AUTO_DEF_BUZZER_ON_C) &&
        cfg_temp_is(s_cfg.buzzer_off_c, AUTO_DEF_BUZZER_OFF_C)) {
        return;
    }

    s_cfg.buzzer_on_c  = AUTO_DEF_BUZZER_ON_C;
    s_cfg.buzzer_off_c = AUTO_DEF_BUZZER_OFF_C;

    LOGW(TAG,
         "★ 检测到出厂默认的旧蜂鸣器阈值 %.1f/%.1fC，"
         "已按新需求升级为 %.1f/%.1fC（32℃ 数字闪烁 + 蜂鸣器提示）；"
         "如需自定义请用 cfg buzzer_on_c / buzzer_off_c",
         AUTO_OLD_BUZZER_ON_C, AUTO_OLD_BUZZER_OFF_C, AUTO_DEF_BUZZER_ON_C, AUTO_DEF_BUZZER_OFF_C);

    (void)auto_policy_save();
}

// 读不到就用默认
static app_err_t auto_load(void) {
    Preferences prefs;
    if (!prefs.begin(AUTO_NVS_NAMESPACE, true)) {
        // 首次上电正常
        LOGI(TAG, "NVS 里没有配置/版本不符，使用默认值");
        return APP_ERR_FAIL;
    }

    auto_blob_t blob;
    memset(&blob, 0, sizeof(blob));

    const size_t got = prefs.getBytes(AUTO_NVS_KEY_CFG, &blob, sizeof(blob));
    prefs.end();

    if (got != sizeof(blob)) {
        LOGI(TAG, "NVS 里没有配置/版本不符，使用默认值 (长度 %u)", (unsigned)got);
        return APP_ERR_FAIL;
    }
    // 校验不过整块丢
    if (blob.magic != AUTO_CFG_MAGIC) {
        LOGW(TAG, "NVS 里没有配置/版本不符，使用默认值 (magic 0x%08X)", (unsigned)blob.magic);
        return APP_ERR_INVALID_STATE;
    }
    if (blob.version != (uint16_t)AUTO_CFG_VERSION) {
        LOGW(TAG, "NVS 里没有配置/版本不符，使用默认值 (版本 %u != %u)", (unsigned)blob.version,
             (unsigned)AUTO_CFG_VERSION);
        return APP_ERR_INVALID_STATE;
    }
    if (blob.size != (uint16_t)sizeof(auto_cfg_t)) {
        LOGW(TAG, "NVS 里没有配置/版本不符，使用默认值 (大小 %u != %u)", (unsigned)blob.size,
             (unsigned)sizeof(auto_cfg_t));
        return APP_ERR_INVALID_STATE;
    }
    // 头对也要查数值
    if (!cfg_sane(&blob.cfg)) {
        LOGW(TAG, "NVS 里的配置数值不合理（阈值越界或迟滞反转），使用默认值");
        return APP_ERR_INVALID_STATE;
    }

    s_cfg = blob.cfg;

    LOGI(TAG, "已从 NVS 载入配置");
    auto_load_migrate_legacy_alarm();
    return APP_OK;
}

// 铺默认再读配置
app_err_t auto_policy_init(void) {
    if (s_inited) {
        return APP_OK;
    }

    // 先铺默认再读
    cfg_set_defaults(&s_cfg);
    s_buzzer_muted = false;

    if (auto_load() == APP_OK) {
        // 数值体检在 auto_load 里做过了
    }

    s_inited = true;

    LOGI(TAG, "init ok: 模式=%s, 光控(%.0f/%.0f%%, %s), 蜂鸣器(%.0f/%.0fC, %s), 开门亮灯=%s",
         (s_cfg.mode == AUTO_MODE_AUTO) ? "自动" : "手动", s_cfg.light_on_pct, s_cfg.light_off_pct,
         s_cfg.auto_light_enable ? "启用" : "停用", s_cfg.buzzer_on_c, s_cfg.buzzer_off_c,
         s_cfg.auto_buzzer_enable ? "启用" : "停用", s_cfg.door_light_cascade ? "开" : "关");
    // 一律返回 OK
    return APP_OK;
}

app_err_t auto_policy_save(void) {
    auto_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic   = AUTO_CFG_MAGIC;
    blob.version = (uint16_t)AUTO_CFG_VERSION;
    blob.size    = (uint16_t)sizeof(auto_cfg_t);
    blob.cfg     = s_cfg;

    Preferences prefs;
    if (!prefs.begin(AUTO_NVS_NAMESPACE, false)) {
        LOGW(TAG, "打开 NVS(%s) 失败: 配置无法保存", AUTO_NVS_NAMESPACE);
        return APP_ERR_FAIL;
    }

    const size_t put = prefs.putBytes(AUTO_NVS_KEY_CFG, &blob, sizeof(blob));
    prefs.end();

    if (put != sizeof(blob)) {
        LOGW(TAG, "配置保存失败: 改动仍在内存里生效");
        return APP_ERR_FAIL;
    }

    LOGD(TAG, "配置已保存到 NVS");
    return APP_OK;
}

// 切模式并落盘
app_err_t auto_policy_set_mode(auto_mode_t mode) {
    if (mode != AUTO_MODE_AUTO && mode != AUTO_MODE_MANUAL) {
        LOGW(TAG, "set_mode: 非法模式值 %d", (int)mode);
        return APP_ERR_INVALID_ARG;
    }

    const auto_mode_t old = s_cfg.mode;
    s_cfg.mode            = mode;

    if (old == mode) {
        // 值没变就不落盘
        return APP_OK;
    }

    LOGI(TAG, "模式切换: %s -> %s（%s）", auto_policy_mode_name_cn(old), auto_policy_mode_name_cn(mode),
         (mode == AUTO_MODE_AUTO) ? "光控生效" : "光控停用，灯只听语音/手机");
    // 模式掉电不丢
    (void)auto_policy_save();
    return APP_OK;
}

auto_mode_t auto_policy_get_mode(void) {
    return s_cfg.mode;
}

// 现在是否自动
bool auto_policy_is_auto(void) {
    return auto_policy_get_mode() == AUTO_MODE_AUTO;
}

const char *auto_policy_mode_name(auto_mode_t mode) {
    return (mode == AUTO_MODE_AUTO) ? "auto" : "manual";
}

const char *auto_policy_mode_name_cn(auto_mode_t mode) {
    return (mode == AUTO_MODE_AUTO) ? "自动模式" : "手动模式";
}

const auto_cfg_t *auto_policy_cfg(void) {
    // 别强转去写
    return &s_cfg;
}

// 恢复后落盘
app_err_t auto_policy_reset_defaults(void) {
    auto_cfg_t d;
    cfg_set_defaults(&d);

    s_cfg = d;
    // 顺手清静音锁
    s_buzzer_muted = false;

    LOGW(TAG,
         "已恢复默认阈值: 模式=%s, 光控=%.1f/%.1f%%, 蜂鸣器=%.1f/%.1fC, "
         "温度告警=%.1fC, 雨滴=%.1f%%, 开门亮灯=%s",
         auto_policy_mode_name_cn(d.mode), d.light_on_pct, d.light_off_pct, d.buzzer_on_c, d.buzzer_off_c,
         d.temp_alert_c, d.rain_pct, d.door_light_cascade ? "开" : "关");
    return auto_policy_save();
}

app_err_t auto_policy_set_threshold(const char *key, const char *val) {
    if (key == NULL || val == NULL) {
        return APP_ERR_INVALID_ARG;
    }

    // 改副本再提交
    auto_cfg_t c = s_cfg;

    float       v = 0.0f;
    bool        b = false;
    auto_mode_t m = AUTO_MODE_MANUAL;

    // 认 auto 别名
    if (str_ieq(key, "mode") || str_ieq(key, "auto")) {
        if (!parse_mode(val, &m)) {
            return cfg_reject(key, val, "只认 auto/manual/1/0/true/false");
        }
        if (c.mode == m) {
            return APP_OK;
        }
        c.mode = m;
        LOGI(TAG, "cfg mode = %s", auto_policy_mode_name(m));
        s_cfg = c;
        return auto_policy_save();
    }

    if (str_ieq(key, "light_on_pct")) {
        if (!parse_float(val, &v) || v <= 0.0f || v >= 100.0f) {
            return cfg_reject(key, val, "光照阈值必须落在 (0,100) 开区间内");
        }
        // 先调 on 会暂反转
        if (v >= c.light_off_pct) {
            LOGW(TAG,
                 "提示: light_on_pct(%.1f) ≥ light_off_pct(%.1f)，"
                 "迟滞区已反转，请接着把 light_off_pct 调到更大",
                 v, c.light_off_pct);
        }
        if (v == c.light_on_pct) {
            return APP_OK;
        }
        c.light_on_pct = v;
        LOGI(TAG, "cfg light_on_pct = %.1f", v);
        s_cfg = c;
        return auto_policy_save();
    }
    if (str_ieq(key, "light_off_pct")) {
        if (!parse_float(val, &v) || v <= 0.0f || v >= 100.0f) {
            return cfg_reject(key, val, "光照阈值必须落在 (0,100) 开区间内");
        }
        // off 要大于 on
        if (v <= c.light_on_pct) {
            LOGW(TAG, "cfg light_off_pct %s 被拒: %.1f 必须 > light_on_pct(%.1f)", val, v, c.light_on_pct);
            return APP_ERR_INVALID_STATE;
        }
        if (v == c.light_off_pct) {
            return APP_OK;
        }
        c.light_off_pct = v;
        LOGI(TAG, "cfg light_off_pct = %.1f", v);
        s_cfg = c;
        return auto_policy_save();
    }
    if (str_ieq(key, "auto_light_enable")) {
        if (!parse_bool(val, &b)) {
            return cfg_reject(key, val, "只认 1/0/true/false/on/off/yes/no");
        }
        if (b == c.auto_light_enable) {
            return APP_OK;
        }
        c.auto_light_enable = b;
        LOGI(TAG, "cfg auto_light_enable = %s", b ? "true" : "false");
        s_cfg = c;
        return auto_policy_save();
    }

    if (str_ieq(key, "buzzer_on_c")) {
        if (!parse_float(val, &v) || v <= 0.0f || v >= 80.0f) {
            return cfg_reject(key, val, "蜂鸣器温度阈值必须落在 (0,80) 开区间内");
        }
        // 同光照处理
        if (v <= c.buzzer_off_c) {
            LOGW(TAG,
                 "提示: buzzer_on_c(%.1f) ≤ buzzer_off_c(%.1f)，"
                 "迟滞区已反转，请接着把 buzzer_off_c 调到更小",
                 v, c.buzzer_off_c);
        }
        if (v == c.buzzer_on_c) {
            return APP_OK;
        }
        c.buzzer_on_c = v;
        LOGI(TAG, "cfg buzzer_on_c = %.1f", v);
        s_cfg = c;
        return auto_policy_save();
    }
    if (str_ieq(key, "buzzer_off_c")) {
        if (!parse_float(val, &v) || v <= 0.0f || v >= 80.0f) {
            return cfg_reject(key, val, "蜂鸣器温度阈值必须落在 (0,80) 开区间内");
        }
        // off 要小于 on
        if (v >= c.buzzer_on_c) {
            LOGW(TAG, "cfg buzzer_off_c %s 被拒: %.1f 必须 < buzzer_on_c(%.1f)", val, v, c.buzzer_on_c);
            return APP_ERR_INVALID_STATE;
        }
        if (v == c.buzzer_off_c) {
            return APP_OK;
        }
        c.buzzer_off_c = v;
        LOGI(TAG, "cfg buzzer_off_c = %.1f", v);
        s_cfg = c;
        return auto_policy_save();
    }
    if (str_ieq(key, "auto_buzzer_enable")) {
        if (!parse_bool(val, &b)) {
            return cfg_reject(key, val, "只认 1/0/true/false/on/off/yes/no");
        }
        if (b == c.auto_buzzer_enable) {
            return APP_OK;
        }
        c.auto_buzzer_enable = b;
        LOGI(TAG, "cfg auto_buzzer_enable = %s", b ? "true" : "false");
        s_cfg = c;
        return auto_policy_save();
    }

    if (str_ieq(key, "temp_alert_c")) {
        if (!parse_float(val, &v) || v <= 0.0f || v >= 80.0f) {
            return cfg_reject(key, val, "温度告警阈值必须落在 (0,80) 开区间内");
        }
        if (v == c.temp_alert_c) {
            return APP_OK;
        }
        c.temp_alert_c = v;
        LOGI(TAG, "cfg temp_alert_c = %.1f", v);
        s_cfg = c;
        return auto_policy_save();
    }
    if (str_ieq(key, "rain_pct")) {
        if (!parse_float(val, &v) || v <= 0.0f || v >= 100.0f) {
            return cfg_reject(key, val, "雨滴阈值必须落在 (0,100) 开区间内");
        }
        if (v == c.rain_pct) {
            return APP_OK;
        }
        c.rain_pct = v;
        LOGI(TAG, "cfg rain_pct = %.1f", v);
        s_cfg = c;
        return auto_policy_save();
    }

    if (str_ieq(key, "door_light_cascade")) {
        if (!parse_bool(val, &b)) {
            return cfg_reject(key, val, "只认 1/0/true/false/on/off/yes/no");
        }
        if (b == c.door_light_cascade) {
            return APP_OK;
        }
        c.door_light_cascade = b;
        LOGI(TAG, "cfg door_light_cascade = %s", b ? "true" : "false");
        s_cfg = c;
        return auto_policy_save();
    }

    // 现场反了就改这个
    if (str_ieq(key, "buzzer_active_low")) {
        if (!parse_bool(val, &b)) {
            return cfg_reject(key, val, "只认 1/0/true/false/on/off/yes/no");
        }
        if (b == c.buzzer_active_low) {
            return APP_OK;
        }
        c.buzzer_active_low = b;
        LOGI(TAG, "cfg buzzer_active_low = %s（%s）", b ? "true" : "false",
             b ? "低电平触发：PNP 型模块用这个" : "高电平触发：普通模块用这个");
        s_cfg = c;
        return auto_policy_save();
    }

    LOGW(TAG,
         "cfg %s 不认识（支持: mode/auto/light_on_pct/light_off_pct/"
         "auto_light_enable/buzzer_on_c/buzzer_off_c/auto_buzzer_enable/"
         "temp_alert_c/rain_pct/door_light_cascade/buzzer_active_low）",
         key);
    return APP_ERR_INVALID_ARG;
}

void auto_policy_dump(void) {
    LOGI(TAG, "  mode               = %s", auto_policy_mode_name(s_cfg.mode));
    LOGI(TAG, "  light_on_pct       = %.1f", s_cfg.light_on_pct);
    LOGI(TAG, "  light_off_pct      = %.1f", s_cfg.light_off_pct);
    LOGI(TAG, "  auto_light_enable  = %s", s_cfg.auto_light_enable ? "true" : "false");
    LOGI(TAG, "  buzzer_on_c        = %.1f", s_cfg.buzzer_on_c);
    LOGI(TAG, "  buzzer_off_c       = %.1f", s_cfg.buzzer_off_c);
    LOGI(TAG, "  auto_buzzer_enable = %s", s_cfg.auto_buzzer_enable ? "true" : "false");
    LOGI(TAG, "  temp_alert_c       = %.1f", s_cfg.temp_alert_c);
    LOGI(TAG, "  rain_pct           = %.1f", s_cfg.rain_pct);
    LOGI(TAG, "  door_light_cascade = %s", s_cfg.door_light_cascade ? "true" : "false");
    LOGI(TAG, "  buzzer_active_low  = %s（%s）", s_cfg.buzzer_active_low ? "true" : "false",
         s_cfg.buzzer_active_low ? "低电平触发" : "高电平触发");
}

int auto_policy_cfg_to_json(char *buf, int len) {
    if (buf == NULL || len <= 0) {
        return -1;
    }
    buf[0] = '\0';

    // 定点字符串，别让 %f 上阵
    char s_on[16], s_off[16], s_bon[16], s_boff[16], s_tal[16], s_rain[16];
    fmt1(s_on, sizeof(s_on), s_cfg.light_on_pct);
    fmt1(s_off, sizeof(s_off), s_cfg.light_off_pct);
    fmt1(s_bon, sizeof(s_bon), s_cfg.buzzer_on_c);
    fmt1(s_boff, sizeof(s_boff), s_cfg.buzzer_off_c);
    fmt1(s_tal, sizeof(s_tal), s_cfg.temp_alert_c);
    fmt1(s_rain, sizeof(s_rain), s_cfg.rain_pct);

    const char *mode = auto_policy_mode_name(s_cfg.mode);

    // 手写 JSON 省堆
    const int n = snprintf(buf, (size_t)len,
                           "{\"mode\":\"%s\",\"light_on_pct\":%s,\"light_off_pct\":%s,"
                           "\"auto_light_enable\":%s,\"buzzer_on_c\":%s,\"buzzer_off_c\":%s,"
                           "\"auto_buzzer_enable\":%s,\"temp_alert_c\":%s,\"rain_pct\":%s,"
                           "\"door_light_cascade\":%s,\"buzzer_active_low\":%s}",
                           mode, s_on, s_off, s_cfg.auto_light_enable ? "true" : "false", s_bon, s_boff,
                           s_cfg.auto_buzzer_enable ? "true" : "false", s_tal, s_rain,
                           s_cfg.door_light_cascade ? "true" : "false", s_cfg.buzzer_active_low ? "true" : "false");

    if (n < 0 || n >= len) {
        // 绝不发半截 JSON
        LOGW(TAG, "配置 JSON 缓冲太小(需 %d，有 %d)，退化为最小 JSON", n, len);
        const int m = snprintf(buf, (size_t)len, "{\"mode\":\"%s\"}", mode);
        if (m < 0 || m >= len) {
            buf[0] = '\0';
            LOGE(TAG, "缓冲太小(%d)，连最小 JSON 都放不下，本次不上报", len);
            return -1;
        }
        return m;
    }
    return n;
}

void auto_policy_mute_buzzer(void) {
    // 置锁不是关一次
    s_buzzer_muted = true;

    LOGI(TAG, "蜂鸣器静音锁【置位】：温度降到 %.1f℃ 以下才自动解除", s_cfg.buzzer_off_c);
}

void auto_policy_unmute_buzzer(void) {
    const bool was = s_buzzer_muted;
    s_buzzer_muted = false;

    // 只解锁不主动响
    LOGI(TAG, "蜂鸣器静音锁【解除】（不改蜂鸣器现状，是否响由温度规则决定）%s",
         was ? "" : " —— 提示: 原本就没置位");
}

bool auto_policy_buzzer_muted(void) {
    return s_buzzer_muted;
}

void auto_policy_clear_mute(void) {
    // 不打日志免刷屏
    s_buzzer_muted = false;
}

static auto_policy_door_light_fn s_door_light_cb   = NULL;
static void                     *s_door_light_user = NULL;

// NULL 表示注销
app_err_t auto_policy_set_door_light_hook(auto_policy_door_light_fn cb, void *user) {
    s_door_light_cb   = cb;
    s_door_light_user = user;

    LOGD(TAG, "门灯联动回调%s", (cb != NULL) ? "已注册" : "已注销");
    return APP_OK;
}

void auto_policy_notify_door_lit(void) {
    if (s_door_light_cb != NULL) {
        s_door_light_cb(s_door_light_user);
    }
}
