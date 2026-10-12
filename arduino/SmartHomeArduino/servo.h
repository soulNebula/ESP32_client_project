// c
// 门舵机接口
#pragma once
#include <stdbool.h>
#include "app_common.h"

// 上电先关门
app_err_t bsp_servo_init(void);

app_err_t bsp_servo_door_set(bool open);

app_err_t bsp_servo_door_set_pct(int pct);

// 没配上也算成功
app_err_t bsp_servo_door_set_deg(float deg);

// 只是软件状态
bool bsp_servo_door_is_open(void);

int bsp_servo_door_get_pct(void);

// 松劲给占空比 0
app_err_t bsp_servo_door_release(void);

bool bsp_servo_ready(void);
