idf_component_register(
    SRCS
        "audio_bsp.c"
        "rock_player.c"
        "rock_player_tx.c"
        "rock_player_decode.c"
        "rock_player_rx.c"
        "rock_player_bringup.c"
        "audio_spectrum.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${PROJECT_DIR}/main"
    PRIV_REQUIRES
        "rock_source"
        "esp_codec_dev"
        "esp_audio_codec"
        "esp-dsp"
        "driver"
        "esp_driver_gpio"
        "esp_driver_i2s"
        "esp_driver_i2c"
        "esp_timer"
        "heap"
    WHOLE_ARCHIVE
)
