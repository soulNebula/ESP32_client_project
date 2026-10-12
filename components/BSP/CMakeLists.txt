# CMakeLists.txt for BSP component
# 板级硬件驱动：引脚真源 + 三条总线 + 各器件驱动 + 语音通道
# 引脚真源只有 board_config.h 一份，本组件内所有 .c 都从它取值，不许硬编码 GPIO 号

idf_component_register(
    SRCS
        "I2C/i2c_bus.c"
        "ADC/adc_bus.c"
        "GPIO/gpio_util.c"
        "LED/led.c"
        "KEY/key.c"
        "SERVO/servo.c"
        "BUZZER/buzzer.c"
        "SENSOR/sensor.c"
        "I2S_MIC/i2s_mic.c"
        "VOICE/voice.c"
        "VOICE/voice_esp_sr.c"
    INCLUDE_DIRS
        "."
        "I2C"
        "ADC"
        "GPIO"
        "LED"
        "KEY"
        "SERVO"
        "BUZZER"
        "SENSOR"
        "I2S_MIC"
        "VOICE"
    REQUIRES
        driver
        esp_adc
        esp_timer
        esp_hw_support
        esp_ringbuf
        freertos
        json
        espressif__esp-sr
)
