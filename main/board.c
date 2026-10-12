// c
// 上电初始化与自检
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

#include "board.h"
// 引脚唯一真源
#include "board_config.h"
#include "gpio_util.h"
#include "i2c_bus.h"
#include "adc_bus.h"

#include "led.h"
#include "servo.h"
#include "buzzer.h"
#include "key.h"
#include "sensor.h"
#include "voice.h"
#include "panel.h"

static const char *TAG = "BOARD";

// 自检表上界
#define BOARD_STEP_MAX 12

_Static_assert(BOARD_STEP_MAX >= 11, "BOARD_STEP_MAX 必须容纳 board_init() 的 11 个步骤");

// 补状态灯字面量
#define BOARD_MOD_STATUS_LED "status_led"

// 自检表的一项
typedef struct {
    const char *name;
    esp_err_t err;
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
static void board_step(const char *name, esp_err_t err) {
    if (s_mod_cnt >= BOARD_STEP_MAX) {
        // 表满属代码问题
        ESP_LOGW(TAG, "自检表已满(%d)，\"%s\" 的结果没能记录", BOARD_STEP_MAX, name);
        return;
    }

    s_mod[s_mod_cnt].name    = name;
    s_mod[s_mod_cnt].err     = err;
    s_mod[s_mod_cnt].skipped = false;
    s_mod_cnt++;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[ OK ] %s", name);
    } else {
        ESP_LOGW(TAG, "[FAIL] %s (%s)", name, esp_err_to_name(err));
    }
}

// 记一步跳过
static void __attribute__((unused)) board_skip(const char *name, const char *reason) {
    if (s_mod_cnt >= BOARD_STEP_MAX) {
        return;
    }

    s_mod[s_mod_cnt].name    = name;
    s_mod[s_mod_cnt].err     = ESP_OK;
    s_mod[s_mod_cnt].skipped = true;
    s_mod_cnt++;

    ESP_LOGI(TAG, "[SKIP] %s (%s)", name, reason);
}

// 打印引脚表
void board_dump_pinmap(void) {
#if CONFIG_APP_BOARD_BRINGUP_LOG
    ESP_LOGI(TAG, "=========== 引脚分配表（唯一真源 board_config.h） ===========");
    ESP_LOGI(TAG, "  I2C%d    : SDA=GPIO%d SCL=GPIO%d @%dHz  → 温湿度 SHT30/AHT20 + 光照 BH1750",
             (int)BSP_I2C_PORT, (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO, (int)BSP_I2C_FREQ_HZ);
    ESP_LOGI(TAG, "  ADC1     : 光照=GPIO%d 雨滴AO=GPIO%d (通道由 IDF 按 GPIO 换算)", (int)BSP_GPIO_LIGHT_ADC,
             (int)BSP_GPIO_RAIN_AO);
    ESP_LOGI(TAG,
             "             光照方向宏 BSP_LIGHT_ADC_INVERT=%d "
             "(遮住光敏后 light_pct 应变小，反了就改它)",
             (int)BSP_LIGHT_ADC_INVERT);
    ESP_LOGI(TAG, "  雨滴DO   : GPIO%d (可选，不接也行)", (int)BSP_GPIO_RAIN_DO);
    ESP_LOGI(TAG,
             "  4 路灯   : 客厅=GPIO%d 厨房=GPIO%d 卧室=GPIO%d 浴室=GPIO%d "
             "(LEDC %dHz, active_low=%d)",
             (int)BSP_LED_GPIO_LIVING, (int)BSP_LED_GPIO_KITCHEN, (int)BSP_LED_GPIO_BEDROOM,
             (int)BSP_LED_GPIO_BATH, (int)BSP_LED_PWM_FREQ_HZ, (int)BSP_LED_ACTIVE_LOW);
    ESP_LOGI(TAG, "  门舵机 x%d: 信号=GPIO%d (%dHz, 脉宽 %d~%dus) —— 建议独立 5V 供电并与板子共地",
             BSP_SERVO_COUNT, (int)BSP_SERVO_GPIO_DOOR, (int)BSP_SERVO_FREQ_HZ, BSP_SERVO_MIN_PULSE_US,
             BSP_SERVO_MAX_PULSE_US);
    ESP_LOGI(TAG, "  蜂鸣器   : GPIO%d (%s, active_low=%d)", (int)BSP_BUZZER_GPIO,
             BSP_BUZZER_ACTIVE ? "有源" : "无源", (int)BSP_BUZZER_ACTIVE_LOW);
    ESP_LOGI(TAG, "  显示屏   : %s (I2C0) SDA=GPIO%d SCL=GPIO%d @%dHz, 地址=0x%02X", BSP_PANEL_NAME,
             (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO, (int)BSP_I2C_FREQ_HZ, BSP_I2C_ADDR_SSD1306);
#if CONFIG_APP_KEY_ENABLE
    ESP_LOGI(TAG, "  按键 x%d  : KEY1=GPIO%d KEY2=GPIO%d (按下为低, 消抖 %dms, 长按 %dms)", BSP_KEY_COUNT,
             (int)BSP_KEY_GPIO_KEY1, (int)BSP_KEY_GPIO_KEY2, BSP_KEY_DEBOUNCE_MS, BSP_KEY_LONG_PRESS_MS);
#else
    ESP_LOGI(TAG, "  按键     : 【无按键板】GPIO0 / GPIO15 未使用");
    ESP_LOGI(TAG, "             控制入口 = 语音指令 + 手机 MQTT 页面 + 串口调试台");
#endif
    ESP_LOGI(TAG, "  语音输入 : INMP441(I²S%d) SCK=GPIO%d WS=GPIO%d SD=GPIO%d @%dHz + 板载 ESP-SR",
             (int)BSP_I2S_MIC_PORT, (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO,
             (int)BSP_I2S_MIC_SD_GPIO, BSP_I2S_MIC_SAMPLE_RATE);
    ESP_LOGI(TAG, "             L/R 接 GND（左声道）；模型在 flash 的 model 分区，随 idf.py flash 一起烧");
    ESP_LOGI(TAG, "  状态灯   : GPIO%d (板载 WS2812, RMT 驱动, enable=%d)", (int)BSP_STATUS_LED_GPIO,
             (int)BSP_STATUS_LED_ENABLE);
    ESP_LOGI(TAG, "==========================================================");
#endif
}

// 按顺序初始化
// 幂等
esp_err_t board_init(void) {
    if (s_inited) {
        ESP_LOGI(TAG, "board already initialized, skip");
        return ESP_OK;
    }

    s_mod_cnt = 0;

    ESP_LOGI(TAG, "================= board init begin =================");

    // 1. 状态灯最早
    board_step(BOARD_MOD_STATUS_LED, bsp_status_led_init());
    bsp_status_led_show(BSP_STATUS_BOOTING);

    // 2. I2C 总线
    const esp_err_t i2c_err = bsp_i2c_init();
    board_step(BOARD_MOD_I2C, i2c_err);
    if (i2c_err == ESP_OK) {
        // 扫一遍并缓存
        uint8_t   found[BSP_I2C_SCAN_MAX];
        const int n = bsp_i2c_scan(found, sizeof(found));

        for (int i = 0; i < n && i < (int)(sizeof(found) / sizeof(found[0])); i++) {
            ESP_LOGI(TAG, "  在线设备[%d] = 0x%02X", i, (unsigned)found[i]);
        }
    } else {
        ESP_LOGW(TAG, "I2C 总线没建起来(%s)，跳过扫描；温湿度/光照会缺失，其它功能照常",
                 esp_err_to_name(i2c_err));
    }

    // 3. ADC1
    board_step(BOARD_MOD_ADC, bsp_adc_init());

    // 4. 4 路灯
    board_step(BOARD_MOD_LED, bsp_led_init());

    // 5. 门舵机
    board_step(BOARD_MOD_SERVO, bsp_servo_init());

    // 6. 蜂鸣器
    board_step(BOARD_MOD_BUZZER, bsp_buzzer_init());

    // 7. 传感器
    board_step(BOARD_MOD_SENSOR, bsp_sensors_init());

    // 8. 屏幕最慢
#if CONFIG_APP_PANEL_ENABLE
    board_step(BOARD_MOD_PANEL, panel_init());
#else
    board_skip(BOARD_MOD_PANEL, "CONFIG_APP_PANEL_ENABLE=n");
#endif

    // 9. 按键
#if CONFIG_APP_KEY_ENABLE
    board_step(BOARD_MOD_KEY, bsp_key_init());
#else
    board_skip(BOARD_MOD_KEY, "无按键板（CONFIG_APP_KEY_ENABLE=n）");
#endif
    // 按键开关收尾

    // 10. 语音通道
    board_step(BOARD_MOD_VOICE, bsp_voice_init());

    // 11. 蓝牙延后起
    // 只登记，别在这启动
#if CONFIG_APP_BLE_ENABLE
    board_skip(BOARD_MOD_BLE, "由 main 在联网后启动");
#else
    board_skip(BOARD_MOD_BLE, "CONFIG_APP_BLE_ENABLE=n");
#endif

    s_inited = true;

    board_dump_summary();

    // 状态灯留在开机中
    ESP_LOGI(TAG, "================= board init end =================");
    return ESP_OK;
}

// 打印自检汇总
void board_dump_summary(void) {
#if CONFIG_APP_BOARD_BRINGUP_LOG
    int ok_cnt   = 0;
    int fail_cnt = 0;
    int skip_cnt = 0;

    ESP_LOGI(TAG, "---------------- 模块初始化结果 ----------------");
    for (int i = 0; i < s_mod_cnt; i++) {
        if (s_mod[i].skipped) {
            skip_cnt++;
            ESP_LOGI(TAG, "  [SKIP] %s", s_mod[i].name);
        } else if (s_mod[i].err == ESP_OK) {
            ok_cnt++;
            ESP_LOGI(TAG, "  [ OK ] %s", s_mod[i].name);
        } else {
            fail_cnt++;
            ESP_LOGW(TAG, "  [FAIL] %-8s %s", s_mod[i].name, esp_err_to_name(s_mod[i].err));
        }
    }
    ESP_LOGI(TAG, "-----------------------------------------------");

    if (fail_cnt == 0 && skip_cnt == 0) {
        ESP_LOGI(TAG, "board init ok: 全部 %d 个模块正常", s_mod_cnt);
        return;
    }

    if (fail_cnt == 0) {
        ESP_LOGI(TAG, "board init ok: %d 个模块正常, %d 个按配置跳过（功能少了但没坏）", ok_cnt, skip_cnt);
        return;
    }

    // 只用 WARN
    ESP_LOGW(TAG, "board init 完成: %d/%d 个模块正常, 失败清单如下:", ok_cnt, s_mod_cnt);
    for (int i = 0; i < s_mod_cnt; i++) {
        if (!s_mod[i].skipped && s_mod[i].err != ESP_OK) {
            ESP_LOGW(TAG, "  - %s (%s)", s_mod[i].name, esp_err_to_name(s_mod[i].err));
        }
    }
    ESP_LOGW(TAG, "配件没到 / 没插好 / 接错线都会这样，其它功能照常运行，不影响开机");
#endif
}

// 查某步是否 OK
// 跳过的算不可用
bool board_module_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (int i = 0; i < s_mod_cnt; i++) {
        if (strcmp(s_mod[i].name, name) == 0) {
            return (s_mod[i].err == ESP_OK) && !s_mod[i].skipped;
        }
    }

    ESP_LOGD(TAG, "board_module_ok: 未知模块名 \"%s\"", name);
    return false;
}
