# CMakeLists.txt for ui component
# 屏幕界面：0.96 寸单色 OLED(SSD1306 128x64, I2C) 驱动 + 1bpp 页显存脏页差分 + 中英文字库 + 四行排版
# 本组件不认识 device_id_t / sensor 快照，只认 display_view_input_t 这个视图模型
# 换屏或换排版都不影响业务层

idf_component_register(
    SRCS
        "panel.c"
        "panel_font.c"
        "font_cn_data.c"     # 基础字 + ASCII 8x16（tools/dev/convert_font.py 生成）
        "font_cn_extra.c"    # 补充汉字（tools/dev/gen_panel_font.ps1 生成）
        "display_view.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        BSP
        driver
        esp_timer
        freertos
)
