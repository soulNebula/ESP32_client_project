// c
// 蜂鸣器接口
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 上电静音
esp_err_t bsp_buzzer_init(void);

// 幂等防杂音
esp_err_t bsp_buzzer_set(bool on);

bool bsp_buzzer_is_on(void);

// 建议 50~500
// 报警时别调
esp_err_t bsp_buzzer_beep(int ms);

void bsp_buzzer_tick(void);

bool bsp_buzzer_ready(void);

// 低电平触发用开漏
// 运行期可切，不落盘
esp_err_t bsp_buzzer_set_polarity_low(bool active_low);

bool bsp_buzzer_is_polarity_low(void);

// 自检约 6 秒
esp_err_t bsp_buzzer_polarity_test(void);

#ifdef __cplusplus
}
#endif
