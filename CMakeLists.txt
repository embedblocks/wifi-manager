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
        nvs_flash
        esp_netif
    PRIV_REQUIRES
        # ESP-IDF v6.0 removed the built-in wifi_provisioning component.
        # Its replacement, espressif/network_provisioning, is pulled from
        # the component registry via idf_component.yml.
        network_provisioning
        bt
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
