# CMakeLists.txt for App component
# 设备逻辑：设备状态中枢 + 指令路由 + 自动联动 + 阈值策略 + 联网通道
# 把"要执行器"和"要读模式/阈值"的两拨代码放进同一个组件，依赖环就不存在了

idf_component_register(
    SRCS
        "device_model.c"
        "app_link.c"
        "automation.c"
        "home_auto.c"
        "net_link.c"
        "mqtt_protocol.c"
        "ble_app.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        BSP
        driver
        esp_wifi
        esp_netif
        esp_event
        mqtt
        json
        lwip
        nvs_flash
        esp_timer
        freertos
        bt
)
