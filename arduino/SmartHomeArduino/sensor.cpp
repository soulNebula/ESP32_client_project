// c
// 温湿度光照雨滴采集
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "app_common.h"
#include "app_config.h"
#include "bsp_config.h"
#include "i2c_bus.h"
#include "sensor.h"

static const char *TAG = "SENSOR";

// 缺省兜底
#ifndef APP_SENSOR_PERIOD_MS
#define APP_SENSOR_PERIOD_MS 1000
#endif

// 3.x 把 11db 改名了
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#ifndef ADC_11db
#define ADC_11db ADC_ATTEN_DB_12
#endif
#endif

// 防设成 1ms
#define SENSORS_MIN_PERIOD_MS 500

// 低通系数
#define SENSORS_FILTER_ALPHA 0.30f

// 连错判掉线
#define SENSORS_FAIL_LIMIT 3
// 连错判不可信
#define SENSORS_CRC_FAIL_LIMIT 5

// 12bit 满量程
#define SENSORS_ADC_FULL_SCALE ((1 << BSP_ADC_RESOLUTION) - 1)
// 连读 8 次平均
#define SENSORS_LDR_SAMPLES 8
// 同上
#define SENSORS_RAIN_SAMPLES 8

// S3 只有 GPIO1~GPIO10 是 ADC1
// ADC2 开了 WiFi 就废
#define SENSORS_ADC1_GPIO_MIN 1
#define SENSORS_ADC1_GPIO_MAX 10

// 悬空判据
#define SENSORS_LDR_RAW_LO 40
#define SENSORS_LDR_RAW_HI 4030

// 贴导轨判悬空
#define SENSORS_RAIN_RAW_RAIL_LO 20
#define SENSORS_RAIN_RAW_RAIL_HI 4075
// 抗单次毛刺
#define SENSORS_RAIN_RAIL_LIMIT 3
// 防溢出
#define SENSORS_RAIN_RAIL_CNT_MAX 255

// 光照满量程
#define SENSORS_LUX_PER_PCT 5.0f

// 单次高重复性
#define SHT30_CMD_MEAS_HIGH 0x2C06
// 最大 15ms
#define SHT30_MEAS_DELAY_MS 20
// 帧 6 字节
#define SHT30_FRAME_LEN 6

// 两条都发更稳
#define AHT20_CMD_INIT 0xBE
#define AHT20_CMD_CAL  0xE1
#define AHT20_CMD_MEAS 0xAC
#define AHT20_ARG_0    0x00
#define AHT20_ARG_INIT 0x08
#define AHT20_ARG_MEAS 0x33
// 校准约 10ms
#define AHT20_INIT_DELAY_MS 10
// 最长 75ms
#define AHT20_MEAS_DELAY_MS 80
// bit7 是忙
#define AHT20_STATUS_BUSY 0x80
// 状态+数据
#define AHT20_FRAME_LEN 7

#define BH1750_CMD_POWER_ON   0x01
#define BH1750_CMD_CONT_H_RES 0x10
// 上电等稳定
#define BH1750_POWER_ON_DELAY_MS 10
// 一次 120ms
#define BH1750_MEAS_MS 120
// 除以 1.2
#define BH1750_LUX_DIV 1.2f

// ±3% 迟滞
#define SENSORS_RAIN_HYST_PCT 3.0f

// 跟原工程一致取 0
// bsp_config 那边写的是 1
#define SENSORS_RAIN_ADC_INVERT 0

// 物理合法区间
#define SENSORS_TH_TEMP_MIN (-45.0f)
#define SENSORS_TH_TEMP_MAX (130.0f)

// 内部哨兵，只在文件内用
#define SENSORS_ERR_CRC  (-101)
#define SENSORS_ERR_BUSY (-102)

// 临界区别做 I2C
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_inited;
// 最近快照
static sensor_snapshot_t s_snap;

static sensor_th_kind_t    s_th_kind = SENSOR_TH_NONE;
static sensor_light_kind_t s_light_kind = SENSOR_LIGHT_NONE;

// 0 表示没这个器件
static uint8_t s_th_addr;
static uint8_t s_light_addr;

// 仅光敏方案登记
static bool s_ldr_ok;
static bool s_rain_ok;

// 连错就掉线
static int s_th_fail;
// 连错不可信
static int s_th_crc;
static int s_light_fail;
static int s_rain_rail_cnt;
// 节流时间戳
static int64_t s_bh1750_last_ms;

// 首次直采真值
static bool s_filt_temp;
static bool s_filt_hum;
static bool s_filt_light_pct;
static bool s_filt_rain;

// 告警只打一次
static bool s_ldr_warned;
static bool s_rain_warned;

// 自动采样开关
static bool s_auto_run;
static uint32_t s_period_ms = APP_SENSOR_PERIOD_MS;
static int64_t  s_last_sample_ms;

static float s_rain_threshold = BSP_DEF_RAIN_PCT;

// 夹到 lo~hi
static float sensors_clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

// 一阶低通
static float sensors_lowpass(float prev, float sample, bool *inited) {
    if (!*inited) {
        *inited = true;
        return sample;
    }
    return prev + SENSORS_FILTER_ALPHA * (sample - prev);
}

// CRC-8 0x31
static uint8_t sensors_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// 原子拷快照
static void sensors_snapshot_copy(sensor_snapshot_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_snap;
    portEXIT_CRITICAL(&s_mux);
}

// 这一脚能做 ADC 吗
static bool sensors_adc_pin_ok(int gpio) {
    return (gpio >= SENSORS_ADC1_GPIO_MIN) && (gpio <= SENSORS_ADC1_GPIO_MAX);
}

// 取平均抗抖动
static app_err_t sensors_adc_read_avg(int gpio, int samples, int *out_avg) {
    if ((out_avg == NULL) || !sensors_adc_pin_ok(gpio)) {
        return APP_ERR_INVALID_ARG;
    }

    int32_t sum    = 0;
    int     ok_cnt = 0;

    for (int i = 0; i < samples; i++) {
        // analogRead 不报错，只会给数
        const int raw = (int)analogRead((uint8_t)gpio);
        if (raw >= 0) {
            sum += raw;
            ok_cnt++;
        }
    }

    if (ok_cnt == 0) {
        return APP_ERR_FAIL;
    }

    // 四舍五入
    *out_avg = (int)((sum + (ok_cnt / 2)) / ok_cnt);
    return APP_OK;
}

// 原始值换算成百分比
static float sensors_raw_to_pct(int raw) {
    if (SENSORS_ADC_FULL_SCALE <= 0) {
        return 0.0f;
    }
    if (raw <= 0) {
        return 0.0f;
    }
    if (raw >= SENSORS_ADC_FULL_SCALE) {
        return 100.0f;
    }

    const float pct = ((float)raw * 100.0f) / (float)SENSORS_ADC_FULL_SCALE;
    if (pct < 0.0f) {
        return 0.0f;
    }
    if (pct > 100.0f) {
        return 100.0f;
    }
    return pct;
}

// 命令式器件
static app_err_t sensors_sht30_read(float *out_temp, float *out_rh) {
    if (s_th_addr == 0) {
        return APP_ERR_INVALID_STATE;
    }

    const uint8_t cmd[2] = {
        (uint8_t)(SHT30_CMD_MEAS_HIGH >> 8),
        (uint8_t)(SHT30_CMD_MEAS_HIGH & 0xFF),
    };

    app_err_t err = bsp_i2c_write(s_th_addr, cmd, (int)sizeof(cmd));
    if (err != APP_OK) {
        return err;
    }

    // 最大 15ms
    delay(SHT30_MEAS_DELAY_MS);

    uint8_t buf[SHT30_FRAME_LEN] = {0};
    err                          = bsp_i2c_read(s_th_addr, buf, (int)sizeof(buf));
    if (err != APP_OK) {
        return err;
    }

    // CRC 错就整帧弃
    if ((sensors_crc8(&buf[0], 2) != buf[2]) || (sensors_crc8(&buf[3], 2) != buf[5])) {
        return SENSORS_ERR_CRC;
    }

    const uint16_t raw_t  = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    const uint16_t raw_rh = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

    // 先转浮点再除
    *out_temp = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    *out_rh   = sensors_clampf(100.0f * (float)raw_rh / 65535.0f, 0.0f, 100.0f);
    return APP_OK;
}

// 失败不致命
static app_err_t sensors_aht20_init(void) {
    if (s_th_addr == 0) {
        return APP_ERR_INVALID_STATE;
    }

    const uint8_t cmd_init[3] = {AHT20_CMD_INIT, AHT20_ARG_INIT, AHT20_ARG_0};
    app_err_t     err         = bsp_i2c_write(s_th_addr, cmd_init, (int)sizeof(cmd_init));
    if (err != APP_OK) {
        return err;
    }
    delay(AHT20_INIT_DELAY_MS);

    const uint8_t cmd_cal[3] = {AHT20_CMD_CAL, AHT20_ARG_INIT, AHT20_ARG_0};
    err                      = bsp_i2c_write(s_th_addr, cmd_cal, (int)sizeof(cmd_cal));
    if (err != APP_OK) {
        return err;
    }
    delay(AHT20_INIT_DELAY_MS);
    return APP_OK;
}

// 先湿度后温度
static app_err_t sensors_aht20_read(float *out_temp, float *out_rh) {
    if (s_th_addr == 0) {
        return APP_ERR_INVALID_STATE;
    }

    const uint8_t cmd[3] = {AHT20_CMD_MEAS, AHT20_ARG_MEAS, AHT20_ARG_0};
    app_err_t     err    = bsp_i2c_write(s_th_addr, cmd, (int)sizeof(cmd));
    if (err != APP_OK) {
        return err;
    }

    // 最长 75ms
    delay(AHT20_MEAS_DELAY_MS);

    uint8_t buf[AHT20_FRAME_LEN] = {0};
    err                          = bsp_i2c_read(s_th_addr, buf, (int)sizeof(buf));
    if (err != APP_OK) {
        return err;
    }

    // 忙不算掉线
    if ((buf[0] & AHT20_STATUS_BUSY) != 0) {
        return SENSORS_ERR_BUSY;
    }

    if (sensors_crc8(&buf[0], 6) != buf[6]) {
        return SENSORS_ERR_CRC;
    }

    const uint32_t raw_rh =
        (((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4));
    const uint32_t raw_t =
        ((((uint32_t)buf[3] & 0x0FU) << 16) | ((uint32_t)buf[4] << 8) | (uint32_t)buf[5]);

    *out_rh   = sensors_clampf((float)raw_rh * 100.0f / 1048576.0f, 0.0f, 100.0f);
    *out_temp = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
    return APP_OK;
}

// 连错摘 valid
static void sensors_th_data_error(sensor_snapshot_t *s, const char *why) {
    s_th_crc++;
    if (s_th_crc >= SENSORS_CRC_FAIL_LIMIT) {
        s->th_valid = false;
        s_th_crc    = 0;
        LOGE(TAG,
             "温湿度连续 %d 次数据不可信（%s）→ th_valid=false："
             "查接线/上拉/供电，AHT20 克隆片还可能不支持 CRC 校验",
             SENSORS_CRC_FAIL_LIMIT, why);
    } else {
        LOGW(TAG, "温湿度数据不可信（%s，连续第 %d 次），本次作废、保留上次值", why, s_th_crc);
    }
}

// 三条错误分开算
static void sensors_sample_th(sensor_snapshot_t *s) {
    if (s_th_kind == SENSOR_TH_NONE) {
        s->th_valid = false;
        return;
    }

    float t  = 0.0f;
    float rh = 0.0f;
    const app_err_t err =
        (s_th_kind == SENSOR_TH_SHT30) ? sensors_sht30_read(&t, &rh) : sensors_aht20_read(&t, &rh);

    if (err == APP_OK) {
        // 第二道防线
        if ((t < SENSORS_TH_TEMP_MIN) || (t > SENSORS_TH_TEMP_MAX) || (rh < 0.0f) || (rh > 100.0f)) {
            sensors_th_data_error(s, "数值越界");
            return;
        }

        if (!s->th_valid) {
            LOGI(TAG, "温湿度传感器恢复读数");
        }
        s_th_fail      = 0;
        s_th_crc       = 0;
        s->th_valid    = true;
        s->temperature = sensors_lowpass(s->temperature, t, &s_filt_temp);
        s->humidity    = sensors_lowpass(s->humidity, rh, &s_filt_hum);
        return;
    }

    if (err == SENSORS_ERR_BUSY) {
        // 还在测本轮跳过
        return;
    }

    if (err == SENSORS_ERR_CRC) {
        sensors_th_data_error(s, "CRC 校验失败");
        return;
    }

    s_th_fail++;
    s_th_crc = 0;
    if (s_th_fail >= SENSORS_FAIL_LIMIT) {
        s->th_valid = false;
        // 重取滤波初值
        s_filt_temp = false;
        s_filt_hum  = false;
        LOGW(TAG, "温湿度连续 %d 次通信失败（%s）→ th_valid=false", s_th_fail, app_err_text(err));
    }
}

// 连续模式省等
static app_err_t sensors_bh1750_begin(void) {
    if (s_light_addr == 0) {
        return APP_ERR_INVALID_STATE;
    }

    uint8_t   cmd = BH1750_CMD_POWER_ON;
    app_err_t err = bsp_i2c_write(s_light_addr, &cmd, 1);
    if (err != APP_OK) {
        return err;
    }
    delay(BH1750_POWER_ON_DELAY_MS);

    cmd = BH1750_CMD_CONT_H_RES;
    return bsp_i2c_write(s_light_addr, &cmd, 1);
}

// 裸读 2 字节
static app_err_t sensors_bh1750_read(float *out_lux) {
    if (s_light_addr == 0) {
        return APP_ERR_INVALID_STATE;
    }

    uint8_t         buf[2] = {0};
    const app_err_t err    = bsp_i2c_read(s_light_addr, buf, (int)sizeof(buf));
    if (err != APP_OK) {
        return err;
    }

    const uint16_t raw = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    *out_lux           = (float)raw / BH1750_LUX_DIV;
    return APP_OK;
}

// 按宏处理极性
static float sensors_ldr_raw_to_pct(int raw) {
    float pct = sensors_raw_to_pct(raw);
#if BSP_LIGHT_ADC_INVERT
    pct = 100.0f - pct;
#endif
    return sensors_clampf(pct, 0.0f, 100.0f);
}

// 越大越湿
static float sensors_rain_raw_to_pct(int raw) {
    float pct = sensors_raw_to_pct(raw);
#if SENSORS_RAIN_ADC_INVERT
    pct = 100.0f - pct;
#endif
    return sensors_clampf(pct, 0.0f, 100.0f);
}

// BH1750 需节流
// 光敏悬空冻结
static void sensors_sample_light(sensor_snapshot_t *s) {
    if (s_light_kind == SENSOR_LIGHT_BH1750) {
        const int64_t now_ms = app_uptime_ms();
        if ((now_ms - s_bh1750_last_ms) < (int64_t)BH1750_MEAS_MS) {
            // 沿用上次值
            return;
        }
        s_bh1750_last_ms = now_ms;

        float           lux = 0.0f;
        const app_err_t err = sensors_bh1750_read(&lux);
        if (err == APP_OK) {
            s_light_fail   = 0;
            s->light_valid = true;
            s->light_lux   = (lux < 0.0f) ? 0.0f : lux;
            s->light_pct   = sensors_clampf(s->light_lux / SENSORS_LUX_PER_PCT, 0.0f, 100.0f);
            return;
        }

        s_light_fail++;
        if (s_light_fail >= SENSORS_FAIL_LIMIT) {
            // 不退回光敏
            s->light_valid = false;
            LOGW(TAG, "BH1750 连续 %d 次读取失败（%s）→ light_valid=false", s_light_fail,
                 app_err_text(err));
        } else {
            LOGW(TAG, "BH1750 读取失败（%s），保留上次 lux", app_err_text(err));
        }
        return;
    }

    if (s_light_kind == SENSOR_LIGHT_LDR) {
        int raw = 0;
        if (sensors_adc_read_avg(BSP_GPIO_LIGHT_ADC, SENSORS_LDR_SAMPLES, &raw) != APP_OK) {
            if (!s_ldr_warned) {
                s_ldr_warned = true;
                LOGW(TAG, "光敏电阻 ADC 读取失败 → light_valid=false");
            }
            s->light_valid = false;
            return;
        }

        // 悬空贴导轨
        const bool plausible = (raw > SENSORS_LDR_RAW_LO) && (raw < SENSORS_LDR_RAW_HI);
        s->light_valid       = plausible;

        if (!plausible) {
            if (!s_ldr_warned) {
                s_ldr_warned = true;
                LOGW(TAG,
                     "光敏电阻读数贴在 ADC 导轨（raw=%d，可信窗口 %d~%d）→ light_valid=false："
                     "十有八九没接光敏电阻；若确实接了请看 SENSORS_LDR_RAW_LO/HI 的说明",
                     raw, SENSORS_LDR_RAW_LO, SENSORS_LDR_RAW_HI);
            }
            // 冻结不发噪声
            return;
        }
        s_ldr_warned = false;

        const float pct = sensors_ldr_raw_to_pct(raw);
        s->light_pct    = sensors_lowpass(s->light_pct, pct, &s_filt_light_pct);
        // 光敏 lux 是估算
        s->light_lux = s->light_pct * SENSORS_LUX_PER_PCT;
        return;
    }

    s->light_valid = false;
}

// 贴导轨强制停雨
// 迟滞防抖
static void sensors_sample_rain(sensor_snapshot_t *s) {
    if (!s_rain_ok) {
        s->rain_valid = false;
        s->raining    = false;
        return;
    }

    int raw = 0;
    if (sensors_adc_read_avg(BSP_GPIO_RAIN_AO, SENSORS_RAIN_SAMPLES, &raw) != APP_OK) {
        if (!s_rain_warned) {
            s_rain_warned = true;
            LOGW(TAG, "雨滴 ADC 读取失败 → rain_valid=false");
        }
        s_rain_rail_cnt = 0;
        s->rain_valid   = false;
        s->raining      = false;
        return;
    }

    const bool rail = (raw < SENSORS_RAIN_RAW_RAIL_LO) || (raw > SENSORS_RAIN_RAW_RAIL_HI);
    if (rail) {
        if (s_rain_rail_cnt < SENSORS_RAIN_RAIL_CNT_MAX) {
            s_rain_rail_cnt++;
        }
        s->rain_valid = (s_rain_rail_cnt < SENSORS_RAIN_RAIL_LIMIT);

        if (!s->rain_valid) {
            if (s->raining) {
                s->raining = false;
                LOGI(TAG, "天气状态 → Sunny（雨滴数据不可信，强制退出下雨状态）");
            }
            if (!s_rain_warned) {
                s_rain_warned = true;
                LOGW(TAG,
                     "雨滴读数连续 %d 次贴在 ADC 导轨（raw=%d，可信区间 %d~%d）→ rain_valid=false："
                     "通常是模块没接/没共地/没供电（悬空引脚的两端漂移）",
                     s_rain_rail_cnt, raw, SENSORS_RAIN_RAW_RAIL_LO, SENSORS_RAIN_RAW_RAIL_HI);
            }
        }
        // 贴导轨不采用
        return;
    }

    s_rain_rail_cnt = 0;
    s_rain_warned   = false;
    s->rain_valid   = true;
    s->rain_pct     = sensors_lowpass(s->rain_pct, sensors_rain_raw_to_pct(raw), &s_filt_rain);

    // 阈值运行时可改
    float th = 0.0f;
    portENTER_CRITICAL(&s_mux);
    th = s_rain_threshold;
    portEXIT_CRITICAL(&s_mux);

    if (!s->raining) {
        if (s->rain_pct > (th + SENSORS_RAIN_HYST_PCT)) {
            s->raining = true;
            LOGI(TAG, "天气状态 → Raining!（rain_pct=%.1f%%）", (double)s->rain_pct);
        }
    } else {
        if (s->rain_pct < (th - SENSORS_RAIN_HYST_PCT)) {
            s->raining = false;
            LOGI(TAG, "天气状态 → Sunny（rain_pct=%.1f%%）", (double)s->rain_pct);
        }
    }
}

// 先 SHT30
static void sensors_probe_th(void) {
    if (!bsp_i2c_ready()) {
        LOGW(TAG, "I2C 总线不可用，温湿度无法探测 → th_valid=false");
        return;
    }

    if (bsp_i2c_probe((uint8_t)BSP_I2C_ADDR_SHT30)) {
        s_th_addr      = (uint8_t)BSP_I2C_ADDR_SHT30;
        s_th_kind      = SENSOR_TH_SHT30;
        s_snap.th_kind = SENSOR_TH_SHT30;
        LOGI(TAG, "温湿度：检测到 SHT30/SHT31（0x%02X，%dkHz）", (unsigned)BSP_I2C_ADDR_SHT30,
             (int)(BSP_I2C_FREQ_HZ / 1000));
        return;
    }

    if (bsp_i2c_probe((uint8_t)BSP_I2C_ADDR_AHT20)) {
        s_th_addr      = (uint8_t)BSP_I2C_ADDR_AHT20;
        s_th_kind      = SENSOR_TH_AHT20;
        s_snap.th_kind = SENSOR_TH_AHT20;
        LOGI(TAG, "温湿度：检测到 AHT20/AHT10（0x%02X）", (unsigned)BSP_I2C_ADDR_AHT20);
        if (sensors_aht20_init() != APP_OK) {
            LOGW(TAG, "AHT20 初始化/校准命令未完全成功，仍按 AHT20 尝试读取");
        }
        return;
    }

    LOGW(TAG, "温湿度：0x%02X / 0x%02X 都没扫到 → th_valid=false（温度联动将停用）",
         (unsigned)BSP_I2C_ADDR_SHT30, (unsigned)BSP_I2C_ADDR_AHT20);
}

// BH1750 优先
// 两方案互斥
static void sensors_probe_light(void) {
    if (bsp_i2c_ready() && bsp_i2c_probe((uint8_t)BSP_I2C_ADDR_BH1750)) {
        s_light_addr = (uint8_t)BSP_I2C_ADDR_BH1750;

        if (sensors_bh1750_begin() == APP_OK) {
            s_light_kind      = SENSOR_LIGHT_BH1750;
            s_snap.light_kind = SENSOR_LIGHT_BH1750;
            // 从此刻计时
            s_bh1750_last_ms = app_uptime_ms();
            LOGI(TAG, "光照：检测到 BH1750（0x%02X，%dkHz，连续 H-分辨率）→ 读真实 lux",
                 (unsigned)BSP_I2C_ADDR_BH1750, (int)(BSP_I2C_FREQ_HZ / 1000));
            return;
        }
        LOGW(TAG, "BH1750 在线但上电/测量命令失败 → 退回光敏电阻方案");
    }

    if (s_ldr_ok) {
        s_light_kind      = SENSOR_LIGHT_LDR;
        s_snap.light_kind = SENSOR_LIGHT_LDR;
        LOGI(TAG, "光照：未发现 BH1750 → 用光敏电阻（GPIO%d，ADC 等效百分比%s）", BSP_GPIO_LIGHT_ADC,
             BSP_LIGHT_ADC_INVERT ? "，已按宏取反" : "");
        return;
    }

    LOGW(TAG,
         "光照：BH1750 不在线且光敏电阻 ADC 不能用（GPIO%d 不是 ADC1 引脚？）"
         "→ light_valid 恒为 false",
         BSP_GPIO_LIGHT_ADC);
}

// 失败只降级
app_err_t bsp_sensors_init(void) {
    if (s_inited) {
        LOGI(TAG, "bsp_sensors_init 重复调用，忽略（幂等）");
        return APP_OK;
    }
    s_inited = true;

    // 安全起点
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.th_kind    = SENSOR_TH_NONE;
    s_snap.light_kind = SENSOR_LIGHT_NONE;
    s_snap.updated_ms = app_uptime_ms();
    // init 前别设阈值
    s_rain_threshold = BSP_DEF_RAIN_PCT;

    if (bsp_i2c_init() != APP_OK) {
        LOGE(TAG, "bsp_i2c_init 失败：温湿度与 BH1750 不可用（雨滴/光敏走 ADC，不受影响）");
    } else if (bsp_i2c_ready()) {
        uint8_t   found[BSP_I2C_SCAN_MAX]        = {0};
        const int n                              = bsp_i2c_scan(found, BSP_I2C_SCAN_MAX);
        char      list[BSP_I2C_SCAN_MAX * 6 + 1] = {0};
        size_t    off                            = 0;
        for (int i = 0; (i < n) && (off < (sizeof(list) - 6)); i++) {
            off += (size_t)snprintf(&list[off], sizeof(list) - off, "0x%02X ", found[i]);
        }
        // 接线问题看这里
        LOGI(TAG, "I2C 扫描到 %d 个设备: %s", n, (n > 0) ? list : "(无)");
    } else {
        LOGW(TAG, "bsp_i2c_init 返回成功但总线未就绪");
    }

    // ADC 只有 ADC1 能用
    analogReadResolution(BSP_ADC_RESOLUTION);
    LOGI(TAG, "ADC 就绪：%dbit 满量程 %d，只用 ADC1（GPIO%d~GPIO%d）", (int)BSP_ADC_RESOLUTION,
         SENSORS_ADC_FULL_SCALE, SENSORS_ADC1_GPIO_MIN, SENSORS_ADC1_GPIO_MAX);

    // 光敏方案备用
    if (sensors_adc_pin_ok(BSP_GPIO_LIGHT_ADC)) {
        analogSetPinAttenuation((uint8_t)BSP_GPIO_LIGHT_ADC, BSP_ADC_ATTENUATION);
        s_ldr_ok = true;
    } else {
        LOGW(TAG, "光敏电阻：GPIO%d 不是 ADC1 引脚 → light_valid 恒为 false", BSP_GPIO_LIGHT_ADC);
    }

    // 雨滴走 ADC1
    if (sensors_adc_pin_ok(BSP_GPIO_RAIN_AO)) {
        analogSetPinAttenuation((uint8_t)BSP_GPIO_RAIN_AO, BSP_ADC_ATTENUATION);
        s_rain_ok = true;
    }

    sensors_probe_th();
    sensors_probe_light();

    if (s_rain_ok) {
        // DO 不用
        LOGI(TAG,
             "雨滴：AO=GPIO%d（ADC，0~100%% 越大越湿），DO=GPIO%d 未使用（阈值由 AO 百分比+迟滞判定）",
             BSP_GPIO_RAIN_AO, BSP_GPIO_RAIN_DO);
    } else {
        LOGW(TAG, "雨滴：AO 通道登记失败（GPIO%d）→ rain_valid 恒为 false", BSP_GPIO_RAIN_AO);
    }

    const char *th_name    = (s_th_kind == SENSOR_TH_SHT30)   ? "SHT30/SHT31(0x44)"
                             : (s_th_kind == SENSOR_TH_AHT20) ? "AHT20/AHT10(0x38)"
                                                              : "未接(th_valid=false)";
    const char *light_name = (s_light_kind == SENSOR_LIGHT_BH1750) ? "BH1750(真实lux)"
                             : (s_light_kind == SENSOR_LIGHT_LDR)  ? "光敏电阻(等效百分比)"
                                                                   : "未接(light_valid=false)";
    const char *rain_name  = s_rain_ok ? "ADC(GPIO2)" : "未接(rain_valid=false)";

    LOGI(TAG, "init ok, 温湿度=%s, 光照方案=%s, 雨滴=%s, 下雨阈值=%.1f%%(迟滞±%.1f%%)", th_name,
         light_name, rain_name, (double)s_rain_threshold, (double)SENSORS_RAIN_HYST_PCT);
    return APP_OK;
}

// 以旧快照为底
void bsp_sensors_sample_now(void) {
    sensor_snapshot_t s;
    sensors_snapshot_copy(&s);

    sensors_sample_th(&s);
    sensors_sample_light(&s);
    sensors_sample_rain(&s);

    const int64_t now_ms = app_uptime_ms();

    portENTER_CRITICAL(&s_mux);
    s.sample_count = s_snap.sample_count + 1;
    s.updated_ms   = now_ms;
    s_snap         = s;
    portEXIT_CRITICAL(&s_mux);
}

// 只拷结构体
void bsp_sensors_get(sensor_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    sensors_snapshot_copy(out);
}

// 只是打开开关
app_err_t bsp_sensors_start_auto(void) {
    if (s_auto_run) {
        // 幂等
        return APP_OK;
    }

    uint32_t period = (uint32_t)APP_SENSOR_PERIOD_MS;
    if (period < SENSORS_MIN_PERIOD_MS) {
        LOGW(TAG, "采样周期 %u ms 太小，钳到下限 %d ms", (unsigned)period, SENSORS_MIN_PERIOD_MS);
        period = SENSORS_MIN_PERIOD_MS;
    }
    s_period_ms = period;

    // 第一拍立刻采
    s_last_sample_ms = 0;
    s_auto_run       = true;

    LOGI(TAG, "自动采样已开启：周期 %u ms（主循环调 bsp_sensors_tick）", (unsigned)s_period_ms);
    return APP_OK;
}

// 到点才采
void bsp_sensors_tick(void) {
    if (!s_inited || !s_auto_run) {
        return;
    }

    const int64_t now_ms = app_uptime_ms();
    if ((s_last_sample_ms != 0) && ((now_ms - s_last_sample_ms) < (int64_t)s_period_ms)) {
        return;
    }

    s_last_sample_ms = now_ms;
    bsp_sensors_sample_now();
}

// NaN 按 0 处理
void bsp_sensors_set_rain_threshold(float pct) {
    // 顺带挡 NaN
    if (!(pct >= 0.0f)) {
        pct = 0.0f;
    }
    if (pct > 100.0f) {
        pct = 100.0f;
    }

    portENTER_CRITICAL(&s_mux);
    s_rain_threshold = pct;
    portEXIT_CRITICAL(&s_mux);

    LOGI(TAG, "下雨判定阈值已设为 %.1f%%（带 ±%.1f%% 迟滞）", (double)pct,
         (double)SENSORS_RAIN_HYST_PCT);
}

float bsp_sensors_get_rain_threshold(void) {
    float th = 0.0f;
    portENTER_CRITICAL(&s_mux);
    th = s_rain_threshold;
    portEXIT_CRITICAL(&s_mux);
    return th;
}

// 量纲区分
bool bsp_sensors_light_is_lux(void) {
    return (s_light_kind == SENSOR_LIGHT_BH1750);
}

// 硬件在位
bool bsp_sensors_th_ready(void) {
    return (s_th_kind != SENSOR_TH_NONE);
}
