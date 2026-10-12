// c
// 四路房间灯接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 四个房间的灯
typedef enum {
    // 客厅那路
    BSP_LED_LIVING = 0,
    // 厨房那路
    BSP_LED_KITCHEN,
    // 卧室那路
    BSP_LED_BEDROOM,
    // 浴室那路
    BSP_LED_BATH,
    BSP_LED_MAX,
} bsp_led_zone_t;

const char *bsp_led_zone_name(bsp_led_zone_t zone);

// 把硬件准备好
esp_err_t bsp_led_init(void);

// 亮度唯一入口
esp_err_t bsp_led_set_level(bsp_led_zone_t zone, int pct);

esp_err_t bsp_led_set_power(bsp_led_zone_t zone, bool on);

// 编号不对给 0
int bsp_led_get_level(bsp_led_zone_t zone);

// 只看设定值
bool bsp_led_is_on(bsp_led_zone_t zone);

// 一路失败也设完
esp_err_t bsp_led_set_all(int pct);

esp_err_t bsp_led_all_off(void);

// 至少一路配上
bool bsp_led_ready(void);

#ifdef __cplusplus
}
#endif
