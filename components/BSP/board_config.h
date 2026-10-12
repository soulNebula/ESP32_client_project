// c
// 引脚分配表，改引脚只改这里

#ifndef __BOARD_CONFIG_H__
#define __BOARD_CONFIG_H__

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#ifdef __cplusplus
extern "C" {
#endif

// 温光屏共用一路
#define BSP_I2C_PORT     I2C_NUM_0
#define BSP_I2C_SDA_GPIO GPIO_NUM_8
#define BSP_I2C_SCL_GPIO GPIO_NUM_9

// 400k 会超时
#define BSP_I2C_FREQ_HZ    100000
#define BSP_I2C_TIMEOUT_MS 100

// 读不到先扫描
#define BSP_I2C_ADDR_SHT30  0x44
#define BSP_I2C_ADDR_AHT20  0x38
#define BSP_I2C_ADDR_BH1750 0x23
// 地址看背面跳线
#define BSP_I2C_ADDR_SSD1306     0x3C
#define BSP_I2C_ADDR_SSD1306_ALT 0x3D

// ADC2 不可用
#define BSP_ADC_UNIT     ADC_UNIT_1
#define BSP_ADC_ATTEN    ADC_ATTEN_DB_12
#define BSP_ADC_BITWIDTH ADC_BITWIDTH_12

// 通道号非 GPIO
#define BSP_ADC_CH_LIGHT ADC_CHANNEL_0
#define BSP_ADC_CH_RAIN  ADC_CHANNEL_1

#define BSP_GPIO_LIGHT_ADC GPIO_NUM_1
#define BSP_GPIO_RAIN_AO   GPIO_NUM_2
// DO 可选不接
#define BSP_GPIO_RAIN_DO GPIO_NUM_3

// 1=光强电压低
#define BSP_LIGHT_ADC_INVERT 1

// 1=越湿电压高
// 悬空读数判无效
#define BSP_RAIN_ADC_INVERT 1

#define BSP_LED_ZONE_COUNT 4

#define BSP_LED_GPIO_LIVING  GPIO_NUM_4
#define BSP_LED_GPIO_KITCHEN GPIO_NUM_5
#define BSP_LED_GPIO_BEDROOM GPIO_NUM_6
#define BSP_LED_GPIO_BATH    GPIO_NUM_7

#define BSP_LED_PWM_TIMER LEDC_TIMER_2
#define BSP_LED_PWM_MODE  LEDC_LOW_SPEED_MODE
#define BSP_LED_PWM_RES   LEDC_TIMER_10_BIT
// 避开可见闪烁
#define BSP_LED_PWM_FREQ_HZ 5000

// 低电平点亮改 1
#define BSP_LED_ACTIVE_LOW 0

// 只有门一路
#define BSP_SERVO_TIMER LEDC_TIMER_0
#define BSP_SERVO_MODE  LEDC_LOW_SPEED_MODE
// 分辨率 14bit
#define BSP_SERVO_RES     LEDC_TIMER_14_BIT
#define BSP_SERVO_FREQ_HZ 50

#define BSP_SERVO_CH_DOOR LEDC_CHANNEL_0

#define BSP_SERVO_GPIO_DOOR GPIO_NUM_17
#define BSP_SERVO_COUNT     1

// 抖就调小 MAX
#define BSP_SERVO_MIN_PULSE_US 500
#define BSP_SERVO_MAX_PULSE_US 2500

// 开门转到 90 度
#define BSP_SERVO_DOOR_OPEN_DEG 90.0f
#define BSP_SERVO_DOOR_SHUT_DEG 0.0f

// 1=有源 0=无源
#define BSP_BUZZER_ACTIVE 1

#define BSP_BUZZER_GPIO GPIO_NUM_18
// 现场实测：本板模块低电平才响（默认高电平=停）
// active_low=0 时"响"是推挽拉高、"停"是开漏高阻 → 模块不响；
// active_low=1 时"响"是推挽拉低、"停"是开漏高阻 → 正好匹配
#define BSP_BUZZER_ACTIVE_LOW 1

// 通道必须错开
// 蜂鸣器要单独定时器
#define BSP_BUZZER_LEDC_TIMER LEDC_TIMER_1
#define BSP_BUZZER_LEDC_CH    LEDC_CHANNEL_3
// 2k 人耳最敏感
#define BSP_BUZZER_FREQ_HZ 2000
#define BSP_BUZZER_DUTY_ON 50

// 彩屏坏改 I2C
#define BSP_PANEL_NAME "SSD1306 128x64"
// 0=旧 SPI 屏
#define BSP_PANEL_IS_I2C 1

// KEY1=BOOT
#define BSP_KEY_GPIO_KEY1     GPIO_NUM_0
#define BSP_KEY_GPIO_KEY2     GPIO_NUM_15
#define BSP_KEY_ACTIVE_LEVEL  0
#define BSP_KEY_DEBOUNCE_MS   30
#define BSP_KEY_LONG_PRESS_MS 2000
#define BSP_KEY_COUNT         2

// SCK 不用 13
// 38 与状态灯冲突
#define BSP_I2S_MIC_ENABLE   1
#define BSP_I2S_MIC_PORT     I2S_NUM_0
#define BSP_I2S_MIC_SCK_GPIO GPIO_NUM_38
#define BSP_I2S_MIC_WS_GPIO  GPIO_NUM_39
#define BSP_I2S_MIC_SD_GPIO  GPIO_NUM_40
// 16k 单声道
#define BSP_I2S_MIC_SAMPLE_RATE 16000
#define BSP_I2S_MIC_READ_TIMEOUT_MS 100

// v1.1 才有
#define BSP_STATUS_LED_ENABLE 1
#define BSP_STATUS_LED_GPIO   GPIO_NUM_48

// 只联动客厅灯
#define BSP_DEF_LIGHT_ON_PCT  20.0f
#define BSP_DEF_LIGHT_OFF_PCT 45.0f

// 迟滞 32.5/32.0（用户指定：超过 32.5℃ 就响）
#define BSP_DEF_BUZZER_ON_C  32.5f
#define BSP_DEF_BUZZER_OFF_C 32.0f

// ≥32 数字闪烁
#define BSP_DEF_TEMP_ALERT_C 32.0f

#define BSP_DEF_RAIN_PCT 30.0f

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CONFIG_H__ */
