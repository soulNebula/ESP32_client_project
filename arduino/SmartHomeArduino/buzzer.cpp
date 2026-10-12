// c
// 蜂鸣器开关与极性
#include <Arduino.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#include "app_common.h"
#include "bsp_config.h"
#include "buzzer.h"
// GPIO 输出封装
#include "gpio_util.h"

static const char *TAG = "BUZZER";

// 初始化成功了吗
static bool s_inited;
// 软件想要的状态
static bool s_on;
// 已写入硬件的状态
static bool s_hw_on;
// 短鸣到期时刻
static int64_t s_beep_until_ms;

// 极性可运行期切
static bool s_active_low = (BSP_BUZZER_ACTIVE_LOW != 0);

// 引脚配成不响
static app_err_t buzzer_apply_idle(void) {
#if BSP_BUZZER_ACTIVE
    // 静音走开漏高阻
    pinMode(BSP_BUZZER_GPIO, OUTPUT_OPEN_DRAIN);
    digitalWrite(BSP_BUZZER_GPIO, HIGH);
    return APP_OK;
#else
    // 无源走 LEDC
    return APP_OK;
#endif
}

// 有源才走 GPIO
#if BSP_BUZZER_ACTIVE

// 写响停
static app_err_t buzzer_hw_write(bool on) {
    if (!on) {
        // 静音回高阻
        return buzzer_apply_idle();
    }

    // 发声切推挽
    pinMode(BSP_BUZZER_GPIO, OUTPUT);
    // 按极性拉电平
    digitalWrite(BSP_BUZZER_GPIO, s_active_low ? LOW : HIGH);
    // Arduino 的 pinMode 不报错
    return APP_OK;
}

#endif

// 有源后端
#if BSP_BUZZER_ACTIVE

static const char *const BUZZER_BACKEND_NAME = "有源";

// 把硬件准备好
static app_err_t buzzer_hw_init(void) {
    return buzzer_apply_idle();
}

// 无源后端
#else

static const char *const BUZZER_BACKEND_NAME = "无源(LEDC 2kHz)";

// 定时器必须错开
#define BUZZER_LEDC_TIMER LEDC_TIMER_1

// 通道必须错开
#define BUZZER_LEDC_CH LEDC_CHANNEL_1

// 10bit 够用
#define BUZZER_LEDC_RES 10

// 占空比越大越响
#define BUZZER_DUTY_ON_MAX ((uint32_t)BSP_BUZZER_DUTY_ON * ((1u << BUZZER_LEDC_RES) - 1u) / 100u)

// 失败降级 GPIO
static bool s_gpio_fallback;

// 把硬件准备好
static app_err_t buzzer_hw_init(void) {
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = LEDC_LOW_SPEED_MODE;
    tcfg.duty_resolution = (ledc_timer_bit_t)BUZZER_LEDC_RES;
    tcfg.timer_num       = BUZZER_LEDC_TIMER;
    // 2kHz 报警频段
    tcfg.freq_hz         = BSP_BUZZER_FREQ_HZ;
    tcfg.clk_cfg         = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        LOGW(TAG,
             "LEDC_TIMER_%d(%dHz) 申请失败: %s —— 降级为 GPIO 电平驱动"
             "（裸蜂鸣器只会「哒」一声，不会连续响）",
             (int)BUZZER_LEDC_TIMER, (int)BSP_BUZZER_FREQ_HZ, esp_err_to_name(err));
        s_gpio_fallback = true;
        return bsp_gpio_output_init(BSP_BUZZER_GPIO, 0, BSP_BUZZER_ACTIVE_LOW);
    }

    ledc_channel_config_t ccfg = {};
    ccfg.gpio_num   = BSP_BUZZER_GPIO;
    ccfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ccfg.channel    = BUZZER_LEDC_CH;
    ccfg.intr_type  = LEDC_INTR_DISABLE;
    ccfg.timer_sel  = BUZZER_LEDC_TIMER;
    // 上电静音
    ccfg.duty   = 0;
    ccfg.hpoint = 0;

    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        LOGW(TAG, "LEDC CH%d 配置失败: %s —— 降级为 GPIO 电平驱动", (int)BUZZER_LEDC_CH,
             esp_err_to_name(err));
        s_gpio_fallback = true;
        return bsp_gpio_output_init(BSP_BUZZER_GPIO, 0, BSP_BUZZER_ACTIVE_LOW);
    }

    return APP_OK;
}

// 写响停
static app_err_t buzzer_hw_write(bool on) {
    if (s_gpio_fallback) {
        // 降级只会哒一声
        bsp_gpio_write(BSP_BUZZER_GPIO, on);
        return APP_OK;
    }

    const uint32_t duty = on ? BUZZER_DUTY_ON_MAX : 0u;

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CH, duty);
    if (err != ESP_OK) {
        LOGW(TAG, "ledc_set_duty 失败: %s（duty=%u）", esp_err_to_name(err), (unsigned)duty);
        return APP_ERR_FAIL;
    }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CH);
    if (err != ESP_OK) {
        LOGW(TAG, "ledc_update_duty 失败: %s", esp_err_to_name(err));
        return APP_ERR_FAIL;
    }
    return APP_OK;
}

// 后端分叉收尾
#endif

// 初始化并静音
app_err_t bsp_buzzer_init(void) {
    if (s_inited) {
        // 幂等
        return APP_OK;
    }

    s_on            = false;
    s_hw_on         = false;
    s_beep_until_ms = 0;

    const app_err_t err = buzzer_hw_init();
    if (err != APP_OK) {
        LOGW(TAG, "init failed: GPIO%d, %s 蜂鸣器（%s）—— 后续 bsp_buzzer_set() 只更新软件状态",
             BSP_BUZZER_GPIO, BUZZER_BACKEND_NAME, app_err_text(err));
        // 失败不 abort
        return APP_ERR_FAIL;
    }

    s_inited = true;
    LOGI(TAG, "init ok: GPIO%d, %s 蜂鸣器", BSP_BUZZER_GPIO, BUZZER_BACKEND_NAME);
    return APP_OK;
}

// 两级幂等
app_err_t bsp_buzzer_set(bool on) {
    if (on == s_on) {
        // 没变就不碰硬件
        return APP_OK;
    }

    // 先记软件状态
    s_on = on;

    if (!s_inited) {
        // 不可用算成功
        return APP_OK;
    }

    if (on == s_hw_on) {
        // 硬件已是此状态
        return APP_OK;
    }

    const app_err_t err = buzzer_hw_write(on);
    if (err == APP_OK) {
        s_hw_on = on;
    } else {
        LOGW(TAG, "写蜂鸣器失败（on=%d）", (int)on);
    }
    return err;
}

// 查是否在响
bool bsp_buzzer_is_on(void) {
    return s_on;
}

// 短鸣不阻塞
app_err_t bsp_buzzer_beep(int ms) {
    if (ms <= 0) {
        // 参数非法直接退
        return APP_ERR_INVALID_ARG;
    }

    // 时长看 tick
    s_beep_until_ms = app_uptime_ms() + (int64_t)ms;

    // 会打断报警声
    const app_err_t err = bsp_buzzer_set(true);

    LOGD(TAG, "beep %dms", ms);
    return err;
}

// 轮询短鸣到期
void bsp_buzzer_tick(void) {
    if (s_beep_until_ms == 0) {
        // 没有短鸣
        return;
    }

    if (app_uptime_ms() < s_beep_until_ms) {
        // 还没到点
        return;
    }

    s_beep_until_ms = 0;
    (void)bsp_buzzer_set(false);
}

// 初始化成功了吗
bool bsp_buzzer_ready(void) {
    return s_inited;
}

// 切极性并静音
app_err_t bsp_buzzer_set_polarity_low(bool active_low) {
    if (s_active_low == active_low && s_inited) {
        // 没变不动硬件
        return APP_OK;
    }

    s_active_low = active_low;
    // 复位成停
    s_on            = false;
    s_hw_on         = false;
    s_beep_until_ms = 0;

    const app_err_t err = buzzer_apply_idle();
    if (err != APP_OK) {
        LOGW(TAG, "切换极性后重配引脚失败: %s", app_err_text(err));
        return err;
    }

    LOGI(TAG, "触发极性已切换为【%s】并静音",
         active_low ? "低电平触发（开漏高阻）" : "高电平触发（推挽低电平）");
    return APP_OK;
}

// 读当前极性
bool bsp_buzzer_is_polarity_low(void) {
    return s_active_low;
}

// 极性自检 4 段
app_err_t bsp_buzzer_polarity_test(void) {
#if !BSP_BUZZER_ACTIVE
    LOGW(TAG, "极性自检只对【有源】蜂鸣器有意义（无源是 LEDC 方波驱动，没有触发极性这回事）");
    return APP_ERR_NOT_SUPPORTED;
#else
    if (!s_inited) {
        LOGW(TAG, "极性自检跳过：蜂鸣器没有初始化成功");
        return APP_ERR_INVALID_STATE;
    }

    // 每段 1.5 秒
    const int SEG = 1500;

    LOGI(TAG, "======== 蜂鸣器极性自检开始（约 6 秒，请听声音）========");
    LOGI(TAG, "  接下来 4 段，每段 1.5 秒，请记住【哪一段是安静的】：");

    // 这 6 秒会把主循环卡住
    // 自检是人工触发的，保留阻塞
    LOGI(TAG, "  段 1/4：低电平触发 + 静音   ← 如果这段安静，你要的就是它");
    (void)bsp_buzzer_set_polarity_low(true);
    delay(SEG);

    LOGI(TAG, "  段 2/4：高电平触发 + 静音   ← 如果这段安静，你要的是它");
    (void)bsp_buzzer_set_polarity_low(false);
    delay(SEG);

    LOGI(TAG, "  段 3/4：低电平触发 + 吹响（对照）");
    (void)bsp_buzzer_set_polarity_low(true);
    (void)bsp_buzzer_set(true);
    delay(SEG);

    LOGI(TAG, "  段 4/4：高电平触发 + 吹响（对照）");
    (void)bsp_buzzer_set_polarity_low(false);
    (void)bsp_buzzer_set(true);
    delay(SEG);

    // 恢复默认极性
    (void)bsp_buzzer_set_polarity_low(BSP_BUZZER_ACTIVE_LOW != 0);
    (void)bsp_buzzer_set(false);

    LOGI(TAG, "======== 自检结束（已恢复默认极性并静音）========");
    LOGI(TAG, "  请按你听到的结果执行：");
    LOGI(TAG, "    · 段 1 安静  → 串口敲： cfg buzzer_active_low 1");
    LOGI(TAG, "    · 段 2 安静  → 串口敲： cfg buzzer_active_low 0");
    LOGI(TAG, "    · 两段都响   → 信号线可能接到了 3V3，或模块通电自响：拔掉信号线再听");
    LOGI(TAG, "    · 两段都安静 → 蜂鸣器可能没接上，或模块需要更大的驱动电流");
    return APP_OK;
#endif
}
