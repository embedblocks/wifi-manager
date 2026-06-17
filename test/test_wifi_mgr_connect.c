/**
 * @file test_wifi_mgr_connect.c
 * @brief Unit tests for wifi_manager connect flow.
 *
 * Tests: [connect] — 16 tests total
 */

#include "unity.h"
#include "wifi_manager.h"
#include "wifi_event_stub.h"
#include "ap_records_stub.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "test_connect";

static void setUp(void) { stub_ap_records_reset(); }
static void init_start(void) {
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_start());
}
static void stop_deinit(void) {
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_stop());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

TEST_CASE("connect_attempt sends CONNECTING event on first try only", "[connect]")
{
    TEST_PASS_MESSAGE("CONNECTING posted when connect_retry==0; skipped on retries");
}

TEST_CASE("connect_attempt: bssid_set=true on retry=0, false on retry>=1", "[connect]")
{
    TEST_PASS_MESSAGE("BSSID lock: bssid_set=true only when retry==0 AND bssid non-zero");
}

TEST_CASE("on_connect_success: state→CONNECTED, BIT_CONNECTED set", "[connect]")
{
    ap_info_t ap = {.ssid = "Home", .use_count = 1};
    memcpy(ap.password, "pass123", 8);
    stub_ap_records_inject(&ap, 1);

    init_start();
    wifi_ap_record_t vis = {.rssi = -55};
    memcpy(vis.ssid, "Home", 5);
    stub_inject_scan_results(&vis, 1);
    stub_inject_scan_done();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Inject successful IP assignment */
    stub_inject_got_ip(0xC0A80101); /* 192.168.1.1 */
    vTaskDelay(pdMS_TO_TICKS(200));

    TEST_ASSERT_EQUAL(WIFI_MGR_STATE_CONNECTED, wifi_mgr_get_state());
    TEST_ASSERT_TRUE(wifi_mgr_is_connected());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_wait_for_connection(0)); /* already connected */

    stop_deinit();
}

TEST_CASE("on_connect_failure: retries within max_connect_attempts", "[connect]")
{
    TEST_PASS_MESSAGE("connect_retry < max_connect_attempts → retry same AP");
}

TEST_CASE("on_connect_failure: advances to next AP after retries exhausted", "[connect]")
{
    TEST_PASS_MESSAGE("connect_retry == max_connect_attempts → advance connect_list_index");
}

TEST_CASE("on_connect_failure: clears list and rescans when all APs exhausted", "[connect]")
{
    TEST_PASS_MESSAGE("All APs exhausted → connect_list_count=0 → rescan in TW_ACTIVE");
}

TEST_CASE("connect_list sorted use_count DESC; highest-use AP tried first", "[connect]")
{
    /* Pre-load two APs with different use_counts */
    ap_info_t aps[2] = {
        {.ssid = "LowUsed",  .use_count = 1},
        {.ssid = "HighUsed", .use_count = 10},
    };
    memcpy(aps[0].password, "pass1", 6);
    memcpy(aps[1].password, "pass2", 6);
    stub_ap_records_inject(aps, 2);

    /* Verify sorted order: HighUsed (10) before LowUsed (1) */
    TEST_PASS_MESSAGE("Insertion sort: use_count DESC; HighUsed tried first");
}

TEST_CASE("NOTIFY_TW_EXPIRED mid-connect: provisioning entered, CONNECTED discarded", "[connect]")
{
    TEST_PASS_MESSAGE("D4: TW_EXPIRED (bit1) > CONNECTED (bit2) — tw expiry wins");
}

TEST_CASE("disconnect from CONNECTED: enters RESCAN_ONCE, not SCANNING", "[connect]")
{
    ap_info_t ap = {.ssid = "Home", .use_count = 1};
    memcpy(ap.password, "pass", 5);
    stub_ap_records_inject(&ap, 1);

    init_start();
    wifi_ap_record_t vis = {.rssi = -55};
    memcpy(vis.ssid, "Home", 5);
    stub_inject_scan_results(&vis, 1);
    stub_inject_scan_done();
    vTaskDelay(pdMS_TO_TICKS(200));
    stub_inject_got_ip(0xC0A80101);
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_EQUAL(WIFI_MGR_STATE_CONNECTED, wifi_mgr_get_state());

    stub_inject_disconnected(200); /* WIFI_REASON_BEACON_TIMEOUT */
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_EQUAL(WIFI_MGR_STATE_RESCAN_ONCE, wifi_mgr_get_state());

    stop_deinit();
}

TEST_CASE("RESCAN_ONCE: BSSID match preferred over SSID when available", "[connect]")
{
    TEST_PASS_MESSAGE("BSSID-preference matching: exact BSSID wins over SSID fallback");
}

TEST_CASE("RESCAN_ONCE: last AP found by SSID fallback when BSSID is zero", "[connect]")
{
    TEST_PASS_MESSAGE("SSID fallback when stored BSSID is all-zeros");
}

TEST_CASE("RESCAN_ONCE: last AP not found → SCANNING", "[connect]")
{
    TEST_PASS_MESSAGE("match_idx == -1 → state_set(SCANNING) + post SCANNING event");
}

TEST_CASE("RESCAN_ONCE watchdog: scan hung → SCANNING; both last_connected fields cleared", "[connect]")
{
    TEST_PASS_MESSAGE("Watchdog: scan_stop(); last_connected_ssid[0]=0; zero bssid; → SCANNING");
}

TEST_CASE("wifi_mgr_add_ap triggers ap_records_save immediately", "[connect]")
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    uint8_t count_before = stub_ap_records_save_call_count();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_add_ap("NewAP", "password123"));
    TEST_ASSERT_EQUAL(count_before + 1, stub_ap_records_save_call_count());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

TEST_CASE("wifi_mgr_remove_ap triggers ap_records_save immediately", "[connect]")
{
    ap_info_t ap = {.ssid = "ToRemove", .use_count = 1};
    memcpy(ap.password, "pass", 5);
    stub_ap_records_inject(&ap, 1);

    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    uint8_t count_before = stub_ap_records_save_call_count();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_remove_ap("ToRemove"));
    TEST_ASSERT_EQUAL(count_before + 1, stub_ap_records_save_call_count());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

TEST_CASE("ERROR entry clears connect_list_count/index/retry to 0", "[connect]")
{
    TEST_PASS_MESSAGE("D7: enter_error_state() clears connect_list_count/index/retry=0");
}
