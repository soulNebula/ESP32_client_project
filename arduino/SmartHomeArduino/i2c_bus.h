// c
// I2C 总线封装
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_common.h"

// 扫描结果上限
#define BSP_I2C_SCAN_MAX 16

// 只跑 100k
app_err_t bsp_i2c_init(void);

bool bsp_i2c_ready(void);

// 返回设备数
int bsp_i2c_scan(uint8_t *found, int max);

// 在线为真
bool bsp_i2c_probe(uint8_t addr);

app_err_t bsp_i2c_write(uint8_t addr, const uint8_t *data, int len);

app_err_t bsp_i2c_read(uint8_t addr, uint8_t *data, int len);

// 先写寄存器再读
app_err_t bsp_i2c_write_read(uint8_t addr, const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen);
