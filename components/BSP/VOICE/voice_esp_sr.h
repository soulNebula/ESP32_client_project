// c
// 识别后端接口
#pragma once
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把识别跑起来
esp_err_t bsp_voice_sr_start(void);

bool bsp_voice_sr_ready(void);

// 在等命令吗
bool bsp_voice_sr_awake(void);

const char *bsp_voice_sr_wake_word(void);

// 报模型名
const char *bsp_voice_sr_mn_model(void);

// 打命令词表
void bsp_voice_sr_print_commands(void);

#ifdef __cplusplus
}
#endif
