// c
// 门舵机接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 上电先转到关门位
esp_err_t bsp_servo_init(void);

esp_err_t bsp_servo_door_set(bool open);

// 0关 100开
esp_err_t bsp_servo_door_set_pct(int pct);

// 只看软件状态
bool bsp_servo_door_is_open(void);

int bsp_servo_door_get_pct(void);

// 只调试用 会拧坏门
esp_err_t bsp_servo_door_set_deg(float deg);

// 松劲后门位置不定
esp_err_t bsp_servo_door_release(void);

bool bsp_servo_ready(void);

#ifdef __cplusplus
}
#endif
