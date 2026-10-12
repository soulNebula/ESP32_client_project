// c
// 按键扫描与消抖
#include <Arduino.h>

#include "app_common.h"
#include "bsp_config.h"
#include "gpio_util.h"
#include "key.h"

static const char *TAG = "KEY";

// 主循环至少这么快调一次
#define KEY_SCAN_PERIOD_MS 10

// 长按重复间隔
#define KEY_LONG_REPEAT_MS 500

static_assert(BSP_KEY_MAX == BSP_KEY_COUNT,
              "key.h 的 BSP_KEY_MAX 必须等于 bsp_config.h 的 BSP_KEY_COUNT");

// 顺序同枚举
static const int s_key_gpio[BSP_KEY_MAX] = {
    BSP_KEY_GPIO_KEY1,
    BSP_KEY_GPIO_KEY2,
};

// 一路按键状态
typedef struct {
    // 消抖后电平
    int stable_level;
    // 原始读数
    int raw_level;
    // 本次变化起点，-1 表示没在变
    int64_t change_ms;
    // 是否按下
    bool pressed;
    // 长按已报
    bool long_reported;
    // 按下起点
    int64_t press_start_ms;
    // 上次长按时刻
    int64_t last_long_ms;
} key_ctx_t;

static key_ctx_t s_ctx[BSP_KEY_MAX];

// 回调槽
static bsp_key_cb_t s_cb[BSP_KEY_MAX];
static void        *s_cb_user[BSP_KEY_MAX];

// 上次扫描时刻
static int64_t s_last_scan_ms;

// 幂等标志
static bool s_inited;

// 键号合法吗
static bool key_valid(bsp_key_id_t key) {
    return ((int)key >= 0) && ((int)key < BSP_KEY_MAX);
}

// 低电平即按下
static inline bool key_level_is_pressed(int level) {
    return (level == BSP_KEY_ACTIVE_LEVEL);
}

// 投递事件
static void key_emit(bsp_key_id_t key, bsp_key_event_t ev) {
    if (s_cb[key] != NULL) {
        s_cb[key](key, ev, s_cb_user[key]);
    }
}

// 扫一遍按键
static void key_scan_once(void) {
    const int64_t now_ms = app_uptime_ms();

    for (int i = 0; i < BSP_KEY_MAX; i++) {
        key_ctx_t         *k  = &s_ctx[i];
        const bsp_key_id_t id = (bsp_key_id_t)i;

        // -1 表示读失败
        const int raw = bsp_gpio_read(s_key_gpio[i]);
        if (raw < 0) {
            continue;
        }
        k->raw_level = raw;

        // 电平没变
        if (raw == k->stable_level) {
            k->change_ms = -1;

            if (k->pressed) {
                const int64_t held_ms = now_ms - k->press_start_ms;

                if (!k->long_reported) {
                    // 到阈值报一次
                    if (held_ms >= (int64_t)BSP_KEY_LONG_PRESS_MS) {
                        k->long_reported = true;
                        k->last_long_ms  = now_ms;
                        key_emit(id, BSP_KEY_EVENT_LONG);
                    }
                } else if ((now_ms - k->last_long_ms) >= (int64_t)KEY_LONG_REPEAT_MS) {
                    // 500ms 再报长按
                    k->last_long_ms = now_ms;
                    key_emit(id, BSP_KEY_EVENT_LONG);
                }
            }
            continue;
        }

        // 电平变了，开始按时间消抖
        if (k->change_ms < 0) {
            k->change_ms = now_ms;
        }
        if ((now_ms - k->change_ms) < (int64_t)BSP_KEY_DEBOUNCE_MS) {
            continue;
        }

        k->change_ms    = -1;
        k->stable_level = raw;

        if (key_level_is_pressed(raw)) {
            // 按下只更新状态
            k->pressed        = true;
            k->long_reported  = false;
            k->press_start_ms = now_ms;
            k->last_long_ms   = 0;
            continue;
        }

        const int64_t held_ms = now_ms - k->press_start_ms;

        k->pressed = false;

        // 松手补报长按
        if (!k->long_reported && held_ms >= (int64_t)BSP_KEY_LONG_PRESS_MS) {
            k->long_reported = true;
            key_emit(id, BSP_KEY_EVENT_LONG);
        }

        // 松开先报
        key_emit(id, BSP_KEY_EVENT_RELEASE);

        // 才算短按
        if (!k->long_reported && held_ms < (int64_t)BSP_KEY_LONG_PRESS_MS) {
            key_emit(id, BSP_KEY_EVENT_SHORT);
        }

        k->long_reported = false;

        LOGD(TAG, "KEY%d release: held=%dms", i + 1, (int)held_ms);
    }
}

// 把按键准备好
app_err_t bsp_key_init(void) {
    if (s_inited) {
        return APP_OK;
    }

    for (int i = 0; i < BSP_KEY_MAX; i++) {
        const app_err_t err = bsp_gpio_input_init(s_key_gpio[i], true);
        if (err != APP_OK) {
            // 单键失败可跳过
            LOGW(TAG, "KEY%d (GPIO%d) 配置失败: %s —— 该键不可用，另一个键照常工作", i + 1,
                 s_key_gpio[i], app_err_text(err));
        }
    }

    // 上电取实际电平
    const int64_t now_ms = app_uptime_ms();

    for (int i = 0; i < BSP_KEY_MAX; i++) {
        int level = bsp_gpio_read(s_key_gpio[i]);
        if (level < 0) {
            // 读失败当没按
            level = (BSP_KEY_ACTIVE_LEVEL != 0) ? 0 : 1;
        }

        s_ctx[i].stable_level   = level;
        s_ctx[i].raw_level      = level;
        s_ctx[i].change_ms      = -1;
        s_ctx[i].pressed        = key_level_is_pressed(level);
        s_ctx[i].long_reported  = false;
        s_ctx[i].press_start_ms = now_ms;
        s_ctx[i].last_long_ms   = 0;
    }

    s_last_scan_ms = now_ms;
    s_inited       = true;

    LOGI(TAG, "init ok: KEY1=GPIO%d, KEY2=GPIO%d, active_level=%d", BSP_KEY_GPIO_KEY1,
         BSP_KEY_GPIO_KEY2, BSP_KEY_ACTIVE_LEVEL);
    LOGI(TAG, "主循环请每 %dms 调一次 bsp_key_tick()（消抖 %dms，长按 %dms，长按每 %dms 重报一次）",
         KEY_SCAN_PERIOD_MS, BSP_KEY_DEBOUNCE_MS, BSP_KEY_LONG_PRESS_MS, KEY_LONG_REPEAT_MS);
    return APP_OK;
}

// 注册回调
app_err_t bsp_key_register_cb(bsp_key_id_t key, bsp_key_cb_t cb, void *user) {
    if (!key_valid(key)) {
        return APP_ERR_INVALID_ARG;
    }

    s_cb[key]      = cb;
    s_cb_user[key] = user;
    return APP_OK;
}

// 主循环里轮询
void bsp_key_tick(void) {
    if (!s_inited) {
        return;
    }

    // 扫太密没意义
    const int64_t now_ms = app_uptime_ms();
    if ((now_ms - s_last_scan_ms) < (int64_t)KEY_SCAN_PERIOD_MS) {
        return;
    }
    s_last_scan_ms = now_ms;

    key_scan_once();
}

// 现在按住了吗
bool bsp_key_is_pressed(bsp_key_id_t key) {
    // 按下判定晚 30ms
    return key_valid(key) && s_ctx[key].pressed;
}

// 初始化好了没
bool bsp_key_ready(void) {
    return s_inited;
}
