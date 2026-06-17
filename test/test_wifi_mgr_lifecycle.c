/**
 * @file test_wifi_mgr_lifecycle.c
 * @brief Unit tests for wifi_manager lifecycle (init/start/stop/deinit).
 *
 * Tests: [lifecycle] — 10 tests total
 * Requires: ap_records_stub, wifi_event_stub components in test_app/components/
 */

#include "unity.h"
#include "wifi_manager.h"
#include "esp_log.h"

static const char *TAG = "test_lifecycle";

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void init_and_start(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_start());
}

static void stop_and_deinit(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_stop());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

/* ── Test cases ───────────────────────────────────────────────────────────── */

TEST_CASE("init/deinit without start succeeds", "[lifecycle]")
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    TEST_ASSERT_EQUAL(WIFI_MGR_STATE_IDLE, wifi_mgr_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
    TEST_ASSERT_EQUAL(WIFI_MGR_STATE_IDLE, wifi_mgr_get_state());
}

TEST_CASE("double init returns INVALID_STATE", "[lifecycle]")
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, wifi_mgr_init(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

TEST_CASE("start without init returns INVALID_STATE", "[lifecycle]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, wifi_mgr_start());
}

TEST_CASE("deinit while started returns INVALID_STATE", "[lifecycle]")
{
    init_and_start();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, wifi_mgr_deinit());
    stop_and_deinit();
}

TEST_CASE("init+start+stop+deinit completes within 2s", "[lifecycle]")
{
    TickType_t start = xTaskGetTickCount();
    init_and_start();
    stop_and_deinit();
    TickType_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "Lifecycle elapsed: %lu ms", (unsigned long)elapsed);
    TEST_ASSERT_LESS_THAN(2000, elapsed);
}

TEST_CASE("wait_for_connection times out when not connected", "[lifecycle]")
{
    init_and_start();
    esp_err_t ret = wifi_mgr_wait_for_connection(200);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, ret);
    stop_and_deinit();
}

TEST_CASE("cancel_wait unblocks waiter immediately", "[lifecycle]")
{
    init_and_start();
    wifi_mgr_cancel_wait();
    esp_err_t ret = wifi_mgr_wait_for_connection(5000); /* would block 5s without cancel */
    TEST_ASSERT_EQUAL(ESP_ERR_WIFI_MGR_CANCELED, ret);
    stop_and_deinit();
}

TEST_CASE("reset_wait_cancel re-enables blocking wait", "[lifecycle]")
{
    init_and_start();
    wifi_mgr_cancel_wait();
    wifi_mgr_reset_wait_cancel();
    /* After reset, wait should time out normally (not return CANCELED) */
    esp_err_t ret = wifi_mgr_wait_for_connection(100);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, ret);
    stop_and_deinit();
}

TEST_CASE("is_provisioned returns false with empty ap_records_stub", "[lifecycle]")
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_init(NULL));
    TEST_ASSERT_FALSE(wifi_mgr_is_provisioned());
    TEST_ASSERT_EQUAL(ESP_OK, wifi_mgr_deinit());
}

TEST_CASE("heap usage does not grow across start/stop cycle", "[lifecycle]")
{
    /* Heap check: delta from before init to after deinit must be ~0.
     * Some small variation is acceptable due to log buffers etc. */
    size_t heap_before = esp_get_free_heap_size();
    init_and_start();
    stop_and_deinit();
    size_t heap_after = esp_get_free_heap_size();

    /* Allow up to 512 bytes variance for log/system overhead */
    int32_t delta = (int32_t)heap_before - (int32_t)heap_after;
    ESP_LOGI(TAG, "Heap delta: %"PRId32" bytes (before=%zu, after=%zu)",
             delta, heap_before, heap_after);
    TEST_ASSERT_LESS_THAN(512, delta > 0 ? delta : -delta);
}
