// c
// 通用 GPIO 与状态灯接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// -1 则不改电平
esp_err_t bsp_gpio_output_init(int gpio, int init_level, bool active_low);

esp_err_t bsp_gpio_input_init(int gpio, bool pull_up);

void bsp_gpio_write(int gpio, bool on);

// 出错返回 -1
int bsp_gpio_read(int gpio);

// 没接也不报错
esp_err_t bsp_status_led_init(void);

// 分量限到 40
void bsp_status_led_set(uint8_t r, uint8_t g, uint8_t b);

void bsp_status_led_off(void);

// code 见宏定义
void bsp_status_led_show(int code);

// 蓝：开机中
#define BSP_STATUS_BOOTING 0
// 黄：WiFi 未连
#define BSP_STATUS_NO_WIFI 1
// 绿：在线
#define BSP_STATUS_ONLINE 2
// 红：高温报警
#define BSP_STATUS_ALERT 3
// 紫：配网模式
#define BSP_STATUS_AP_MODE 4

#ifdef __cplusplus
}
#endif
