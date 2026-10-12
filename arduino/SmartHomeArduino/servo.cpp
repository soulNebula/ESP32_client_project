// c
// 门舵机 PWM 控制
#include <Arduino.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#include "app_common.h"
#include "bsp_config.h"
#include "servo.h"

static const char *TAG = "SERVO";

// 舵机独占一个定时器
#define SERVO_MODE  LEDC_LOW_SPEED_MODE
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_CH    LEDC_CHANNEL_0

// 周期 20ms
#define SERVO_PERIOD_US (1000000 / BSP_SERVO_FREQ_HZ)

// 5.x 把这个宏改名了
#if !defined(SOC_LEDC_TIMER_BIT_WIDE_NUM) && defined(SOC_LEDC_TIMER_BIT_WIDTH)
#define SOC_LEDC_TIMER_BIT_WIDE_NUM SOC_LEDC_TIMER_BIT_WIDTH
#endif

// 两个名字都没有就按 14 兜底
#ifndef SOC_LEDC_TIMER_BIT_WIDE_NUM
#define SOC_LEDC_TIMER_BIT_WIDE_NUM 14
#endif

// 板子是 S3，LEDC 只到 14bit
// bsp_config 写的 16 装不下
#if BSP_SERVO_RES > SOC_LEDC_TIMER_BIT_WIDE_NUM
#define SERVO_RES_BITS SOC_LEDC_TIMER_BIT_WIDE_NUM
#else
#define SERVO_RES_BITS BSP_SERVO_RES
#endif

// 别写 2^res
#define SERVO_DUTY_MAX ((1u << SERVO_RES_BITS) - 1u)

// 行程 180°
#define SERVO_ANGLE_SPAN 180.0f

// 配好了吗
static bool s_ready;
// 松劲了吗
static bool s_released;
// 开合百分比
static int s_pct;
// 当前角度
static float s_deg;

// +0.5 别省
static uint32_t angle_to_pulse_us(float deg) {
    const float span = (float)(BSP_SERVO_MAX_PULSE_US - BSP_SERVO_MIN_PULSE_US);
    return (uint32_t)((float)BSP_SERVO_MIN_PULSE_US + span * (deg / SERVO_ANGLE_SPAN) + 0.5f);
}

static uint32_t pulse_to_duty(uint32_t us) {
    if (us > SERVO_PERIOD_US) {
        // 超周期就夹紧
        us = SERVO_PERIOD_US;
    }
    return (uint32_t)(((uint64_t)us * SERVO_DUTY_MAX) / SERVO_PERIOD_US);
}

static app_err_t servo_write_duty(uint32_t duty) {
    esp_err_t err = ledc_set_duty(SERVO_MODE, SERVO_CH, duty);
    if (err != ESP_OK) {
        LOGW(TAG, "ledc_set_duty 失败: %s（duty=%u）", esp_err_to_name(err), (unsigned)duty);
        return APP_ERR_FAIL;
    }

    err = ledc_update_duty(SERVO_MODE, SERVO_CH);
    if (err != ESP_OK) {
        LOGW(TAG, "ledc_update_duty 失败: %s", esp_err_to_name(err));
        return APP_ERR_FAIL;
    }
    return APP_OK;
}

static int deg_to_pct(float deg) {
    const float span = BSP_SERVO_DOOR_OPEN_DEG - BSP_SERVO_DOOR_SHUT_DEG;
    if (span == 0.0f) {
        // 防除零
        return 0;
    }

    float pct = (deg - BSP_SERVO_DOOR_SHUT_DEG) * 100.0f / span;
    if (pct < 0.0f) {
        pct = 0.0f;
    } else if (pct > 100.0f) {
        pct = 100.0f;
    }
    return (int)(pct + 0.5f);
}

// 上电先关门
app_err_t bsp_servo_init(void) {
    if (s_ready) {
        // 配过了
        return APP_OK;
    }

#if BSP_SERVO_RES > SOC_LEDC_TIMER_BIT_WIDE_NUM
    // 说清楚为什么降bit
    LOGW(TAG, "本芯片 LEDC 最多 %dbit，装不下 %dbit，舵机按 %dbit 跑", (int)SOC_LEDC_TIMER_BIT_WIDE_NUM,
         (int)BSP_SERVO_RES, (int)SERVO_RES_BITS);
#endif

    // 通道号不能重复用
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = SERVO_MODE;
    tcfg.duty_resolution = (ledc_timer_bit_t)SERVO_RES_BITS;
    tcfg.timer_num       = SERVO_TIMER;
    tcfg.freq_hz         = BSP_SERVO_FREQ_HZ;
    tcfg.clk_cfg         = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        // 失败不 abort
        LOGE(TAG, "ledc_timer_config(TIMER_%d, %dHz) 失败: %s —— 门舵机不可用", (int)SERVO_TIMER,
             (int)BSP_SERVO_FREQ_HZ, esp_err_to_name(err));
        s_pct = deg_to_pct(BSP_SERVO_DOOR_SHUT_DEG);
        s_deg = BSP_SERVO_DOOR_SHUT_DEG;
        return APP_ERR_FAIL;
    }

    // 初始占空比 0
    ledc_channel_config_t ccfg = {};
    ccfg.gpio_num   = BSP_SERVO_GPIO_DOOR;
    ccfg.speed_mode = SERVO_MODE;
    ccfg.channel    = SERVO_CH;
    ccfg.intr_type  = LEDC_INTR_DISABLE;
    ccfg.timer_sel  = SERVO_TIMER;
    ccfg.duty       = 0;
    ccfg.hpoint     = 0;

    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        LOGE(TAG, "ledc_channel_config(GPIO%d, CH%d) 失败: %s —— 门舵机不可用", (int)BSP_SERVO_GPIO_DOOR,
             (int)SERVO_CH, esp_err_to_name(err));
        s_pct = deg_to_pct(BSP_SERVO_DOOR_SHUT_DEG);
        s_deg = BSP_SERVO_DOOR_SHUT_DEG;
        return APP_ERR_FAIL;
    }

    // 就位后马上关门
    s_ready    = true;
    s_released = false;
    s_deg      = BSP_SERVO_DOOR_SHUT_DEG;
    s_pct      = deg_to_pct(BSP_SERVO_DOOR_SHUT_DEG);

    (void)bsp_servo_door_set_deg(BSP_SERVO_DOOR_SHUT_DEG);

    LOGI(TAG, "init ok: 1/1 路舵机可用（门开=%d°，关=%d°）", (int)BSP_SERVO_DOOR_OPEN_DEG,
         (int)BSP_SERVO_DOOR_SHUT_DEG);
    return APP_OK;
}

app_err_t bsp_servo_door_set(bool open) {
    const float    deg = open ? BSP_SERVO_DOOR_OPEN_DEG : BSP_SERVO_DOOR_SHUT_DEG;
    const app_err_t err = bsp_servo_door_set_deg(deg);

    LOGI(TAG, "door -> %s（%d°）%s", open ? "开" : "关", (int)deg, (err == APP_OK) ? "" : "（写入失败）");
    return err;
}

app_err_t bsp_servo_door_set_pct(int pct) {
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    const float deg =
        BSP_SERVO_DOOR_SHUT_DEG + (BSP_SERVO_DOOR_OPEN_DEG - BSP_SERVO_DOOR_SHUT_DEG) * ((float)pct / 100.0f);

    return bsp_servo_door_set_deg(deg);
}

// 没配上也算成功
app_err_t bsp_servo_door_set_deg(float deg) {
    if (deg < 0.0f) {
        deg = 0.0f;
    } else if (deg > SERVO_ANGLE_SPAN) {
        deg = SERVO_ANGLE_SPAN;
    }

    if (s_released) {
        // 松劲后自动恢复
        LOGD(TAG, "从松劲状态恢复脉冲输出");
    }

    s_deg      = deg;
    s_pct      = deg_to_pct(deg);
    s_released = false;

    if (!s_ready) {
        return APP_OK;
    }

    const uint32_t us   = angle_to_pulse_us(deg);
    const uint32_t duty = pulse_to_duty(us);

    // 别打 %f
    LOGD(TAG, "deg=%d.%d → pulse=%uus → duty=%u", (int)deg, (int)((deg - (float)(int)deg) * 10.0f),
         (unsigned)us, (unsigned)duty);
    return servo_write_duty(duty);
}

// 只是软件状态
bool bsp_servo_door_is_open(void) {
    return (s_pct >= 50);
}

int bsp_servo_door_get_pct(void) {
    return s_pct;
}

// 松劲给占空比 0
app_err_t bsp_servo_door_release(void) {
    s_released = true;

    if (!s_ready) {
        return APP_OK;
    }

    const app_err_t err = servo_write_duty(0);
    LOGI(TAG, "door release（松劲，占空比=0，门不再被保持）");
    return err;
}

bool bsp_servo_ready(void) {
    return s_ready;
}
