// c
// 麦克风接口
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 满量程
#define BSP_VOICE_MIC_FULL_SCALE 32767

// 电平快照
typedef struct {
    int rms;
    int peak;
    int peak_to_peak;
    int dc;
    // 分贝×10
    int db_x10;
    size_t last_samples;
    uint32_t read_count;
    // 削顶点数
    uint32_t clip;

    // 右槽大即接错
    int peak_other;
    // 右槽点数
    int other_samples;
} bsp_voice_mic_level_t;

// 没接也算成功
esp_err_t bsp_voice_mic_init(void);

// 阻塞读音频
esp_err_t bsp_voice_mic_read(int16_t *buf, size_t samples, size_t *out_read, uint32_t timeout_ms);

void bsp_voice_mic_get_level(bsp_voice_mic_level_t *out);

// 通道好≠麦接好
bool bsp_voice_mic_ready(void);

#ifdef __cplusplus
}
#endif
