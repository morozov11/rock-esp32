idf_component_register(
    SRCS
        "rock_gui.c"
        "rock_gui_widgets.c"
        "rock_gui_keyboard.c"
        "rock_gui_views_catalog.c"
        "rock_gui_views_cast.c"
        "rock_gui_views_now_playing.c"
        "rock_ui_screens.c"
        "rock_serial.c"
        "rock_screenshot.c"
        "fonts/font_hack_16.c"
        "fonts/font_inter_14.c"
        "fonts/font_inter_18.c"
        "fonts/font_inter_24.c"
    INCLUDE_DIRS "include" "fonts"
    REQUIRES
        "rock_platform"
        "rock_audio"
        "rock_display"
        "esp_lvgl_port"
        "lvgl"
    PRIV_REQUIRES
        "driver"
        "esp_driver_usb_serial_jtag"
        "esp_wifi"
        "esp_timer"
        "heap"
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LV_USE_FONT_COMPRESSED=1
    LV_USE_SNAPSHOT=1
)
