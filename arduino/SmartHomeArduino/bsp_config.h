// c
// 引脚分配表，改引脚只改这里
#pragma once

// 温光屏共用一路
#define BSP_I2C_SDA_GPIO 8
#define BSP_I2C_SCL_GPIO 9

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

// 12bit 满量程
#define BSP_ADC_RESOLUTION 12

// 11dB 才到 3.3V
#define BSP_ADC_ATTENUATION ADC_11db

// ESP32-S3 只有 ADC1 能用
#define BSP_GPIO_LIGHT_ADC 1
#define BSP_GPIO_RAIN_AO   2
// DO 可选不接
#define BSP_GPIO_RAIN_DO   3

// 1=光强电压低
#define BSP_LIGHT_ADC_INVERT 1

// 0=越湿电压高（跟主工程 sensor.c 一致）
// 装反了改 1
// 悬空读数判无效
#define BSP_RAIN_ADC_INVERT 0

#define BSP_LED_ZONE_COUNT 4

#define BSP_LED_GPIO_LIVING  4
#define BSP_LED_GPIO_KITCHEN 5
#define BSP_LED_GPIO_BEDROOM 6
#define BSP_LED_GPIO_BATH    7

// 避开可见闪烁
#define BSP_LED_PWM_FREQ_HZ 5000

// 10bit 够用
#define BSP_LED_PWM_RES 10

// 低电平点亮改 1
#define BSP_LED_ACTIVE_LOW 0

// 只有门一路
#define BSP_SERVO_GPIO_DOOR 17
#define BSP_SERVO_COUNT     1

#define BSP_SERVO_FREQ_HZ 50

// ★ 不能超过 14：S3 的 LEDC 计数器只有 14 位
// 写 16 会让 ledc_timer_config 直接失败，门就死了
#define BSP_SERVO_RES 14

// 抖就调小 MAX
#define BSP_SERVO_MIN_PULSE_US 500
#define BSP_SERVO_MAX_PULSE_US 2500

// 开门转到 90 度
#define BSP_SERVO_DOOR_OPEN_DEG 90.0f
#define BSP_SERVO_DOOR_SHUT_DEG 0.0f

// 1=有源 0=无源
#define BSP_BUZZER_ACTIVE 1

#define BSP_BUZZER_GPIO 18

// 现场实测：本板模块低电平才响（默认高电平=停）
// 低电平触发型模块用 1，普通模块用 0
#define BSP_BUZZER_ACTIVE_LOW 1

// 无源才用得上
#define BSP_BUZZER_FREQ_HZ 2000
#define BSP_BUZZER_DUTY_ON 50

// 彩屏坏改 I2C
#define BSP_PANEL_NAME "SSD1306 128x64"

#define BSP_PANEL_WIDTH  128
#define BSP_PANEL_HEIGHT 64

// KEY1=BOOT
#define BSP_KEY_GPIO_KEY1    0
#define BSP_KEY_GPIO_KEY2    15
#define BSP_KEY_ACTIVE_LEVEL 0
#define BSP_KEY_DEBOUNCE_MS  30
// 长按 2 秒
#define BSP_KEY_LONG_PRESS_MS 2000
#define BSP_KEY_COUNT         2

// INMP441 麦克风
// SCK 不用 13，38 与状态灯冲突
#define BSP_I2S_MIC_SCK_GPIO 38
#define BSP_I2S_MIC_WS_GPIO  39
#define BSP_I2S_MIC_SD_GPIO  40
// 16k 单声道
#define BSP_I2S_MIC_SAMPLE_RATE 16000

// v1.1 才有
#define BSP_STATUS_LED_ENABLE 1
#define BSP_STATUS_LED_GPIO   48
// 很亮，压到 40
#define BSP_STATUS_LED_BRIGHT 40

// 只联动客厅灯
#define BSP_DEF_LIGHT_ON_PCT  20.0f
#define BSP_DEF_LIGHT_OFF_PCT 45.0f

// 迟滞 32.5/32.0（用户指定：超过 32.5℃ 就响）
#define BSP_DEF_BUZZER_ON_C  32.5f
#define BSP_DEF_BUZZER_OFF_C 32.0f

// ≥32 数字闪烁
#define BSP_DEF_TEMP_ALERT_C 32.0f

#define BSP_DEF_RAIN_PCT 30.0f
