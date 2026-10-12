// c
// ADC1 采样接口
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#ifdef __cplusplus
extern "C" {
#endif

// 最多 6 路
#define BSP_ADC_MAX_CHANNELS 6

typedef int bsp_adc_ch_t;
#define BSP_ADC_CH_INVALID (-1)

// 模拟量走 ADC1
esp_err_t bsp_adc_init(void);

// 只收 ADC1 脚
bsp_adc_ch_t bsp_adc_channel_add(int gpio, adc_atten_t atten, adc_bitwidth_t bitwidth);

// 返回 0~4095
esp_err_t bsp_adc_read_raw(bsp_adc_ch_t ch, int *out);

// 没校准按线性算
esp_err_t bsp_adc_read_mv(bsp_adc_ch_t ch, int *mv);

// 最多 16 次
esp_err_t bsp_adc_read_avg(bsp_adc_ch_t ch, int samples, int *out_avg);

float bsp_adc_raw_to_pct(int raw, int max_raw);

// ADC 起好了没
bool bsp_adc_ready(void);

#ifdef __cplusplus
}
#endif
