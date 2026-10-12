idf_component_register(
    SRCS
        "display_bsp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${PROJECT_DIR}/main"
    PRIV_REQUIRES
        "esp_lcd"
        "esp_lcd_st7701"
        "esp_lcd_touch"
        "esp_lcd_touch_gt911"
        "esp_lvgl_port"
        "lvgl"
        "driver"
        "esp_driver_gpio"
        "esp_driver_i2c"
        "esp_driver_ledc"
        "esp_timer"
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LV_USE_FONT_COMPRESSED=1
    LV_USE_SNAPSHOT=1
)
