idf_component_register(
    SRCS
        "rock_cast_discovery.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        "mdns"
        "esp-tls"
        "esp_netif"
        "esp_timer"
        "heap"
        "lwip"
    WHOLE_ARCHIVE
)
