// c
// 环境快照结构
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 温湿度型号
typedef enum {
    SENSOR_TH_NONE = 0,
    // I2C 0x44
    SENSOR_TH_SHT30,
    // I2C 0x38
    SENSOR_TH_AHT20,
} sensor_th_kind_t;

// 光照方案
typedef enum {
    SENSOR_LIGHT_NONE = 0,
    // GPIO1 ADC
    SENSOR_LIGHT_LDR,
    // I2C 0x23
    SENSOR_LIGHT_BH1750,
} sensor_light_kind_t;

// 悬空读数不可信
typedef struct {
    float temperature;
    float humidity;
    bool th_valid;
    sensor_th_kind_t th_kind;

    float light_pct;
    // 光敏是估算
    float light_lux;
    bool                light_valid;
    sensor_light_kind_t light_kind;

    float rain_pct;
    bool raining;
    // 悬空必 false
    bool rain_valid;

    uint32_t sample_count;
    int64_t updated_ms;
} sensor_snapshot_t;

esp_err_t bsp_sensors_init(void);

void bsp_sensors_sample_now(void);

// 线程安全
void bsp_sensors_get(sensor_snapshot_t *out);

// 周期 1000ms
esp_err_t bsp_sensors_start_auto(void);

void bsp_sensors_stop_auto(void);

// 带 3% 迟滞
void bsp_sensors_set_rain_threshold(float pct);

float bsp_sensors_get_rain_threshold(void);

bool bsp_sensors_light_is_lux(void);

bool bsp_sensors_th_ready(void);

#ifdef __cplusplus
}
#endif
