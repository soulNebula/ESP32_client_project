// c
// GPIO 读写与状态灯
#include <Arduino.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#include "app_common.h"
#include "bsp_config.h"
#include "gpio_util.h"

#if BSP_STATUS_LED_ENABLE
// 3.x 只剩新 RMT 驱动
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
// 2.x 用 core 自带的封装
#else
#include "esp32-hal-rmt.h"
#endif
#endif

static const char *TAG = "GPIO";

// 表按最大引脚数开
#define GPIO_TABLE_MAX 64

// 下标即 GPIO 号
typedef struct {
    bool cfg_out;
    // 低电平有效
    bool active_low;
    // 只告警一次
    bool warned;
} gpio_out_ctx_t;

static gpio_out_ctx_t s_out[GPIO_TABLE_MAX];

// 先挡范围防移位越界
static bool gpio_pin_ok(int gpio) {
    return (gpio >= 0) && (gpio < GPIO_TABLE_MAX) && GPIO_IS_VALID_GPIO(gpio);
}

app_err_t bsp_gpio_output_init(int gpio, int init_level, bool active_low) {
    if (!gpio_pin_ok(gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        LOGE(TAG, "GPIO%d 不能做输出（不存在或不可输出）", gpio);
        return APP_ERR_INVALID_ARG;
    }

    pinMode(gpio, OUTPUT);

    // 先登记再设电平
    s_out[gpio].cfg_out    = true;
    s_out[gpio].active_low = active_low;
    s_out[gpio].warned     = false;

    // -1 则不设电平
    if (init_level >= 0) {
        bsp_gpio_write(gpio, init_level != 0);
        LOGI(TAG, "GPIO%d 输出已配 (active_%s, 初始逻辑=%d)", gpio, active_low ? "low" : "high",
             init_level ? 1 : 0);
    } else {
        LOGI(TAG, "GPIO%d 输出已配 (active_%s, 初始电平不改)", gpio, active_low ? "low" : "high");
    }
    return APP_OK;
}

app_err_t bsp_gpio_input_init(int gpio, bool pull_up) {
    if (!gpio_pin_ok(gpio)) {
        LOGE(TAG, "GPIO%d 不存在，不能做输入", gpio);
        return APP_ERR_INVALID_ARG;
    }

    // 按键上拉，按下读 0
    pinMode(gpio, pull_up ? INPUT_PULLUP : INPUT_PULLDOWN);

    // 清掉输出标记
    s_out[gpio].cfg_out = false;

    LOGI(TAG, "GPIO%d 输入已配 (%s)", gpio, pull_up ? "内部上拉" : "内部下拉");
    return APP_OK;
}

void bsp_gpio_write(int gpio, bool on) {
    if (!gpio_pin_ok(gpio)) {
        LOGE(TAG, "bsp_gpio_write: GPIO%d 不存在", gpio);
        return;
    }

    if (!s_out[gpio].cfg_out) {
        // 只告警一次
        if (!s_out[gpio].warned) {
            LOGW(TAG, "GPIO%d 未用 bsp_gpio_output_init() 配置过，本次写入被忽略", gpio);
            s_out[gpio].warned = true;
        }
        return;
    }

    // 异或统一取反
    const uint32_t level = (on != s_out[gpio].active_low) ? 1u : 0u;

    // Arduino 的 digitalWrite 不报错
    digitalWrite(gpio, level);
}

// 读原始电平
int bsp_gpio_read(int gpio) {
    if (!gpio_pin_ok(gpio)) {
        LOGE(TAG, "bsp_gpio_read: GPIO%d 不存在", gpio);
        return -1;
    }

    return (digitalRead(gpio) != 0) ? 1 : 0;
}

// 板载状态灯
#if BSP_STATUS_LED_ENABLE

// 1 tick = 100ns
#define WS_TICK_NS 100.0f
#define WS_RES_HZ 10000000u

// T0H/T0L/T1H/T1L
#define WS_T0H 3
#define WS_T0L 9
#define WS_T1H 9
#define WS_T1L 3

// 复位低电平 60us
#define WS_RESET_US 60

// 一帧 3 字节
#define WS_BYTES 3

// 等发完上限 50ms
#define WS_WAIT_MS 50

// 抢锁最多等 50ms
#define WS_LOCK_WAIT_MS 50

// 很亮，压到 40
#define WS_BRIGHT_MAX BSP_STATUS_LED_BRIGHT

// 2.x 的 RMT 句柄
#if ESP_ARDUINO_VERSION_MAJOR < 3
static rmt_obj_t *s_ws_rmt;
// 3.x 的通道与编码器
#else
static rmt_channel_handle_t s_ws_chan;
static rmt_encoder_handle_t s_ws_enc;
#endif

static bool s_ws_inited;
static bool s_ws_ready;
static bool s_ws_warned;

// 多任务会抢状态灯
static SemaphoreHandle_t s_ws_lock;

// 把硬件准备好
static app_err_t ws_init(void) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num          = (gpio_num_t)BSP_STATUS_LED_GPIO;
    tx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz     = WS_RES_HZ;
    // 一帧 24 位够放
    tx_cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    tx_cfg.trans_queue_depth = 2;
    // 防上电闪白
    tx_cfg.flags.init_level  = 0;

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_ws_chan);
    if (err != ESP_OK) {
        LOGW(TAG, "状态灯 RMT 通道申请失败(GPIO%d): %s —— 不装也不要紧", BSP_STATUS_LED_GPIO,
             esp_err_to_name(err));
        s_ws_chan = NULL;
        return APP_ERR_FAIL;
    }

    rmt_bytes_encoder_config_t bytes_cfg = {};
    bytes_cfg.bit0.level0    = 1;
    bytes_cfg.bit0.duration0 = WS_T0H;
    bytes_cfg.bit0.level1    = 0;
    bytes_cfg.bit0.duration1 = WS_T0L;
    bytes_cfg.bit1.level0    = 1;
    bytes_cfg.bit1.duration0 = WS_T1H;
    bytes_cfg.bit1.level1    = 0;
    bytes_cfg.bit1.duration1 = WS_T1L;
    // 高位先发
    bytes_cfg.flags.msb_first = 1;

    err = rmt_new_bytes_encoder(&bytes_cfg, &s_ws_enc);
    if (err != ESP_OK) {
        LOGW(TAG, "状态灯编码器创建失败: %s —— 不装也不要紧", esp_err_to_name(err));
        (void)rmt_del_channel(s_ws_chan);
        s_ws_chan = NULL;
        return APP_ERR_FAIL;
    }

    err = rmt_enable(s_ws_chan);
    if (err != ESP_OK) {
        LOGW(TAG, "rmt_enable 失败: %s —— 不装也不要紧", esp_err_to_name(err));
        (void)rmt_del_encoder(s_ws_enc);
        (void)rmt_del_channel(s_ws_chan);
        s_ws_enc  = NULL;
        s_ws_chan = NULL;
        return APP_ERR_FAIL;
    }

    LOGI(TAG, "状态灯 WS2812 就绪: GPIO%d, RMT 新驱动 %dHz(1tick=0.1us), T0H/T0L/T1H/T1L=%d/%d/%d/%d",
         BSP_STATUS_LED_GPIO, (int)WS_RES_HZ, WS_T0H, WS_T0L, WS_T1H, WS_T1L);
    return APP_OK;
#else
    s_ws_rmt = rmtInit(BSP_STATUS_LED_GPIO, RMT_TX_MODE, RMT_MEM_64);
    if (s_ws_rmt == NULL) {
        LOGW(TAG, "状态灯 RMT 通道申请失败(GPIO%d) —— 不装也不要紧", BSP_STATUS_LED_GPIO);
        return APP_ERR_FAIL;
    }

    // 100ns 一拍
    const float tick_ns = rmtSetTick(s_ws_rmt, WS_TICK_NS);
    if (tick_ns < 90.0f || tick_ns > 110.0f) {
        LOGW(TAG, "状态灯 RMT 分频不是 100ns 而是 %.1fns，颜色可能有偏差", (double)tick_ns);
    }

    LOGI(TAG,
         "状态灯 WS2812 就绪: GPIO%d, RMT %.0fns/tick, T0H/T0L/T1H/T1L=%d/%d/%d/%d tick, 复位=%dus",
         BSP_STATUS_LED_GPIO, (double)tick_ns, WS_T0H, WS_T0L, WS_T1H, WS_T1L, WS_RESET_US);
    return APP_OK;
#endif
}

// 发一帧 GRB
static app_err_t ws_send(const uint8_t *grb) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;
    // 发完锁低
    tx_cfg.flags.eot_level = 0;

    esp_err_t err = rmt_transmit(s_ws_chan, s_ws_enc, grb, WS_BYTES, &tx_cfg);
    if (err == ESP_OK) {
        // 栈上的数据要等搬完
        err = rmt_tx_wait_all_done(s_ws_chan, WS_WAIT_MS);
    }

    if (err != ESP_OK) {
        return APP_ERR_FAIL;
    }
#else
    rmt_data_t items[WS_BYTES * 8];

    for (int i = 0; i < (WS_BYTES * 8); i++) {
        // 高位先发
        const bool one = ((grb[i / 8] >> (7 - (i % 8))) & 0x01) != 0;

        items[i].level0    = 1;
        items[i].duration0 = (uint32_t)(one ? WS_T1H : WS_T0H);
        items[i].level1    = 0;
        items[i].duration1 = (uint32_t)(one ? WS_T1L : WS_T0L);
    }

    if (!rmtWriteBlocking(s_ws_rmt, items, WS_BYTES * 8)) {
        return APP_ERR_FAIL;
    }
#endif

    // 灯珠要一段低电平才锁存
    esp_rom_delay_us(WS_RESET_US);
    return APP_OK;
}

// 发一帧，抢不到锁就跳过
static void ws_commit(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_ws_ready || (s_ws_lock == NULL)) {
        return;
    }

    if (xSemaphoreTake(s_ws_lock, pdMS_TO_TICKS(WS_LOCK_WAIT_MS)) != pdTRUE) {
        LOGW(TAG, "状态灯忙，本次颜色更新被跳过");
        return;
    }

    // 线上顺序是 GRB
    const uint8_t grb[WS_BYTES] = {g, r, b};

    if (ws_send(grb) != APP_OK) {
        if (!s_ws_warned) {
            s_ws_warned = true;
            LOGW(TAG, "状态灯发送失败，可能灯珠没焊/引脚不对，不影响开机");
        }
    }

    xSemaphoreGive(s_ws_lock);
}

// 失败也返回成功
app_err_t bsp_status_led_init(void) {
    if (s_ws_inited) {
        return APP_OK;
    }
    s_ws_inited = true;

    if (s_ws_lock == NULL) {
        s_ws_lock = xSemaphoreCreateMutex();
        if (s_ws_lock == NULL) {
            LOGW(TAG, "状态灯互斥锁创建失败，状态灯不可用");
            return APP_OK;
        }
    }

    if (ws_init() != APP_OK) {
        return APP_OK;
    }

    s_ws_ready = true;

    // 上电先发全黑
    ws_commit(0, 0, 0);
    return APP_OK;
}

void bsp_status_led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_ws_ready) {
        // 没就绪就忽略
        return;
    }

    if (r > WS_BRIGHT_MAX) {
        r = WS_BRIGHT_MAX;
    }
    if (g > WS_BRIGHT_MAX) {
        g = WS_BRIGHT_MAX;
    }
    if (b > WS_BRIGHT_MAX) {
        b = WS_BRIGHT_MAX;
    }

    ws_commit(r, g, b);
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
        LOGW(TAG, "未知状态灯编码 %d，按熄灭处理", code);
        bsp_status_led_set(0, 0, 0);
        break;
    }
}

// 关掉时的空实现
#else

static bool s_led_inited;

app_err_t bsp_status_led_init(void) {
    if (!s_led_inited) {
        s_led_inited = true;
        LOGI(TAG, "状态灯已被 BSP_STATUS_LED_ENABLE=0 关闭，跳过初始化");
    }
    return APP_OK;
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
