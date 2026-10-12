// c
// 四路灯 LEDC 调光
#include <Arduino.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#include "app_common.h"
#include "bsp_config.h"
#include "led.h"

static const char *TAG = "LED";

// LEDC 不走 Arduino 封装
// 2.x 的 ledcSetup 固定绑定时器
// 3.x 又换成 ledcAttach
// 两版 driver/ledc.h 一模一样
// 所以直接调 IDF，通道自己排

// 只能写到 1023
#define LED_DUTY_MAX ((1u << BSP_LED_PWM_RES) - 1u)

#define LED_OK_BIT(z) (1u << (int)(z))

// 四路灯共用一个定时器
#define LED_MODE  LEDC_LOW_SPEED_MODE
#define LED_TIMER LEDC_TIMER_2

static const int s_led_gpio[BSP_LED_MAX] = {
    BSP_LED_GPIO_LIVING,
    BSP_LED_GPIO_KITCHEN,
    BSP_LED_GPIO_BEDROOM,
    BSP_LED_GPIO_BATH,
};

// 通道必须错开
static const ledc_channel_t s_led_ch[BSP_LED_MAX] = {
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
    LEDC_CHANNEL_5,
};

static const char *const s_zone_name_cn[BSP_LED_MAX] = {
    "客厅灯",
    "厨房灯",
    "卧室灯",
    "浴室灯",
};

static_assert(BSP_LED_MAX == BSP_LED_ZONE_COUNT,
              "led.h 的 BSP_LED_MAX 必须等于 bsp_config.h 的 BSP_LED_ZONE_COUNT");

// 亮度 0~100
static uint8_t s_level[BSP_LED_MAX];

// 没配上就空操作
static uint8_t s_zone_ok_mask;

static bool s_inited;

static bool led_zone_valid(bsp_led_zone_t zone) {
    return ((int)zone >= 0) && ((int)zone < BSP_LED_MAX);
}

static app_err_t led_apply(bsp_led_zone_t zone) {
    if (!(s_zone_ok_mask & LED_OK_BIT(zone))) {
        // 没接就当没这路
        return APP_OK;
    }

    uint32_t duty = (uint32_t)s_level[zone] * LED_DUTY_MAX / 100u;

#if BSP_LED_ACTIVE_LOW
    // 低电平点亮要反相
    duty = LED_DUTY_MAX - duty;
#endif

    esp_err_t err = ledc_set_duty(LED_MODE, s_led_ch[zone], duty);
    if (err != ESP_OK) {
        LOGW(TAG, "%s 设置占空比失败: %s（duty=%u）", s_zone_name_cn[zone], esp_err_to_name(err),
             (unsigned)duty);
        // 失败就返回
        return APP_ERR_FAIL;
    }

    err = ledc_update_duty(LED_MODE, s_led_ch[zone]);
    if (err != ESP_OK) {
        LOGW(TAG, "%s 提交占空比失败: %s", s_zone_name_cn[zone], esp_err_to_name(err));
        return APP_ERR_FAIL;
    }
    return APP_OK;
}

// 把硬件准备好
app_err_t bsp_led_init(void) {
    if (s_inited) {
        return APP_OK;
    }

    // 定时器只配一次
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = LED_MODE;
    tcfg.duty_resolution = (ledc_timer_bit_t)BSP_LED_PWM_RES;
    tcfg.timer_num       = LED_TIMER;
    tcfg.freq_hz         = BSP_LED_PWM_FREQ_HZ;
    tcfg.clk_cfg         = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        // 定时器失败就退出
        LOGE(TAG, "ledc_timer_config(TIMER_%d, %dHz) 失败: %s", (int)LED_TIMER,
             (int)BSP_LED_PWM_FREQ_HZ, esp_err_to_name(err));
        // 标记已试过
        s_inited = true;
        return APP_ERR_FAIL;
    }

    int ok_cnt = 0;

    for (int i = 0; i < BSP_LED_MAX; i++) {
        ledc_channel_config_t ccfg = {};
        ccfg.gpio_num   = s_led_gpio[i];
        ccfg.speed_mode = LED_MODE;
        ccfg.channel    = s_led_ch[i];
        ccfg.intr_type  = LEDC_INTR_DISABLE;
        ccfg.timer_sel  = LED_TIMER;
        ccfg.duty       = 0;
        ccfg.hpoint     = 0;

        const esp_err_t e = ledc_channel_config(&ccfg);
        if (e != ESP_OK) {
            // 单路坏了不影响别的
            LOGW(TAG, "%s (GPIO%d, LEDC CH%d) PWM 配置失败: %s —— 本路不可用，其余灯正常",
                 s_zone_name_cn[i], s_led_gpio[i], (int)s_led_ch[i], esp_err_to_name(e));
            continue;
        }

        s_zone_ok_mask |= (uint8_t)LED_OK_BIT(i);
        s_level[i] = 0;
        ok_cnt++;

        // 0 占空比会全亮
        (void)led_apply((bsp_led_zone_t)i);
    }

    s_inited = true;

    if (ok_cnt > 0) {
        LOGI(TAG, "init ok: %d/%d 路灯可用（TIMER_%d %dHz, %dbit, CH2~CH5）", ok_cnt, BSP_LED_MAX,
             (int)LED_TIMER, (int)BSP_LED_PWM_FREQ_HZ, (int)BSP_LED_PWM_RES);
    } else {
        LOGE(TAG, "init failed: 0/%d 路灯可用（4 路 PWM 通道全部配置失败）", BSP_LED_MAX);
    }

    return (ok_cnt > 0) ? APP_OK : APP_ERR_FAIL;
}

// 开关灯也走这里
app_err_t bsp_led_set_level(bsp_led_zone_t zone, int pct) {
    if (!led_zone_valid(zone)) {
        return APP_ERR_INVALID_ARG;
    }

    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    s_level[zone] = (uint8_t)pct;
    return led_apply(zone);
}

app_err_t bsp_led_set_power(bsp_led_zone_t zone, bool on) {
    return bsp_led_set_level(zone, on ? 100 : 0);
}

int bsp_led_get_level(bsp_led_zone_t zone) {
    return led_zone_valid(zone) ? (int)s_level[zone] : 0;
}

// 看的是设定值
bool bsp_led_is_on(bsp_led_zone_t zone) {
    return led_zone_valid(zone) && (s_level[zone] > 0);
}

// 遇错不能停
app_err_t bsp_led_set_all(int pct) {
    app_err_t ret = APP_OK;

    for (int i = 0; i < BSP_LED_MAX; i++) {
        if (bsp_led_set_level((bsp_led_zone_t)i, pct) != APP_OK) {
            ret = APP_ERR_FAIL;
        }
    }
    return ret;
}

app_err_t bsp_led_all_off(void) {
    return bsp_led_set_all(0);
}

// 只接一路也算能用
bool bsp_led_ready(void) {
    return (s_zone_ok_mask != 0);
}

const char *bsp_led_zone_name(bsp_led_zone_t zone) {
    return led_zone_valid(zone) ? s_zone_name_cn[zone] : "未知灯";
}
