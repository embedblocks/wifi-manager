idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        wifi-manager
        nvs_flash
        esp_netif
        esp_event
        
)
