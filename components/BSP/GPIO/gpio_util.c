// c
// GPIO 读写与状态灯

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "soc/soc_caps.h"

#include "board_config.h"
#include "gpio_util.h"

static const char *TAG = "GPIO";

// 下标是 GPIO 号
typedef struct {
    bool cfg_out;
    // 低电平有效
    bool active_low;
    // 只告警一次
    bool warned;
} gpio_out_ctx_t;

static gpio_out_ctx_t s_out[GPIO_NUM_MAX];

static bool gpio_pin_ok(int gpio) {
    // 先挡范围防移位越界
    return (gpio >= 0) && (gpio < GPIO_NUM_MAX) && GPIO_IS_VALID_GPIO(gpio);
}

esp_err_t bsp_gpio_output_init(int gpio, int init_level, bool active_low) {
    if (!gpio_pin_ok(gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        ESP_LOGE(TAG, "GPIO%d 不能做输出（不存在或不可输出）", gpio);
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d, 输出) failed: %s", gpio, esp_err_to_name(err));
        return err;
    }

    // 先登记再设电平
    s_out[gpio].cfg_out    = true;
    s_out[gpio].active_low = active_low;
    s_out[gpio].warned     = false;

    // -1 则不设电平
    if (init_level >= 0) {
        bsp_gpio_write(gpio, init_level != 0);
        ESP_LOGI(TAG, "GPIO%d 输出已配 (active_%s, 初始逻辑=%d)", gpio, active_low ? "low" : "high",
                 init_level ? 1 : 0);
    } else {
        ESP_LOGI(TAG, "GPIO%d 输出已配 (active_%s, 初始电平不改)", gpio, active_low ? "low" : "high");
    }
    return ESP_OK;
}

esp_err_t bsp_gpio_input_init(int gpio, bool pull_up) {
    if (!gpio_pin_ok(gpio)) {
        ESP_LOGE(TAG, "GPIO%d 不存在，不能做输入", gpio);
        return ESP_ERR_INVALID_ARG;
    }

    // 按键上拉，按下读 0
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d, 输入) failed: %s", gpio, esp_err_to_name(err));
        return err;
    }

    // 清掉输出标记
    s_out[gpio].cfg_out = false;

    ESP_LOGI(TAG, "GPIO%d 输入已配 (%s)", gpio, pull_up ? "内部上拉" : "内部下拉");
    return ESP_OK;
}

void bsp_gpio_write(int gpio, bool on) {
    if (!gpio_pin_ok(gpio)) {
        ESP_LOGE(TAG, "bsp_gpio_write: GPIO%d 不存在", gpio);
        return;
    }

    if (!s_out[gpio].cfg_out) {
        // 只告警一次
        if (!s_out[gpio].warned) {
            ESP_LOGW(TAG, "GPIO%d 未用 bsp_gpio_output_init() 配置过，本次写入被忽略", gpio);
            s_out[gpio].warned = true;
        }
        return;
    }

    // 异或统一取反
    const uint32_t level = (on != s_out[gpio].active_low) ? 1u : 0u;

    const esp_err_t err = gpio_set_level((gpio_num_t)gpio, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level(GPIO%d,%u) failed: %s", gpio, (unsigned)level, esp_err_to_name(err));
    }
}

// 读原始电平
int bsp_gpio_read(int gpio) {
    if (!gpio_pin_ok(gpio)) {
        ESP_LOGE(TAG, "bsp_gpio_read: GPIO%d 不存在", gpio);
        return -1;
    }

    return (gpio_get_level((gpio_num_t)gpio) != 0) ? 1 : 0;
}

// 板载状态灯
#if BSP_STATUS_LED_ENABLE

#define BSP_LED_RMT_RES_HZ 10000000u
#define BSP_LED_T0H_TICKS 3
#define BSP_LED_T0L_TICKS 9
#define BSP_LED_T1H_TICKS 9
#define BSP_LED_T1L_TICKS 3

// 复位低电平 60us
#define BSP_LED_RESET_US 60

#define BSP_LED_RMT_MEM_SYMBOLS SOC_RMT_MEM_WORDS_PER_CHANNEL

// 等发完上限 50ms
#define BSP_LED_RMT_WAIT_MS 50

// 抢锁最多等 50ms
#define BSP_LED_LOCK_WAIT_MS 50

// 亮度上限 40
#define BSP_LED_MAX_BRIGHTNESS 40

static rmt_channel_handle_t s_led_chan    = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static bool s_led_inited = false;
static bool s_led_ready  = false;
static bool s_led_tx_warned = false;

// 多任务会抢状态灯
static StaticSemaphore_t s_led_mutex_buf;
static SemaphoreHandle_t s_led_mutex = NULL;

static esp_err_t status_led_send(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led_ready || s_led_chan == NULL || s_led_encoder == NULL || s_led_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(BSP_LED_LOCK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "状态灯忙，本次颜色更新被跳过");
        return ESP_ERR_TIMEOUT;
    }

    // 线上顺序是 GRB
    const uint8_t grb[3] = {g, r, b};
    // 发完锁低
    const rmt_transmit_config_t tx_cfg = {
        .loop_count      = 0,
        .flags.eot_level = 0,
    };

    esp_err_t err = rmt_transmit(s_led_chan, s_led_encoder, grb, sizeof(grb), &tx_cfg);
    if (err == ESP_OK) {
        // 栈变量须等搬完
        err = rmt_tx_wait_all_done(s_led_chan, BSP_LED_RMT_WAIT_MS);
    }

    xSemaphoreGive(s_led_mutex);

    if (err != ESP_OK) {
        if (!s_led_tx_warned) {
            ESP_LOGW(TAG, "状态灯发送失败(%s)，可能灯珠没焊/引脚不对，不影响开机", esp_err_to_name(err));
            s_led_tx_warned = true;
        }
        return err;
    }

    esp_rom_delay_us(BSP_LED_RESET_US);
    return ESP_OK;
}

// 失败也返回成功
esp_err_t bsp_status_led_init(void) {
    if (s_led_inited) {
        return ESP_OK;
    }
    s_led_inited = true;

    if (s_led_mutex == NULL) {
        s_led_mutex = xSemaphoreCreateMutexStatic(&s_led_mutex_buf);
        if (s_led_mutex == NULL) {
            ESP_LOGW(TAG, "状态灯互斥锁创建失败，状态灯不可用");
            return ESP_OK;
        }
    }

    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = BSP_STATUS_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = BSP_LED_RMT_RES_HZ,
        .mem_block_symbols = BSP_LED_RMT_MEM_SYMBOLS,
        .trans_queue_depth = 2,
        .intr_priority = 0,
        // 防上电闪白
        .flags.init_level = 0,
    };

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "状态灯 RMT 通道申请失败(GPIO%d): %s —— 不装也不要紧", (int)BSP_STATUS_LED_GPIO,
                 esp_err_to_name(err));
        s_led_chan = NULL;
        return ESP_OK;
    }

    const rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 =
            {
                   .level0    = 1,
                   .duration0 = BSP_LED_T0H_TICKS,
                   .level1    = 0,
                   .duration1 = BSP_LED_T0L_TICKS,
                   },
        .bit1 =
            {
                   .level0    = 1,
                   .duration0 = BSP_LED_T1H_TICKS,
                   .level1    = 0,
                   .duration1 = BSP_LED_T1L_TICKS,
                   },
        // 高位先发
        .flags.msb_first = 1,
    };

    err = rmt_new_bytes_encoder(&bytes_cfg, &s_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "状态灯编码器创建失败: %s —— 不装也不要紧", esp_err_to_name(err));
        (void)rmt_del_channel(s_led_chan);
        s_led_chan    = NULL;
        s_led_encoder = NULL;
        return ESP_OK;
    }

    err = rmt_enable(s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_enable 失败: %s —— 不装也不要紧", esp_err_to_name(err));
        (void)rmt_del_encoder(s_led_encoder);
        (void)rmt_del_channel(s_led_chan);
        s_led_chan    = NULL;
        s_led_encoder = NULL;
        return ESP_OK;
    }

    s_led_ready = true;
    ESP_LOGI(TAG,
             "状态灯 WS2812 就绪: GPIO%d, RMT %dHz(1tick=0.1us), "
             "T0H/T0L/T1H/T1L=%d/%d/%d/%d tick, 复位=%dus",
             (int)BSP_STATUS_LED_GPIO, (int)BSP_LED_RMT_RES_HZ, BSP_LED_T0H_TICKS, BSP_LED_T0L_TICKS,
             BSP_LED_T1H_TICKS, BSP_LED_T1L_TICKS, BSP_LED_RESET_US);

    // 上电先发全黑
    (void)status_led_send(0, 0, 0);
    return ESP_OK;
}

void bsp_status_led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led_ready) {
        // 没就绪就忽略
        return;
    }

    if (r > BSP_LED_MAX_BRIGHTNESS) {
        r = BSP_LED_MAX_BRIGHTNESS;
    }
    if (g > BSP_LED_MAX_BRIGHTNESS) {
        g = BSP_LED_MAX_BRIGHTNESS;
    }
    if (b > BSP_LED_MAX_BRIGHTNESS) {
        b = BSP_LED_MAX_BRIGHTNESS;
    }

    (void)status_led_send(r, g, b);
}

void bsp_status_led_off(void) {
    bsp_status_led_set(0, 0, 0);
}

// 按状态码设色
void bsp_status_led_show(int code) {
    switch (code) {
    case BSP_STATUS_BOOTING:
        // 蓝：启动中
        bsp_status_led_set(0, 0, 40);
        break;
    case BSP_STATUS_NO_WIFI:
        // 黄：无 WiFi
        bsp_status_led_set(40, 40, 0);
        break;
    case BSP_STATUS_ONLINE:
        // 绿：联网正常
        bsp_status_led_set(0, 40, 0);
        break;
    case BSP_STATUS_ALERT:
        // 红：高温报警
        bsp_status_led_set(40, 0, 0);
        break;
    case BSP_STATUS_AP_MODE:
        // 紫：配网预留
        bsp_status_led_set(30, 0, 40);
        break;
    default:
        ESP_LOGW(TAG, "未知状态灯编码 %d，按熄灭处理", code);
        bsp_status_led_set(0, 0, 0);
        break;
    }
}

// 关掉时的空实现
#else

static bool s_led_inited = false;

esp_err_t bsp_status_led_init(void) {
    if (!s_led_inited) {
        s_led_inited = true;
        ESP_LOGI(TAG, "状态灯已被 BSP_STATUS_LED_ENABLE=0 关闭，跳过初始化");
    }
    return ESP_OK;
}

void bsp_status_led_set(uint8_t r, uint8_t g, uint8_t b) {
    (void)r;
    (void)g;
    (void)b;
}

void bsp_status_led_off(void) {
}

void bsp_status_led_show(int code) {
    (void)code;
}

// 状态灯开关收尾
#endif
