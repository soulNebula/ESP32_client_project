// c
// 自动联动接口
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// 失败也返 OK
// 控制器先初始化
esp_err_t home_auto_init(void);

// 500ms 调一次
// 传感器离线别动
// 下沿 31.5 防抖
void home_auto_tick(const sensor_snapshot_t *snap);

// 不给值默认 30s
// 只改时间戳不阻塞
void home_auto_protect_living_light(int ms);

#ifdef __cplusplus
}
#endif
