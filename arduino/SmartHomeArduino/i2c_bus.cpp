// c
// I2C 总线封装（走 Wire）
#include <Arduino.h>
#include <Wire.h>

#include "app_common.h"
#include "bsp_config.h"
#include "i2c_bus.h"

static const char *TAG = "I2C";

// 保留区
#define I2C_ADDR_MIN 0x08
#define I2C_ADDR_MAX 0x77

// 告警位图容量
#define I2C_WARN_MAX 128

// 同一地址只报一次
static bool s_warned[I2C_WARN_MAX];

// 总线建好了吗
static bool s_ready;

// 地址合法吗
static bool i2c_addr_ok(uint8_t addr) {
    return (addr >= I2C_ADDR_MIN) && (addr <= I2C_ADDR_MAX);
}

// 防日志刷屏
static void i2c_warn_once(uint8_t addr, const char *what) {
    if (addr < I2C_WARN_MAX) {
        if (s_warned[addr]) {
            return;
        }
        s_warned[addr] = true;
    }

    LOGW(TAG, "%s 0x%02X 失败，同一地址只报这一次", what, (unsigned)addr);
}

// 补芯片名
static void i2c_log_known_addr(uint8_t addr) {
    switch (addr) {
    case BSP_I2C_ADDR_SHT30:
        LOGI(TAG, "       ^ 像是 SHT30/SHT31 温湿度");
        break;
    case BSP_I2C_ADDR_AHT20:
        LOGI(TAG, "       ^ 像是 AHT20 温湿度");
        break;
    case BSP_I2C_ADDR_BH1750:
        LOGI(TAG, "       ^ 像是 BH1750 光照");
        break;
    default:
        break;
    }
}

// 只跑 100k
app_err_t bsp_i2c_init(void) {
    if (s_ready) {
        LOGD(TAG, "I2C 已建好，重复调用忽略");
        return APP_OK;
    }

    // 屏先起过也算成功
    if (!Wire.begin(BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ)) {
        LOGE(TAG, "Wire.begin(SDA=GPIO%d, SCL=GPIO%d, %dHz) 失败，总线不可用", BSP_I2C_SDA_GPIO,
             BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ);
        return APP_ERR_FAIL;
    }

    // 从机不响应时别拖死主循环
    Wire.setTimeOut(BSP_I2C_TIMEOUT_MS);

    s_ready = true;
    LOGI(TAG, "I2C ready: SDA=GPIO%d SCL=GPIO%d @%dHz", BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO,
         BSP_I2C_FREQ_HZ);
    return APP_OK;
}

// 不等于线上有设备
bool bsp_i2c_ready(void) {
    return s_ready;
}

// 用探测别发数据
int bsp_i2c_scan(uint8_t *found, int max) {
    if (!s_ready) {
        LOGE(TAG, "总线没建起来，扫描跳过");
        return 0;
    }

    LOGI(TAG, "scanning 0x%02X ~ 0x%02X ...", I2C_ADDR_MIN, I2C_ADDR_MAX);

    int hits = 0;

    for (int addr = I2C_ADDR_MIN; addr <= I2C_ADDR_MAX; addr++) {
        // 没人应答是常态
        if (!bsp_i2c_probe((uint8_t)addr)) {
            continue;
        }

        LOGI(TAG, "  found device at 0x%02X", (unsigned)addr);
        i2c_log_known_addr((uint8_t)addr);

        if ((found != NULL) && (hits < max)) {
            found[hits] = (uint8_t)addr;
        }
        hits++;
    }

    if (hits == 0) {
        // 扫不到不是错误
        LOGW(TAG, "no I2C device found (check SDA=GPIO%d / SCL=GPIO%d and 4.7k pull-ups)",
             BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
    } else {
        LOGI(TAG, "scan done, %d device(s) found", hits);
    }
    return hits;
}

// 空事务看 ACK
bool bsp_i2c_probe(uint8_t addr) {
    if (!s_ready || !i2c_addr_ok(addr)) {
        return false;
    }

    Wire.beginTransmission(addr);
    // 0 才算在线
    return (Wire.endTransmission() == 0);
}

app_err_t bsp_i2c_write(uint8_t addr, const uint8_t *data, int len) {
    if (!s_ready || !i2c_addr_ok(addr) || (data == NULL) || (len <= 0)) {
        return APP_ERR_INVALID_ARG;
    }

    Wire.beginTransmission(addr);
    Wire.write(data, (size_t)len);

    if (Wire.endTransmission() != 0) {
        i2c_warn_once(addr, "写");
        return APP_ERR_FAIL;
    }
    return APP_OK;
}

// 不发寄存器地址
app_err_t bsp_i2c_read(uint8_t addr, uint8_t *data, int len) {
    if (!s_ready || !i2c_addr_ok(addr) || (data == NULL) || (len <= 0)) {
        return APP_ERR_INVALID_ARG;
    }

    const int got = (int)Wire.requestFrom((int)addr, (int)len);
    if (got != len) {
        i2c_warn_once(addr, "读");
        return APP_ERR_FAIL;
    }

    for (int i = 0; i < len; i++) {
        const int b = Wire.read();
        if (b < 0) {
            i2c_warn_once(addr, "读");
            return APP_ERR_FAIL;
        }
        data[i] = (uint8_t)b;
    }
    return APP_OK;
}

// 先写寄存器再读
app_err_t bsp_i2c_write_read(uint8_t addr, const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen) {
    if (!s_ready || !i2c_addr_ok(addr) || (wbuf == NULL) || (wlen <= 0) || (rbuf == NULL) ||
        (rlen <= 0)) {
        return APP_ERR_INVALID_ARG;
    }

    Wire.beginTransmission(addr);
    Wire.write(wbuf, (size_t)wlen);

    // false = 不发停止位，直接接着读
    if (Wire.endTransmission(false) != 0) {
        i2c_warn_once(addr, "写");
        return APP_ERR_FAIL;
    }

    const int got = (int)Wire.requestFrom((int)addr, (int)rlen);
    if (got != rlen) {
        i2c_warn_once(addr, "读");
        return APP_ERR_FAIL;
    }

    for (int i = 0; i < rlen; i++) {
        const int b = Wire.read();
        if (b < 0) {
            i2c_warn_once(addr, "读");
            return APP_ERR_FAIL;
        }
        rbuf[i] = (uint8_t)b;
    }
    return APP_OK;
}
