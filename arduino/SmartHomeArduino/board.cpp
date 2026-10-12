// c
// 上电初始化与自检

#include <string.h>

#include "board.h"
#include "bsp_config.h"
#include "app_config.h"

#include "gpio_util.h"
#include "i2c_bus.h"

#include "led.h"
#include "servo.h"
#include "buzzer.h"
#include "sensor.h"
#include "key.h"
#include "voice.h"

#include "panel.h"

static const char *TAG = "BOARD";

// 自检表上界
#define BOARD_STEP_MAX 12

static_assert(BOARD_STEP_MAX >= 10, "BOARD_STEP_MAX 必须容纳 board_init() 的全部步骤");

// 补状态灯字面量
#define BOARD_MOD_STATUS_LED "status_led"

// 自检表的一项
typedef struct {
    const char *name;
    app_err_t   err;
    // 跳过不算失败
    bool skipped;
} board_mod_t;

// 自检结果表
static board_mod_t s_mod[BOARD_STEP_MAX];
// 有效项数
static int s_mod_cnt = 0;
// 幂等标志
static bool s_inited = false;

// 记一步结果
static void board_step(const char *name, app_err_t err) {
    if (s_mod_cnt >= BOARD_STEP_MAX) {
        // 表满属代码问题
        LOGW(TAG, "自检表已满(%d)，\"%s\" 的结果没能记录", BOARD_STEP_MAX, name);
        return;
    }

    s_mod[s_mod_cnt].name    = name;
    s_mod[s_mod_cnt].err     = err;
    s_mod[s_mod_cnt].skipped = false;
    s_mod_cnt++;

    if (err == APP_OK) {
        LOGI(TAG, "[ OK ] %s", name);
    } else {
        LOGW(TAG, "[FAIL] %s (%s)", name, app_err_text(err));
    }
}

// 记一步跳过
static void board_skip(const char *name, const char *reason) {
    if (s_mod_cnt >= BOARD_STEP_MAX) {
        return;
    }

    s_mod[s_mod_cnt].name    = name;
    s_mod[s_mod_cnt].err     = APP_OK;
    s_mod[s_mod_cnt].skipped = true;
    s_mod_cnt++;

    LOGI(TAG, "[SKIP] %s (%s)", name, reason);
}

// 打印引脚表
void board_dump_pinmap(void) {
    LOGI(TAG, "=========== 引脚分配表（唯一真源 bsp_config.h） ===========");
    LOGI(TAG, "  I2C      : SDA=GPIO%d SCL=GPIO%d @%dHz  → 温湿度 SHT30/AHT20 + 光照 BH1750", BSP_I2C_SDA_GPIO,
         BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ);
    LOGI(TAG, "  ADC1     : 光照=GPIO%d 雨滴AO=GPIO%d (ESP32-S3 只有 ADC1 能用)", BSP_GPIO_LIGHT_ADC,
         BSP_GPIO_RAIN_AO);
    LOGI(TAG, "             光照方向宏 BSP_LIGHT_ADC_INVERT=%d (遮住光敏后 light_pct 应变小，反了就改它)",
         BSP_LIGHT_ADC_INVERT);
    LOGI(TAG, "  雨滴DO   : GPIO%d (可选，不接也行)", BSP_GPIO_RAIN_DO);
    LOGI(TAG,
         "  4 路灯   : 客厅=GPIO%d 厨房=GPIO%d 卧室=GPIO%d 浴室=GPIO%d (LEDC %dHz, active_low=%d)",
         BSP_LED_GPIO_LIVING, BSP_LED_GPIO_KITCHEN, BSP_LED_GPIO_BEDROOM, BSP_LED_GPIO_BATH,
         BSP_LED_PWM_FREQ_HZ, BSP_LED_ACTIVE_LOW);
    LOGI(TAG, "  门舵机 x%d: 信号=GPIO%d (%dHz, 脉宽 %d~%dus) —— 建议独立 5V 供电并与板子共地",
         BSP_SERVO_COUNT, BSP_SERVO_GPIO_DOOR, BSP_SERVO_FREQ_HZ, BSP_SERVO_MIN_PULSE_US,
         BSP_SERVO_MAX_PULSE_US);
    LOGI(TAG, "  蜂鸣器   : GPIO%d (%s, active_low=%d)", BSP_BUZZER_GPIO, BSP_BUZZER_ACTIVE ? "有源" : "无源",
         BSP_BUZZER_ACTIVE_LOW);
    LOGI(TAG, "  显示屏   : %s (I2C) SDA=GPIO%d SCL=GPIO%d @%dHz, 地址=0x%02X", BSP_PANEL_NAME, BSP_I2C_SDA_GPIO,
         BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ, BSP_I2C_ADDR_SSD1306);
#if APP_USE_KEY
    LOGI(TAG, "  按键 x%d  : KEY1=GPIO%d KEY2=GPIO%d (按下为低, 消抖 %dms, 长按 %dms)", BSP_KEY_COUNT,
         BSP_KEY_GPIO_KEY1, BSP_KEY_GPIO_KEY2, BSP_KEY_DEBOUNCE_MS, BSP_KEY_LONG_PRESS_MS);
#else
    LOGI(TAG, "  按键     : 【无按键板】GPIO%d / GPIO%d 未使用", BSP_KEY_GPIO_KEY1, BSP_KEY_GPIO_KEY2);
    LOGI(TAG, "             控制入口 = 手机 App(蓝牙) + 网页面板(MQTT) + 串口调试台");
#endif
    LOGI(TAG, "  状态灯   : GPIO%d (板载 WS2812, enable=%d)", BSP_STATUS_LED_GPIO, BSP_STATUS_LED_ENABLE);
    LOGI(TAG, "==========================================================");
}

// 按顺序初始化
// 幂等
app_err_t board_init(void) {
    if (s_inited) {
        LOGI(TAG, "board already initialized, skip");
        return APP_OK;
    }

    s_mod_cnt = 0;

    LOGI(TAG, "================= board init begin =================");

    // 1. 状态灯最早
    board_step(BOARD_MOD_STATUS_LED, bsp_status_led_init());
    bsp_status_led_show(BSP_STATUS_BOOTING);

    // 2. I2C 总线
    const app_err_t i2c_err = bsp_i2c_init();
    board_step(BOARD_MOD_I2C, i2c_err);
    if (i2c_err == APP_OK) {
        // 扫一遍并缓存
        uint8_t   found[BSP_I2C_SCAN_MAX];
        const int n = bsp_i2c_scan(found, (int)sizeof(found));

        for (int i = 0; i < n && i < (int)(sizeof(found) / sizeof(found[0])); i++) {
            LOGI(TAG, "  在线设备[%d] = 0x%02X", i, (unsigned)found[i]);
        }
    } else {
        LOGW(TAG, "I2C 总线没建起来(%s)，跳过扫描；温湿度/光照会缺失，其它功能照常", app_err_text(i2c_err));
    }

    // 3. 4 路灯
    board_step(BOARD_MOD_LED, bsp_led_init());

    // 4. 门舵机
    board_step(BOARD_MOD_SERVO, bsp_servo_init());

    // 5. 蜂鸣器
    board_step(BOARD_MOD_BUZZER, bsp_buzzer_init());

    // 6. 传感器
    board_step(BOARD_MOD_SENSOR, bsp_sensors_init());

    // 7. 屏幕最慢
#if APP_USE_PANEL
    board_step(BOARD_MOD_PANEL, panel_init());
#else
    board_skip(BOARD_MOD_PANEL, "APP_USE_PANEL=0");
#endif

    // 8. 按键
#if APP_USE_KEY
    board_step(BOARD_MOD_KEY, bsp_key_init());
#else
    board_skip(BOARD_MOD_KEY, "无按键板（APP_USE_KEY=0）");
#endif

    // 9. 语音通道
    board_step(BOARD_MOD_VOICE, bsp_voice_init());

    s_inited = true;

    board_dump_summary();

    // 状态灯留在开机中
    LOGI(TAG, "================= board init end =================");
    return APP_OK;
}

// 打印自检汇总
void board_dump_summary(void) {
    int ok_cnt   = 0;
    int fail_cnt = 0;
    int skip_cnt = 0;

    LOGI(TAG, "---------------- 模块初始化结果 ----------------");
    for (int i = 0; i < s_mod_cnt; i++) {
        if (s_mod[i].skipped) {
            skip_cnt++;
            LOGI(TAG, "  [SKIP] %s", s_mod[i].name);
        } else if (s_mod[i].err == APP_OK) {
            ok_cnt++;
            LOGI(TAG, "  [ OK ] %s", s_mod[i].name);
        } else {
            fail_cnt++;
            LOGW(TAG, "  [FAIL] %-8s %s", s_mod[i].name, app_err_text(s_mod[i].err));
        }
    }
    LOGI(TAG, "-----------------------------------------------");

    if (fail_cnt == 0 && skip_cnt == 0) {
        LOGI(TAG, "board init ok: 全部 %d 个模块正常", s_mod_cnt);
        return;
    }

    if (fail_cnt == 0) {
        LOGI(TAG, "board init ok: %d 个模块正常, %d 个按配置跳过（功能少了但没坏）", ok_cnt, skip_cnt);
        return;
    }

    // 只用 WARN
    LOGW(TAG, "board init 完成: %d/%d 个模块正常, 失败清单如下:", ok_cnt, s_mod_cnt);
    for (int i = 0; i < s_mod_cnt; i++) {
        if (!s_mod[i].skipped && s_mod[i].err != APP_OK) {
            LOGW(TAG, "  - %s (%s)", s_mod[i].name, app_err_text(s_mod[i].err));
        }
    }
    LOGW(TAG, "配件没到 / 没插好 / 接错线都会这样，其它功能照常运行，不影响开机");
}

// 查某步是否 OK
// 跳过的算不可用
bool board_module_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (int i = 0; i < s_mod_cnt; i++) {
        if (strcmp(s_mod[i].name, name) == 0) {
            return (s_mod[i].err == APP_OK) && !s_mod[i].skipped;
        }
    }

    LOGD(TAG, "board_module_ok: 未知模块名 \"%s\"", name);
    return false;
}
