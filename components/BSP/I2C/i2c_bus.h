// c
// I2C0 总线接口
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// 最多 16 个
#define BSP_I2C_SCAN_MAX 16

// 起 I2C0 总线
esp_err_t bsp_i2c_init(void);

void bsp_i2c_deinit(void);

// 同址只许挂一次
esp_err_t bsp_i2c_add_device(uint8_t addr, uint32_t freq_hz, i2c_master_dev_handle_t *out_dev);

esp_err_t bsp_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len,
                           uint32_t timeout_ms);

esp_err_t bsp_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *data, size_t len,
                            uint32_t timeout_ms);

// 不发 reg 地址
esp_err_t bsp_i2c_read(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len, uint32_t timeout_ms);

// 发 16 位命令字
esp_err_t bsp_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, uint32_t timeout_ms);

// 扫 7 位地址
int bsp_i2c_scan(uint8_t *found, size_t max_found);

// 查在线，用缓存
bool bsp_i2c_present(uint8_t addr);

// 实时探，不走缓存
bool bsp_i2c_probe(uint8_t addr);

bool bsp_i2c_ready(void);

i2c_master_bus_handle_t bsp_i2c_bus(void);

#ifdef __cplusplus
}
#endif
