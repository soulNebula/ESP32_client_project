// c
// 按键接口
#pragma once
#include <stdbool.h>
#include "app_common.h"

typedef enum {
    // 板载 BOOT 键
    BSP_KEY_1 = 0,
    // GPIO15 键
    BSP_KEY_2,
    BSP_KEY_MAX,
} bsp_key_id_t;

typedef enum {
    BSP_KEY_EVENT_SHORT = 0,
    // 每 500ms 重报
    BSP_KEY_EVENT_LONG,
    BSP_KEY_EVENT_RELEASE,
} bsp_key_event_t;

typedef void (*bsp_key_cb_t)(bsp_key_id_t key, bsp_key_event_t event, void *user);

// 把按键准备好
app_err_t bsp_key_init(void);

// NULL 注销
// 回调里别阻塞
app_err_t bsp_key_register_cb(bsp_key_id_t key, bsp_key_cb_t cb, void *user);

// 主循环里轮询
void bsp_key_tick(void);

bool bsp_key_is_pressed(bsp_key_id_t key);

bool bsp_key_ready(void);
