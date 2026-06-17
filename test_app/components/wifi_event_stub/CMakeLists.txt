idf_component_register(
    SRCS
        "wifi_event_stub.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../src"   # wifi_mgr_ctx.h
    REQUIRES
        freertos
        esp_wifi
        esp_netif
        wifi_manager
)
