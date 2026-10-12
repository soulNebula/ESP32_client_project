// c
// GPIO 读写与状态灯
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_common.h"

// active_low 只影响逻辑电平
app_err_t bsp_gpio_output_init(int gpio, int init_level, bool active_low);

app_err_t bsp_gpio_input_init(int gpio, bool pull_up);

void bsp_gpio_write(int gpio, bool on);

// 读原始电平
int bsp_gpio_read(int gpio);

// 状态灯状态码
#define BSP_STATUS_BOOTING 0
#define BSP_STATUS_NO_WIFI 1
#define BSP_STATUS_ONLINE  2
#define BSP_STATUS_ALERT   3
#define BSP_STATUS_AP_MODE 4

// 失败也返回成功
app_err_t bsp_status_led_init(void);

void bsp_status_led_set(uint8_t r, uint8_t g, uint8_t b);

void bsp_status_led_off(void);

// 按状态码设色
void bsp_status_led_show(int code);
