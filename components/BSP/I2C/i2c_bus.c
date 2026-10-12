// c
// I2C0 收发与扫描
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

// 别混老驱动
#include "driver/i2c_master.h"

#include "board_config.h"
#include "i2c_bus.h"

static const char *TAG = "I2C";

// 温湿度 + 光照余量
#define BSP_I2C_MAX_DEVICES 8

// 防线被拉死
#define BSP_I2C_SCAN_PROBE_TIMEOUT_MS 10

// 保留区
#define BSP_I2C_SCAN_ADDR_MIN 0x08
#define BSP_I2C_SCAN_ADDR_MAX 0x77

// 排线长易随机 NAK
#define BSP_I2C_GLITCH_IGNORE_CNT 7

// 栈缓冲不用动态内存
#define BSP_I2C_REG_WRITE_MAX 64

// 从机登记项
typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} i2c_dev_slot_t;

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_dev_slot_t s_devs[BSP_I2C_MAX_DEVICES];
static int s_dev_cnt = 0;

static uint8_t s_scan_found[BSP_I2C_SCAN_MAX];
static int s_scan_cnt = 0;
// 扫过才走缓存
static bool s_scan_done = false;

static int i2c_timeout_or_default(uint32_t timeout_ms) {
    return (timeout_ms == 0u) ? (int)BSP_I2C_TIMEOUT_MS : (int)timeout_ms;
}

// 查登记表
static int i2c_find_slot(uint8_t addr) {
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_devs[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

// 补芯片名
static void i2c_log_known_addr(uint8_t addr) {
    switch (addr) {
    case BSP_I2C_ADDR_SHT30:
        ESP_LOGI(TAG, "       ^ 像是 SHT30/SHT31 温湿度");
        break;
    case BSP_I2C_ADDR_AHT20:
        ESP_LOGI(TAG, "       ^ 像是 AHT20 温湿度");
        break;
    case BSP_I2C_ADDR_BH1750:
        ESP_LOGI(TAG, "       ^ 像是 BH1750 光照");
        break;
    default:
        break;
    }
}

// 建主机总线
esp_err_t bsp_i2c_init(void) {
    if (s_bus != NULL) {
        ESP_LOGD(TAG, "I2C%d already initialized, skip", (int)BSP_I2C_PORT);
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BSP_I2C_PORT,
        .sda_io_num        = BSP_I2C_SDA_GPIO,
        .scl_io_num        = BSP_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = BSP_I2C_GLITCH_IGNORE_CNT,
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        // 内部上拉顶不住高速
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        // 别留野句柄
        s_bus = NULL;
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    // 先看引脚对不对
    ESP_LOGI(TAG, "I2C%d ready: SDA=GPIO%d SCL=GPIO%d @%dHz", (int)BSP_I2C_PORT, (int)BSP_I2C_SDA_GPIO,
             (int)BSP_I2C_SCL_GPIO, (int)BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

// 先摘从机再删总线
void bsp_i2c_deinit(void) {
    if (s_bus == NULL) {
        return;
    }

    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_devs[i].dev != NULL) {
            const esp_err_t err = i2c_master_bus_rm_device(s_devs[i].dev);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "rm device 0x%02X failed: %s", (unsigned)s_devs[i].addr, esp_err_to_name(err));
            }
        }
        s_devs[i].addr = 0;
        s_devs[i].dev  = NULL;
    }
    s_dev_cnt = 0;

    const esp_err_t err = i2c_del_master_bus(s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_del_master_bus failed: %s", esp_err_to_name(err));
    }
    s_bus = NULL;

    // 缓存作废
    s_scan_cnt  = 0;
    s_scan_done = false;

    ESP_LOGI(TAG, "I2C%d released", (int)BSP_I2C_PORT);
}

// 同一地址只许挂一次
esp_err_t bsp_i2c_add_device(uint8_t addr, uint32_t freq_hz, i2c_master_dev_handle_t *out_dev) {
    if (out_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_dev = NULL;

    if (s_bus == NULL) {
        ESP_LOGE(TAG, "add device 0x%02X before bus init", (unsigned)addr);
        return ESP_ERR_INVALID_STATE;
    }

    // 去掉保留和广播
    if (addr < BSP_I2C_SCAN_ADDR_MIN || addr > BSP_I2C_SCAN_ADDR_MAX) {
        ESP_LOGE(TAG, "invalid 7-bit addr 0x%02X (valid 0x%02X~0x%02X)", (unsigned)addr,
                 BSP_I2C_SCAN_ADDR_MIN, BSP_I2C_SCAN_ADDR_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    if (i2c_find_slot(addr) >= 0) {
        ESP_LOGE(TAG, "addr 0x%02X already registered, reuse the old handle", (unsigned)addr);
        return ESP_FAIL;
    }

    if (s_dev_cnt >= BSP_I2C_MAX_DEVICES) {
        ESP_LOGE(TAG, "device table full (%d), cannot add 0x%02X", BSP_I2C_MAX_DEVICES, (unsigned)addr);
        return ESP_ERR_NO_MEM;
    }

    // 传 0 用总线频率
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = (freq_hz == 0u) ? BSP_I2C_FREQ_HZ : freq_hz,
    };

    i2c_master_dev_handle_t dev = NULL;
    const esp_err_t         err = i2c_master_bus_add_device(s_bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X, %dHz) failed: %s", (unsigned)addr,
                 (int)dev_cfg.scl_speed_hz, esp_err_to_name(err));
        return err;
    }

    s_devs[s_dev_cnt].addr = addr;
    s_devs[s_dev_cnt].dev  = dev;
    s_dev_cnt++;

    *out_dev = dev;
    ESP_LOGI(TAG, "device 0x%02X added @%dHz", (unsigned)addr, (int)dev_cfg.scl_speed_hz);
    return ESP_OK;
}

// 中途不释放总线
esp_err_t bsp_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len,
                           uint32_t timeout_ms) {
    if (dev == NULL || buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, i2c_timeout_or_default(timeout_ms));
}

// 拼一包发防复位
esp_err_t bsp_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *data, size_t len,
                            uint32_t timeout_ms) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0u && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((len + 1u) > BSP_I2C_REG_WRITE_MAX) {
        ESP_LOGE(TAG, "write_reg len=%u too long (max %d)", (unsigned)len, BSP_I2C_REG_WRITE_MAX - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[BSP_I2C_REG_WRITE_MAX];
    frame[0] = reg;
    for (size_t i = 0; i < len; i++) {
        frame[i + 1u] = data[i];
    }

    return i2c_master_transmit(dev, frame, len + 1u, i2c_timeout_or_default(timeout_ms));
}

// 不发寄存器地址
esp_err_t bsp_i2c_read(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len, uint32_t timeout_ms) {
    if (dev == NULL || buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_receive(dev, buf, len, i2c_timeout_or_default(timeout_ms));
}

// 发命令字用
esp_err_t bsp_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (dev == NULL || data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit(dev, data, len, i2c_timeout_or_default(timeout_ms));
}

// 用探测别发数据
int bsp_i2c_scan(uint8_t *found, size_t max_found) {
    s_scan_cnt  = 0;
    s_scan_done = true;

    if (s_bus == NULL) {
        ESP_LOGE(TAG, "scan before bus init");
        return 0;
    }

    ESP_LOGI(TAG, "scanning 0x%02X ~ 0x%02X ...", (unsigned)BSP_I2C_SCAN_ADDR_MIN,
             (unsigned)BSP_I2C_SCAN_ADDR_MAX);

    int hits = 0;
    for (uint16_t addr = BSP_I2C_SCAN_ADDR_MIN; addr <= BSP_I2C_SCAN_ADDR_MAX; addr++) {
        // 没人应答是常态
        if (i2c_master_probe(s_bus, addr, BSP_I2C_SCAN_PROBE_TIMEOUT_MS) != ESP_OK) {
            continue;
        }

        ESP_LOGI(TAG, "  found device at 0x%02X", (unsigned)addr);
        i2c_log_known_addr((uint8_t)addr);

        if (hits < (int)BSP_I2C_SCAN_MAX) {
            s_scan_found[s_scan_cnt] = (uint8_t)addr;
            s_scan_cnt++;
        }
        if (found != NULL && (size_t)hits < max_found) {
            found[hits] = (uint8_t)addr;
        }
        hits++;
    }

    if (hits == 0) {
        // 扫不到不是错误
        ESP_LOGW(TAG, "no I2C device found (check SDA=GPIO%d / SCL=GPIO%d and 4.7k pull-ups)",
                 (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    } else {
        ESP_LOGI(TAG, "scan done, %d device(s) found", hits);
    }
    return hits;
}

// 扫过以缓存为准
bool bsp_i2c_present(uint8_t addr) {
    if (s_bus == NULL) {
        return false;
    }

    if (!s_scan_done) {
        return (i2c_master_probe(s_bus, addr, BSP_I2C_SCAN_PROBE_TIMEOUT_MS) == ESP_OK);
    }

    for (int i = 0; i < s_scan_cnt; i++) {
        if (s_scan_found[i] == addr) {
            return true;
        }
    }
    return false;
}

// 抖动测试必须用它
bool bsp_i2c_probe(uint8_t addr) {
    if (s_bus == NULL) {
        return false;
    }
    return (i2c_master_probe(s_bus, addr, BSP_I2C_SCAN_PROBE_TIMEOUT_MS) == ESP_OK);
}

// 不等于线上有设备
bool bsp_i2c_ready(void) {
    return (s_bus != NULL);
}

// 只读，别删别改
i2c_master_bus_handle_t bsp_i2c_bus(void) {
    return s_bus;
}
