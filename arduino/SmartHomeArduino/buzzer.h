// c
// 蜂鸣器接口
#pragma once
#include <stdbool.h>
#include "app_common.h"

// 初始化并静音
app_err_t bsp_buzzer_init(void);

// 两级幂等
app_err_t bsp_buzzer_set(bool on);

// 查是否在响
bool bsp_buzzer_is_on(void);

// 短鸣不阻塞
app_err_t bsp_buzzer_beep(int ms);

// 轮询短鸣到期
void bsp_buzzer_tick(void);

// 初始化成功了吗
bool bsp_buzzer_ready(void);

// 切极性并静音
app_err_t bsp_buzzer_set_polarity_low(bool active_low);

// 读当前极性
bool bsp_buzzer_is_polarity_low(void);

// 极性自检 4 段
app_err_t bsp_buzzer_polarity_test(void);
