idf_component_register(
    SRCS "src/net_relay.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_timer proto
    PRIV_REQUIRES esp_websocket_client esp-tls
)
