// c
// ADC1 采样与校准

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"

#include "board_config.h"
#include "adc_bus.h"

static const char *TAG = "ADC";

#define ADC_RAW_FULL_SCALE_12BIT 4095

// 兜底 3100mV
#define ADC_LINEAR_FULL_SCALE_MV 3100

#define ADC_AVG_SAMPLES_MIN 1
#define ADC_AVG_SAMPLES_MAX 16

// 通道登记项
typedef struct {
    bool used;
    adc_channel_t chan;
    adc_bitwidth_t bitwidth;
    int gpio;
} adc_slot_t;

static adc_oneshot_unit_handle_t s_unit = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_valid = false;
static bool s_inited = false;

// 登记表，下标即句柄
static adc_slot_t s_slots[BSP_ADC_MAX_CHANNELS];

// 非法句柄给 NULL
static adc_slot_t *adc_slot_of(bsp_adc_ch_t ch) {
    if (ch < 0 || ch >= BSP_ADC_MAX_CHANNELS) {
        return NULL;
    }
    if (!s_slots[ch].used) {
        return NULL;
    }
    return &s_slots[ch];
}

// 按位宽算满量程
static int adc_full_scale_of(adc_bitwidth_t bitwidth) {
    if ((int)bitwidth <= 0) {
        return ADC_RAW_FULL_SCALE_12BIT;
    }
    return (1 << (int)bitwidth) - 1;
}

static void adc_setup_calibration(void) {
    s_cali       = NULL;
    s_cali_valid = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    // 一份句柄覆盖全通道
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BSP_ADC_UNIT,
        .chan     = BSP_ADC_CH_LIGHT,
        .atten    = BSP_ADC_ATTEN,
        .bitwidth = BSP_ADC_BITWIDTH,
    };

    const esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err == ESP_OK && s_cali != NULL) {
        s_cali_valid = true;
        ESP_LOGI(TAG, "calibration scheme: curve fitting (mV 为校准后真值)");
        return;
    }

    s_cali       = NULL;
    s_cali_valid = false;
    ESP_LOGW(TAG,
             "曲线拟合校准不可用(%s)，读数改用线性近似 raw*%d/%d —— "
             "配件没到齐时这是正常的，不影响功能",
             esp_err_to_name(err), ADC_LINEAR_FULL_SCALE_MV, ADC_RAW_FULL_SCALE_12BIT);
#else
    ESP_LOGW(TAG, "本芯片不支持曲线拟合校准，读数改用线性近似 raw*%d/%d", ADC_LINEAR_FULL_SCALE_MV,
             ADC_RAW_FULL_SCALE_12BIT);
// 校准开关收尾
#endif
}

// 建单元和校准
esp_err_t bsp_adc_init(void) {
    if (s_inited) {
        return ESP_OK;
    }

    if (s_unit == NULL) {
        // ulp 必须显式关
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id  = BSP_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };

        const esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
        if (err != ESP_OK) {
            // 别留野句柄
            s_unit = NULL;

            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "ADC%d 已被其它模块占用(%s)，请统一走 bsp_adc_init()", (int)BSP_ADC_UNIT + 1,
                         esp_err_to_name(err));
            } else {
                ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
            }
            return err;
        }

        ESP_LOGI(TAG, "ADC%d unit created (atten=%d, bitwidth=%d)", (int)BSP_ADC_UNIT + 1, (int)BSP_ADC_ATTEN,
                 (int)BSP_ADC_BITWIDTH);
        ESP_LOGI(TAG, "ADC%d 量程约 0~%dmV (atten=12dB), 通道按 GPIO 动态登记", (int)BSP_ADC_UNIT + 1,
                 ADC_LINEAR_FULL_SCALE_MV);
    }

    adc_setup_calibration();

    s_inited = true;
    return ESP_OK;
}

bsp_adc_ch_t bsp_adc_channel_add(int gpio, adc_atten_t atten, adc_bitwidth_t bitwidth) {
    if (!s_inited || s_unit == NULL) {
        ESP_LOGE(TAG, "GPIO%d 登记通道前必须先 bsp_adc_init()", gpio);
        return BSP_ADC_CH_INVALID;
    }

    // 换算交给 IDF
    adc_unit_t    unit = BSP_ADC_UNIT;
    adc_channel_t chan = ADC_CHANNEL_0;

    const esp_err_t conv_err = adc_oneshot_io_to_channel(gpio, &unit, &chan);
    if (conv_err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 不是合法的 ADC 引脚 (%s)，拒绝登记", gpio, esp_err_to_name(conv_err));
        return BSP_ADC_CH_INVALID;
    }

    if (unit != ADC_UNIT_1) {
#if SOC_ADC_PERIPH_NUM >= 2
        ESP_LOGE(TAG,
                 "GPIO%d 属于 ADC%d 通道，本工程只放行 ADC1"
                 "（WiFi 开启后 ADC2 不可用）→ 请换到 ADC1 的引脚",
                 gpio, (int)unit + 1);
#else
        ESP_LOGE(TAG, "GPIO%d 落在非 ADC1 单元上，本工程只支持 ADC1", gpio);
#endif
        return BSP_ADC_CH_INVALID;
    }

    // 重复登记复用旧句柄
    for (int i = 0; i < BSP_ADC_MAX_CHANNELS; i++) {
        if (s_slots[i].used && s_slots[i].gpio == gpio) {
            ESP_LOGW(TAG,
                     "GPIO%d 已经登记过(句柄 %d)，复用并覆盖其配置 "
                     "(atten=%d, bitwidth=%d)",
                     gpio, i, (int)atten, (int)bitwidth);
            const adc_oneshot_chan_cfg_t recfg = {
                .atten    = atten,
                .bitwidth = bitwidth,
            };
            const esp_err_t rerr = adc_oneshot_config_channel(s_unit, s_slots[i].chan, &recfg);
            if (rerr != ESP_OK) {
                ESP_LOGE(TAG, "adc_oneshot_config_channel(ADC%d_CH%d) failed: %s", (int)BSP_ADC_UNIT + 1,
                         (int)s_slots[i].chan, esp_err_to_name(rerr));
                return BSP_ADC_CH_INVALID;
            }
            s_slots[i].bitwidth = bitwidth;
            return i;
        }
    }

    int slot = -1;
    for (int i = 0; i < BSP_ADC_MAX_CHANNELS; i++) {
        if (!s_slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGE(TAG, "模拟通道登记表已满(%d)，无法再登记 GPIO%d", BSP_ADC_MAX_CHANNELS, gpio);
        return BSP_ADC_CH_INVALID;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = atten,
        .bitwidth = bitwidth,
    };

    const esp_err_t cfg_err = adc_oneshot_config_channel(s_unit, chan, &chan_cfg);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel(ADC%d_CH%d, GPIO%d) failed: %s", (int)BSP_ADC_UNIT + 1,
                 (int)chan, gpio, esp_err_to_name(cfg_err));
        return BSP_ADC_CH_INVALID;
    }

    s_slots[slot].used     = true;
    s_slots[slot].chan     = chan;
    s_slots[slot].bitwidth = bitwidth;
    s_slots[slot].gpio     = gpio;

    ESP_LOGI(TAG, "ch%d <= GPIO%d = ADC%d_CH%d (atten=%d, bitwidth=%d, 满量程 %d)", slot, gpio,
             (int)BSP_ADC_UNIT + 1, (int)chan, (int)atten, (int)bitwidth, adc_full_scale_of(bitwidth));
    return slot;
}

esp_err_t bsp_adc_read_raw(bsp_adc_ch_t ch, int *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const adc_slot_t *slot = adc_slot_of(ch);
    if (slot == NULL) {
        ESP_LOGE(TAG, "read_raw: 非法通道句柄 %d（需先用 bsp_adc_channel_add 登记）", ch);
        return ESP_ERR_INVALID_ARG;
    }

    // 悬空脚读数不可信
    return adc_oneshot_read(s_unit, slot->chan, out);
}

// 没校准走线性近似
esp_err_t bsp_adc_read_mv(bsp_adc_ch_t ch, int *mv) {
    if (mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const adc_slot_t *slot = adc_slot_of(ch);
    if (slot == NULL) {
        ESP_LOGE(TAG, "read_mv: 非法通道句柄 %d", ch);
        return ESP_ERR_INVALID_ARG;
    }

    int             raw = 0;
    const esp_err_t err = bsp_adc_read_raw(ch, &raw);
    if (err != ESP_OK) {
        // 不再重复打日志
        return err;
    }

    if (s_cali_valid && s_cali != NULL) {
        const esp_err_t cali_err = adc_cali_raw_to_voltage(s_cali, raw, mv);
        if (cali_err == ESP_OK) {
            return ESP_OK;
        }
        // 转换失败退回线性
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed(%s), 本次用线性近似", esp_err_to_name(cali_err));
    }

    const int full = adc_full_scale_of(slot->bitwidth);
    *mv            = (int)(((int32_t)raw * ADC_LINEAR_FULL_SCALE_MV) / full);
    return ESP_OK;
}

// 取平均抗抖动
// 单次失败只丢掉
esp_err_t bsp_adc_read_avg(bsp_adc_ch_t ch, int samples, int *out_avg) {
    if (out_avg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc_slot_of(ch) == NULL) {
        ESP_LOGE(TAG, "read_avg: 非法通道句柄 %d", ch);
        return ESP_ERR_INVALID_ARG;
    }

    if (samples < ADC_AVG_SAMPLES_MIN) {
        samples = ADC_AVG_SAMPLES_MIN;
    } else if (samples > ADC_AVG_SAMPLES_MAX) {
        samples = ADC_AVG_SAMPLES_MAX;
    }

    int32_t   sum      = 0;
    int       ok_cnt   = 0;
    esp_err_t last_err = ESP_OK;

    for (int i = 0; i < samples; i++) {
        int             raw = 0;
        const esp_err_t err = bsp_adc_read_raw(ch, &raw);
        if (err == ESP_OK) {
            sum += raw;
            ok_cnt++;
        } else {
            // 记住最后的错
            last_err = err;
        }
    }

    if (ok_cnt == 0) {
        ESP_LOGE(TAG, "通道 %d 连读 %d 次全部失败: %s", ch, samples, esp_err_to_name(last_err));
        return (last_err != ESP_OK) ? last_err : ESP_FAIL;
    }

    // 四舍五入
    *out_avg = (int)((sum + (ok_cnt / 2)) / ok_cnt);
    return ESP_OK;
}

float bsp_adc_raw_to_pct(int raw, int max_raw) {
    // 防除零
    if (max_raw <= 0) {
        return 0.0f;
    }
    if (raw <= 0) {
        return 0.0f;
    }
    if (raw >= max_raw) {
        return 100.0f;
    }

    const float pct = ((float)raw * 100.0f) / (float)max_raw;
    if (pct < 0.0f) {
        return 0.0f;
    }
    if (pct > 100.0f) {
        return 100.0f;
    }
    return pct;
}

// 是否就绪
bool bsp_adc_ready(void) {
    return (s_inited && s_unit != NULL);
}
