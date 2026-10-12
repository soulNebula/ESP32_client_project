// c
// 按键扫描与消抖
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "gpio_util.h"
#include "key.h"

static const char *TAG = "KEY";

#define KEY_SCAN_PERIOD_MS 10
#define KEY_TASK_STACK_SIZE 3072
#define KEY_TASK_PRIORITY 4

// 消抖判定次数
#define KEY_DEBOUNCE_SAMPLES (BSP_KEY_DEBOUNCE_MS / KEY_SCAN_PERIOD_MS)

// 长按重复间隔
#define KEY_LONG_REPEAT_MS 500

_Static_assert(BSP_KEY_MAX == BSP_KEY_COUNT,
               "key.h 的 BSP_KEY_MAX 必须等于 board_config.h 的 BSP_KEY_COUNT");
_Static_assert(KEY_DEBOUNCE_SAMPLES >= 1, "BSP_KEY_DEBOUNCE_MS 必须 >= 按键扫描周期");

// 顺序同枚举
static const int s_key_gpio[BSP_KEY_MAX] = {
    [BSP_KEY_1] = (int)BSP_KEY_GPIO_KEY1,
    [BSP_KEY_2] = (int)BSP_KEY_GPIO_KEY2,
};

// 一路按键状态
typedef struct {
    // 消抖后电平
    int stable_level;
    // 原始读数
    int raw_level;
    // 连续反电平数
    uint32_t same_cnt;
    // 是否按下
    bool pressed;
    // 长按已报
    bool long_reported;
    // 按下起点
    int64_t press_start_us;
    // 上次长按时刻
    int64_t last_long_us;
} key_ctx_t;

static key_ctx_t s_ctx[BSP_KEY_MAX];

// 回调槽
static bsp_key_cb_t s_cb[BSP_KEY_MAX];
static void        *s_cb_user[BSP_KEY_MAX];

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
    const int64_t now_us = esp_timer_get_time();

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
            k->same_cnt = 0;

            if (k->pressed) {
                const int64_t held_us = now_us - k->press_start_us;

                if (!k->long_reported) {
                    // 到阈值报一次
                    if (held_us >= (int64_t)BSP_KEY_LONG_PRESS_MS * 1000) {
                        k->long_reported = true;
                        k->last_long_us  = now_us;
                        key_emit(id, BSP_KEY_EVENT_LONG);
                    }
                } else if ((now_us - k->last_long_us) >= (int64_t)KEY_LONG_REPEAT_MS * 1000) {
                    // 500ms 再报长按
                    k->last_long_us = now_us;
                    key_emit(id, BSP_KEY_EVENT_LONG);
                }
            }
            continue;
        }

        // 电平变了
        k->same_cnt++;
        if (k->same_cnt < KEY_DEBOUNCE_SAMPLES) {
            continue;
        }

        k->same_cnt     = 0;
        k->stable_level = raw;

        if (key_level_is_pressed(raw)) {
            // 按下只更新状态
            k->pressed        = true;
            k->long_reported  = false;
            k->press_start_us = now_us;
            k->last_long_us   = 0;
            continue;
        }

        const int64_t held_us = now_us - k->press_start_us;

        k->pressed = false;

        // 松手补报长按
        if (!k->long_reported && held_us >= (int64_t)BSP_KEY_LONG_PRESS_MS * 1000) {
            k->long_reported = true;
            key_emit(id, BSP_KEY_EVENT_LONG);
        }

        // 松开先报
        key_emit(id, BSP_KEY_EVENT_RELEASE);

        // 才算短按
        if (!k->long_reported && held_us < (int64_t)BSP_KEY_LONG_PRESS_MS * 1000) {
            key_emit(id, BSP_KEY_EVENT_SHORT);
        }

        k->long_reported = false;

        ESP_LOGD(TAG, "KEY%d release: held=%dms", i + 1, (int)(held_us / 1000));
    }
}

// 节拍必须准
static void key_scan_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "scan task start: period=%dms, debounce=%dms(%d samples), long=%dms, repeat=%dms",
             KEY_SCAN_PERIOD_MS, BSP_KEY_DEBOUNCE_MS, (int)KEY_DEBOUNCE_SAMPLES, BSP_KEY_LONG_PRESS_MS,
             KEY_LONG_REPEAT_MS);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        key_scan_once();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
    }
}

// 初始化按键
esp_err_t bsp_key_init(void) {
    if (s_inited) {
        return ESP_OK;
    }

    for (int i = 0; i < BSP_KEY_MAX; i++) {
        const esp_err_t err = bsp_gpio_input_init(s_key_gpio[i], true);
        if (err != ESP_OK) {
            // 单键失败可跳过
            ESP_LOGW(TAG, "KEY%d (GPIO%d) 配置失败: %s —— 该键不可用，另一个键照常工作", i + 1, s_key_gpio[i],
                     esp_err_to_name(err));
        }
    }

    // 上电取实际电平
    const int64_t now_us = esp_timer_get_time();

    for (int i = 0; i < BSP_KEY_MAX; i++) {
        int level = bsp_gpio_read(s_key_gpio[i]);
        if (level < 0) {
            // 读失败当没按
            level = (BSP_KEY_ACTIVE_LEVEL != 0) ? 0 : 1;
        }

        s_ctx[i].stable_level   = level;
        s_ctx[i].raw_level      = level;
        s_ctx[i].same_cnt       = 0;
        s_ctx[i].pressed        = key_level_is_pressed(level);
        s_ctx[i].long_reported  = false;
        s_ctx[i].press_start_us = now_us;
        s_ctx[i].last_long_us   = 0;
    }

    if (xTaskCreate(key_scan_task, "key_scan", KEY_TASK_STACK_SIZE, NULL, KEY_TASK_PRIORITY, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "创建按键扫描任务失败（栈 %d 字节）", KEY_TASK_STACK_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok: KEY1=GPIO%d, KEY2=GPIO%d, active_level=%d", (int)BSP_KEY_GPIO_KEY1,
             (int)BSP_KEY_GPIO_KEY2, BSP_KEY_ACTIVE_LEVEL);
    return ESP_OK;
}

// 注册回调
esp_err_t bsp_key_register_cb(bsp_key_id_t key, bsp_key_cb_t cb, void *user) {
    if (!key_valid(key)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb[key]      = cb;
    s_cb_user[key] = user;
    return ESP_OK;
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
