idf_component_register(
    SRCS
        "src/blob_storage.c"
        "src/ap_records.c"
        "src/wifi_mgr_events.c"
        "src/wifi_mgr_state.c"
        "src/wifi_mgr_scan.c"
        "src/wifi_mgr_connect.c"
        "src/wifi_mgr_prov.c"
        "src/wifi_manager.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_wifi
        esp_event
        wifi_provisioning
        nvs_flash
        esp_netif
        bt        
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
