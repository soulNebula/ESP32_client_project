// c
// 麦克风采样与电平
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "i2s_mic.h"

static const char *TAG = "voice.mic";

// 16 位会裁低位
#define I2S_MIC_SLOT_IS_32BIT 1

// 15 不削顶
#define I2S_MIC_RAW_SHIFT 15

// 共约 60ms
#define I2S_MIC_DMA_DESC_NUM  4
#define I2S_MIC_DMA_FRAME_NUM 240

// 512 字一帧
#define I2S_MIC_RAW_CHUNK 512

// 直流偏置滤波
#define I2S_MIC_DC_IIR_SHIFT 8

// 单次等待上限
#define I2S_MIC_READ_TIMEOUT_MS_MAX 2000

// 削顶告警限速
#define I2S_MIC_CLIP_LOG_GAP_US 5000000LL

// 端口不用 AUTO
static i2s_chan_handle_t s_rx_chan = NULL;
// 幂等标志
static bool s_inited = false;

// 直流偏置估计
static int32_t s_dc_est = 0;

// 电平快照，不加锁
static bsp_voice_mic_level_t s_level;

// 中转缓冲，不占栈
static int32_t s_raw[I2S_MIC_RAW_CHUNK];

// 前置声明
static uint32_t mic_isqrt_u32(uint32_t x);
static int32_t  mic_log2_q8(uint32_t x);
static void mic_decimate(const int32_t *raw, size_t n_pairs, int16_t *out, size_t *out_n, int32_t *other_peak,
                         int *other_samples);
static void mic_level_update(int32_t peak, int32_t min, int32_t max, int64_t sum, int64_t sum_sq,
                             size_t samples, int32_t other_peak, int other_samples);

// 不用浮点函数

// 整数平方根
static uint32_t mic_isqrt_u32(uint32_t x) {
    if (x == 0) {
        return 0;
    }

    // 先猜上界
    uint32_t r = 1u << 16;
    while (r > 0 && r > x / r) {
        r >>= 1;
    }

    for (int i = 0; i < 6; i++) {
        if (r == 0) {
            break;
        }
        r = (r + x / r) / 2;
    }

    // 边界钉死
    while (r > 0 && r > x / r) {
        r--;
    }
    while ((r + 1u) <= x / (r + 1u)) {
        r++;
    }
    return r;
}

// 整数 log2 Q8
static int32_t mic_log2_q8(uint32_t x) {
    if (x == 0) {
        // 0 当极小值
        return INT32_MIN;
    }

    // 取最高位
    int32_t e = 0;
    while ((x >> (e + 1)) != 0u) {
        e++;
    }

    // 尾数用 Q30
    uint64_t m;
    if (e >= 30) {
        m = (uint64_t)(x >> (e - 30));
    } else {
        m = ((uint64_t)x) << (30 - e);
    }

    // 8 次平方得小数位
    uint32_t frac = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t sq = (m * m) >> 30;

        frac <<= 1;
        if (sq >= (1ull << 31)) {
            sq >>= 1;
            frac |= 1u;
        }
        m = sq;
    }

    return (e << 8) + (int32_t)frac;
}

// 抽单声道样本
static void mic_decimate(const int32_t *raw, size_t n_pairs, int16_t *out, size_t *out_n, int32_t *other_peak,
                         int *other_samples) {
    size_t  k         = 0;
    int32_t other_max = 0;
    int     other_cnt = 0;

    for (size_t p = 0; p < n_pairs; p++) {
        // 左槽样本
        const int32_t word = raw[p * 2u];

        // 右槽用来判接线
        const int32_t o  = raw[p * 2u + 1u] >> I2S_MIC_RAW_SHIFT;
        const int32_t oa = (o < 0) ? -o : o;
        if (oa > other_max) {
            other_max = oa;
        }
        other_cnt++;

        int32_t v = (int32_t)(word >> I2S_MIC_RAW_SHIFT);

        // 先更新再减
        s_dc_est += (v - s_dc_est + (1 << (I2S_MIC_DC_IIR_SHIFT - 1))) >> I2S_MIC_DC_IIR_SHIFT;
        v -= s_dc_est;

        // 不夹会出假尖峰
        if (v > BSP_VOICE_MIC_FULL_SCALE) {
            v = BSP_VOICE_MIC_FULL_SCALE;
        } else if (v < -BSP_VOICE_MIC_FULL_SCALE - 1) {
            v = -BSP_VOICE_MIC_FULL_SCALE - 1;
        }

        if (out != NULL) {
            out[k] = (int16_t)v;
        }
        k++;
    }

    if (out_n != NULL) {
        *out_n = k;
    }
    if (other_peak != NULL) {
        *other_peak = other_max;
    }
    if (other_samples != NULL) {
        *other_samples = other_cnt;
    }
}

// 更新电平快照
static void mic_level_update(int32_t peak, int32_t min, int32_t max, int64_t sum, int64_t sum_sq,
                             size_t samples, int32_t other_peak, int other_samples) {
    if (samples == 0) {
        // 保持上趟值
        s_level.read_count++;
        return;
    }

    const int64_t mean_sq = sum_sq / (int64_t)samples;
    s_level.rms           = (int)((mean_sq > 0) ? mic_isqrt_u32((uint32_t)mean_sq) : 0);

    s_level.peak         = (int)peak;
    s_level.dc           = (int)(sum / (int64_t)samples);
    s_level.peak_to_peak = (int)(max - min);
    s_level.last_samples = samples;
    s_level.read_count++;

    s_level.peak_other    = (int)other_peak;
    s_level.other_samples = other_samples;

    // 用峰值算 dBFS
    if (peak > 0) {
        const int32_t l2_fs   = mic_log2_q8(32768u);
        const int32_t l2_peak = mic_log2_q8((uint32_t)peak);
        const int32_t diff_q8 = l2_fs - l2_peak;
        s_level.db_x10        = -(int)((diff_q8 * 60) / 256);
    } else {
        s_level.db_x10 = -9990;
    }
}

// 初始化 I²S
esp_err_t bsp_voice_mic_init(void) {
    // 幂等
    if (s_inited) {
        ESP_LOGD(TAG, "already inited，忽略重复调用");
        return ESP_OK;
    }

    // 主机模式，只收
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_MIC_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num      = I2S_MIC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num     = I2S_MIC_DMA_FRAME_NUM;
    // 没接时交回全 0
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s（I²S%d 可能被别的模块占用）", esp_err_to_name(err),
                 (int)BSP_I2S_MIC_PORT);
        s_rx_chan = NULL;
        return err;
    }

#if I2S_MIC_SLOT_IS_32BIT
    const i2s_data_bit_width_t slot_bits = I2S_DATA_BIT_WIDTH_32BIT;
#else
    const i2s_data_bit_width_t slot_bits = I2S_DATA_BIT_WIDTH_16BIT;
#endif

    // 飞利浦时序
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_I2S_MIC_SAMPLE_RATE),
        // MCLK 不用
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(slot_bits, I2S_SLOT_MODE_MONO),
        // 三线取自板级表
        .gpio_cfg =
            {
                       .mclk = I2S_GPIO_UNUSED,
                       .bclk = BSP_I2S_MIC_SCK_GPIO,
                       .ws   = BSP_I2S_MIC_WS_GPIO,
                       .dout = I2S_GPIO_UNUSED,
                       .din  = BSP_I2S_MIC_SD_GPIO,
                       .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    }, },
    };

    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode 失败: %s", esp_err_to_name(err));
        // 失败要回滚
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable 失败: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    s_dc_est = 0;
    memset(&s_level, 0, sizeof(s_level));

    s_inited = true;

    ESP_LOGI(TAG, "I²S 麦克风就绪：%dHz / %d 位时隙 / 单声道 / 主机 RX", BSP_I2S_MIC_SAMPLE_RATE,
             (int)slot_bits);
    ESP_LOGI(TAG, "  SCK=GPIO%d  WS=GPIO%d  SD=GPIO%d（引脚来自 board_config.h 第 8 节）",
             (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);
    ESP_LOGI(TAG, "INMP441 是 I²S 不是 I²C：L/R 必须接 GND（左声道），VDD 接 3V3 不要 5V");
    ESP_LOGW(TAG, "麦克风没接不会让初始化失败（只会一直读到全 0）——"
                  "要确认接线请敲串口命令 `mic` 看实时电平");
    return ESP_OK;
}

// 阻塞读单声道
esp_err_t bsp_voice_mic_read(int16_t *buf, size_t samples, size_t *out_read, uint32_t timeout_ms) {
    if (out_read != NULL) {
        *out_read = 0;
    }
    if (buf == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || s_rx_chan == NULL) {
        ESP_LOGW(TAG, "还没调用 bsp_voice_mic_init()");
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms > (uint32_t)I2S_MIC_READ_TIMEOUT_MS_MAX) {
        timeout_ms = (uint32_t)I2S_MIC_READ_TIMEOUT_MS_MAX;
    }

    const int64_t t_start   = esp_timer_get_time();
    const int64_t budget_us = (int64_t)timeout_ms * 1000LL;

    // 本趟统计
    int32_t min    = INT32_MAX;
    int32_t max    = INT32_MIN;
    int32_t peak   = 0;
    int64_t sum    = 0;
    int64_t sum_sq = 0;
    int32_t clip   = 0;

    int32_t other_peak    = 0;
    int     other_samples = 0;

    size_t    got = 0;
    esp_err_t err = ESP_OK;

    while (got < samples) {
        size_t want = samples - got;
        if (want > (size_t)(I2S_MIC_RAW_CHUNK / 2)) {
            want = (size_t)(I2S_MIC_RAW_CHUNK / 2);
        }
        const size_t want_raw = want * 2u;

        // 折算等待 tick
        TickType_t wait_ticks = 0;
        if (budget_us > 0) {
            const int64_t left_us = budget_us - (esp_timer_get_time() - t_start);
            if (left_us <= 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            wait_ticks = pdMS_TO_TICKS((uint32_t)((left_us + 999) / 1000));
            if (wait_ticks == 0) {
                // 至少等一拍
                wait_ticks = 1;
            }
        }

        size_t bytes_read = 0;
        err = i2s_channel_read(s_rx_chan, s_raw, want_raw * sizeof(int32_t), &bytes_read, wait_ticks);
        if (err != ESP_OK) {
            // 超时是常态
            break;
        }

        const size_t n_words = bytes_read / sizeof(int32_t);
        if (n_words < 2) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        const size_t n_pairs = n_words / 2u;
        size_t       n_mono  = 0;

        int32_t chunk_other_peak = 0;
        int     chunk_other_n    = 0;

        mic_decimate(s_raw, n_pairs, buf + got, &n_mono, &chunk_other_peak, &chunk_other_n);
        if (chunk_other_peak > other_peak) {
            other_peak = chunk_other_peak;
        }
        other_samples += chunk_other_n;

        if (n_mono == 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        for (size_t i = 0; i < n_mono; i++) {
            const int32_t v = (int32_t)buf[got + i];

            if (v < min) {
                min = v;
            }
            if (v > max) {
                max = v;
            }
            sum += v;
            sum_sq += (int64_t)v * (int64_t)v;

            const int32_t av = (v < 0) ? -v : v;
            if (av > peak) {
                peak = av;
            }
            // 到满量程算削顶
            if (av >= BSP_VOICE_MIC_FULL_SCALE) {
                clip++;
            }
        }

        got += n_mono;
    }

    // 采样电平同趟做完
    mic_level_update(peak, min, max, sum, sum_sq, got, other_peak, other_samples);

    if (got > 0) {
        // clip 只算本趟
        s_level.clip = (uint32_t)clip;
    }

    if (clip > 0 && got > 0) {
        static int64_t s_last_clip_log_us = 0;
        const int64_t  now                = esp_timer_get_time();
        if (now - s_last_clip_log_us > I2S_MIC_CLIP_LOG_GAP_US) {
            s_last_clip_log_us = now;
            ESP_LOGW(TAG,
                     "本批有 %d 个样本削顶（peak=%d）：声音太大或供电/电平不对；"
                     "持续出现可把 I2S_MIC_RAW_SHIFT 从 %d 调大到 %d",
                     (int)clip, (int)peak, I2S_MIC_RAW_SHIFT, I2S_MIC_RAW_SHIFT + 2);
        }
    }

    if (out_read != NULL) {
        *out_read = got;
    }

    return (got > 0) ? ESP_OK : ((err == ESP_OK) ? ESP_ERR_TIMEOUT : err);
}

// 取电平快照
void bsp_voice_mic_get_level(bsp_voice_mic_level_t *out) {
    if (out == NULL) {
        return;
    }
    *out = s_level;
}

// 通道装好了吗
bool bsp_voice_mic_ready(void) {
    return (s_inited && s_rx_chan != NULL);
}
