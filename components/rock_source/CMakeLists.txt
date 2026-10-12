idf_component_register(
    SRCS
        "rock_source.c"
        "rock_relay_http.c"
        "rock_ring.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        "rock_platform"
        "esp_http_client"
        "esp_audio_codec"
        "esp_timer"
        "esp-tls"
        "mbedtls"
        "heap"
        "lwip"
    WHOLE_ARCHIVE
)
