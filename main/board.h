// c
// 板级自检接口
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 表长度上限
#define BOARD_MODULE_MAX 12

void board_dump_pinmap(void);

// 顺序有依赖别调换
esp_err_t board_init(void);

void board_dump_summary(void);

bool board_module_ok(const char *name);

#define BOARD_MOD_STATUS_LED "status_led"
#define BOARD_MOD_I2C        "i2c"
#define BOARD_MOD_ADC        "adc"
#define BOARD_MOD_LED        "led"
#define BOARD_MOD_SERVO      "servo"
#define BOARD_MOD_BUZZER     "buzzer"
#define BOARD_MOD_SENSOR     "sensor"
#define BOARD_MOD_PANEL      "panel"
#define BOARD_MOD_KEY        "key"
#define BOARD_MOD_VOICE      "voice"
#define BOARD_MOD_BLE        "ble"

#ifdef __cplusplus
}
#endif
