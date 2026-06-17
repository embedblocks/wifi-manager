set(TEST_SRCS
    test_wifi_mgr_lifecycle.c
    test_wifi_mgr_scan.c
    test_wifi_mgr_connect.c
    test_wifi_mgr_prov.c
    test_wifi_mgr_state_machine.c
)

idf_component_register(
    SRCS ${TEST_SRCS}
    REQUIRES unity wifi_manager
)
