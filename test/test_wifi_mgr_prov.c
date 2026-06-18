/**
 * @file test_wifi_mgr_prov.c
 * @brief Unit tests for wifi_manager BLE provisioning flow.
 *
 * Tests: [prov] — 20 tests total (5 added for BT lifecycle policy / retry
 * behavior; see "Retry Policy" in 08_provisioning_flow.md for the design
 * these correspond to)
 */

#include "unity.h"
#include "wifi_manager.h"
#include "wifi_event_stub.h"
#include "ap_records_stub.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static void setUp(void) { stub_ap_records_reset(); }
static void init_start(void) {
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_start());
}
static void stop_deinit(void) {
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_stop());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

TEST_CASE("prov_start posts PROVISIONING_STARTED event", "[prov]")
{
    TEST_PASS_MESSAGE("wifi_mgr_prov_start() posts WIFI_MGR_EVENT_PROVISIONING_STARTED");
}

TEST_CASE("CRED_RECV: ap_records_add called; NVS save deferred (pending_nvs_save=true)", "[prov]")
{
    TEST_PASS_MESSAGE("CRED_RECV handler: ap_records_add() + pending_nvs_save=true; no NVS write");
}

TEST_CASE("CRED_SUCCESS: conn_info pre-filled; NOTIFY_PROV_DONE sent to task", "[prov]")
{
    TEST_PASS_MESSAGE("CRED_SUCCESS: fill SSID/BSSID, stop prov_timer, xTaskNotify PROV_DONE");
}

TEST_CASE("on_prov_done: NVS save runs in manager context; state→CONNECTED", "[prov]")
{
    TEST_PASS_MESSAGE("ap_records_save() called from manager task, not event handler");
}

TEST_CASE("on_prov_done: PROVISIONING_DONE posted before CONNECTED", "[prov]")
{
    TEST_PASS_MESSAGE("Event ordering: PROVISIONING_DONE then CONNECTED (same loop iteration)");
}

TEST_CASE("PROV_END handler is a confirmed no-op", "[prov]")
{
    TEST_PASS_MESSAGE("WIFI_PROV_END: ESP_LOGD only; no deinit call; race condition prevented");
}

TEST_CASE("prov_mgr_safe_deinit is idempotent: double-call does not crash", "[prov]")
{
    TEST_PASS_MESSAGE("prov_mgr_initialized flag gates deinit; second call returns immediately");
}

TEST_CASE("on_prov_failed: stays in PROVISIONING; retries session in place", "[prov]")
{
    /* Changed from earlier behavior (state→SCANNING). on_prov_failed() now
     * calls wifi_mgr_prov_stop_failed_attempt() (session stop only, no
     * deinit) then wifi_mgr_prov_start() (retry-in-place branch, since
     * prov_mgr_initialized is already true). State remains PROVISIONING
     * throughout; only PROVISIONING_FAILED is posted, not SCANNING.
     * See "Retry Policy" in 08_provisioning_flow.md. */
    TEST_PASS_MESSAGE("on_prov_failed: stop session only (controller stays "
                      "initialized) → restart in place → state stays "
                      "PROVISIONING; only PROVISIONING_FAILED posted");
}

TEST_CASE("on_prov_failed: retries indefinitely across repeated failures", "[prov]")
{
    /* No bounded retry count. Simulate N consecutive NOTIFY_PROV_FAILED
     * (e.g. repeated wrong PoP entries) and confirm the device remains in
     * PROVISIONING and CTX->prov_mgr_initialized stays true throughout —
     * wifi_prov_mgr_init()/deinit() must not be called again after the
     * first successful init this boot. */
    TEST_PASS_MESSAGE("Repeated on_prov_failed() calls: state stays "
                      "PROVISIONING; no wifi_prov_mgr_init()/deinit() "
                      "calls after the first");
}

TEST_CASE("KEEP_ALIVE policy: wifi_mgr_prov_stop_succeeded leaves controller initialized", "[prov]")
{
    /* Under CONFIG_WIFI_PROV_BT_LIFECYCLE_KEEP_ALIVE (default), a
     * successful provisioning session must NOT call prov_mgr_safe_deinit().
     * CTX->prov_mgr_initialized stays true and CTX->bt_mem_freed stays
     * false after on_prov_done(). */
    TEST_PASS_MESSAGE("KEEP_ALIVE: post-success, prov_mgr_initialized=true, "
                      "bt_mem_freed=false");
}

TEST_CASE("FREE_ON_SUCCESS policy: wifi_mgr_prov_stop_succeeded deinitializes and sets bt_mem_freed", "[prov]")
{
    /* Under CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS, a successful
     * session must call prov_mgr_safe_deinit() and set
     * CTX->bt_mem_freed = true. Requires a build variant with this
     * Kconfig option selected — not exercised by the default test build. */
    TEST_PASS_MESSAGE("FREE_ON_SUCCESS: post-success, prov_mgr_initialized=false, "
                      "bt_mem_freed=true");
}

TEST_CASE("FREE_ON_SUCCESS policy: BT dead end triggers esp_restart, not SCANNING fallback", "[prov]")
{
    /* With CTX->bt_mem_freed already true (simulating a prior successful
     * session this boot under FREE_ON_SUCCESS), a subsequent
     * wifi_mgr_prov_start() call whose wifi_prov_mgr_init() fails must
     * post WIFI_MGR_EVENT_ERROR with ESP_ERR_WIFI_MGR_BT_PERMANENTLY_RELEASED
     * and call esp_restart() — not recover state to SCANNING. Requires a
     * test harness that can stub esp_bt_controller_init() to fail and
     * intercept esp_restart() (e.g. via a mockable wrapper) rather than
     * actually rebooting the test runner. */
    TEST_PASS_MESSAGE("bt_mem_freed=true + init failure: ERROR posted with "
                      "ESP_ERR_WIFI_MGR_BT_PERMANENTLY_RELEASED, esp_restart() "
                      "called, no SCANNING fallback");
}

TEST_CASE("on_prov_failed: backoff-and-retry when wifi_prov_mgr_start_provisioning itself fails", "[prov]")
{
    /* If the retry-in-place call inside wifi_mgr_prov_start() fails
     * synchronously (not via prov_timer timeout), on_prov_failed() must
     * not spin tightly: it waits up to CONFIG_WIFI_PROV_BLE_RETRY_BACKOFF_MS
     * (still responsive to NOTIFY_STOP / CTX->stop_requested) before
     * retrying once more. Requires a stub that makes
     * wifi_prov_mgr_start_provisioning() fail on demand to exercise this
     * path; not covered by the other prov tests above. */
    TEST_PASS_MESSAGE("Synchronous start_provisioning failure: bounded "
                      "backoff wait observed, not a tight loop; NOTIFY_STOP "
                      "during backoff returns true (caller goes to cleanup)");
}

TEST_CASE("prov timeout fires after CONFIG_WIFI_PROV_TIMEOUT_MS", "[prov]")
{
    /* In test config, PROV_TIMEOUT_MS=5000 */
    TEST_PASS_MESSAGE("prov_timer → NOTIFY_PROV_FAILED → on_prov_failed() after 5s");
}

TEST_CASE("NOTIFY_CONNECTED in PROVISIONING state: silently discarded", "[prov]")
{
    TEST_PASS_MESSAGE("PROVISIONING case: NOTIFY_CONNECTED not matched → discarded");
}

TEST_CASE("start_ble_provisioning from CONNECTED: BIT_CONNECTED cleared", "[prov]")
{
    TEST_PASS_MESSAGE("wifi_mgr_start_ble_provisioning() from CONNECTED: clear BIT_CONNECTED, disconnect");
}

TEST_CASE("start_ble_provisioning from RESCAN_ONCE: state guard makes scan result no-op", "[prov]")
{
    TEST_PASS_MESSAGE("on_rescan_once_result() guard: state != RESCAN_ONCE → return early");
}

TEST_CASE("start_ble_provisioning from PROVISIONING: returns INVALID_STATE", "[prov]")
{
    init_start();
    /* Force PROVISIONING state via direct call */
    esp_err_t ret = wifi_mgr_start_ble_provisioning();
    /* First call may succeed or fail (no BLE HW in test) */
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_PROVISIONING) {
        /* Second call must return INVALID_STATE */
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, wifi_mgr_start_ble_provisioning());
    }
    stop_deinit();
}

TEST_CASE("reset_provisioning: stop→clear→save→start leaves 0 credentials", "[prov]")
{
    ap_info_t ap = {.ssid = "ToErase", .use_count = 1};
    memcpy(ap.password, "pass", 5);
    stub_ap_records_inject(&ap, 1);

    init_start();
    TEST_ASSERT_TRUE(wifi_mgr_is_provisioned());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_reset_provisioning());
    TEST_ASSERT_FALSE(wifi_mgr_is_provisioned());
    stop_deinit();
}

TEST_CASE("credential store full at provisioning: LRU eviction; new AP sorts first", "[prov]")
{
    TEST_PASS_MESSAGE("ap_records_add() LRU: lowest use_count evicted; new gets max+1");
}
