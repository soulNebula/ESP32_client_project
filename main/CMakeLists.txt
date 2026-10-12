# CMakeLists.txt for main component
# board.c 的编排故意放在这里、不放进 BSP：它要同时认识 BSP / ui / App，
# 放回 BSP 立刻形成组件依赖环，而 main 本来就依赖所有组件，放这里依赖图是干净的树

idf_component_register(
    SRCS
        "main.c"
        "board.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        BSP
        App
        ui
        driver
        nvs_flash
        esp_wifi
        esp_timer
        freertos
)
