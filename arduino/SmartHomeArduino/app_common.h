// c
// 错误码与日志，替代 esp_err
#pragma once
#include <Arduino.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// 名字跟 IDF 对齐好对照
typedef int32_t app_err_t;

#define APP_OK 0
// 参数不合法
#define APP_ERR_INVALID_ARG -1
// 不支持的命令
#define APP_ERR_NOT_FOUND -2
// 当前状态不允许
#define APP_ERR_INVALID_STATE -3
// 硬件没写成功
#define APP_ERR_FAIL -4
// 内存不够
#define APP_ERR_NO_MEM -5
// 没实现
#define APP_ERR_NOT_SUPPORTED -6

// tag 用字面量，别传指针
#define LOGI(tag, fmt, ...) Serial.printf("[I][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) Serial.printf("[W][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) Serial.printf("[E][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
// 太吵，默认关
#define LOGD(tag, fmt, ...)                                                                        \
    do {                                                                                           \
        if (app_log_verbose()) {                                                                   \
            Serial.printf("[D][%s] " fmt "\r\n", tag, ##__VA_ARGS__);                               \
        }                                                                                          \
    } while (0)

// 每秒时间戳
uint32_t app_millis(void);

// 开机以来的毫秒
int64_t app_uptime_ms(void);

// 开就打印 LOGD
bool app_log_verbose(void);

void app_log_set_verbose(bool on);

// 错误码转中文
const char *app_err_text(app_err_t err);
