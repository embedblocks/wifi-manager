/* main.c — basic_provisioning example
 *
 * Demonstrates the full wifi_manager boot sequence:
 *
 *   1. NVS / netif / event-loop init (application's responsibility)
 *   2. Register a wifi_manager event handler to log state transitions
 *   3. wifi_mgr_init() + wifi_mgr_start()
 *   4. Block in a network task until connected, then do work
 *   5. Optional: release BT controller after provisioning (provision-once)
 *
 * Build and flash:
 *   idf.py -C examples/basic_provisioning build flash monitor
 *
 * First-boot provisioning:
 *   Install "ESP BLE Prov" app (Android/iOS). The device advertises as
 *   "PROV_<last3MAC>". Follow the app wizard to enter WiFi credentials.
 *   On subsequent boots the device reconnects automatically.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "EXAMPLE";

/* -------------------------------------------------------------------------
 * WiFi manager event handler
 * ------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    switch ((wifi_mgr_event_id_t)id) {

    case WIFI_MGR_EVENT_STARTED:
        ESP_LOGI(TAG, "[wifi] Started — scanning for APs");
        break;

    case WIFI_MGR_EVENT_SCANNING:
        ESP_LOGI(TAG, "[wifi] Scanning (no live APs visible)");
        break;

    case WIFI_MGR_EVENT_LIVE_APS_FOUND: {
        wifi_mgr_scan_result_t *r = data;
        ESP_LOGI(TAG, "[wifi] %d live AP(s) visible, %d known — tw timer running",
                 r->total_live_aps, r->known_aps_visible);
        break;
    }

    case WIFI_MGR_EVENT_CONNECTING: {
        wifi_mgr_connect_info_t *c = data;
        ESP_LOGI(TAG, "[wifi] Connecting to '%s' (AP #%d in connect list)",
                 c->ssid, c->ap_index);
        break;
    }

    case WIFI_MGR_EVENT_CONNECTED: {
        wifi_mgr_connected_info_t *info = data;
        ESP_LOGI(TAG, "[wifi] Connected: SSID='%s'  IP=" IPSTR "  channel=%d",
                 info->ssid, IP2STR((ip4_addr_t *)&info->ip), info->channel);
        break;
    }

    case WIFI_MGR_EVENT_DISCONNECTED: {
        wifi_mgr_disconnected_info_t *d = data;
        ESP_LOGW(TAG, "[wifi] Disconnected from '%s' (reason %d)",
                 d->ssid, d->reason);
        break;
    }

    case WIFI_MGR_EVENT_NO_KNOWN_AP_IN_TW:
        ESP_LOGW(TAG, "[wifi] Time window expired — starting BLE provisioning");
        ESP_LOGW(TAG, "[wifi] Open 'ESP BLE Prov' app and follow the wizard");
        break;

    case WIFI_MGR_EVENT_PROVISIONING_STARTED:
        ESP_LOGI(TAG, "[wifi] BLE provisioning active — waiting for credentials");
        break;

    case WIFI_MGR_EVENT_PROVISIONING_DONE:
        ESP_LOGI(TAG, "[wifi] Provisioning complete");

        /* NOTE: BT memory is already released automatically at this point.
         * wifi_mgr_prov.c configures wifi_prov_mgr_init() with
         * WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM, which frees both
         * classic BT and BLE memory as soon as provisioning completes —
         * no action needed here for the common case.
         *
         * If your application needs to keep classic Bluetooth running
         * after WiFi provisioning (rare), change that handler constant in
         * wifi_mgr_prov.c to WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE
         * instead, which frees only the BLE memory. See L8 in
         * 01_overview.md for the full trade-off discussion. */
        break;

    case WIFI_MGR_EVENT_PROVISIONING_FAILED:
        ESP_LOGE(TAG, "[wifi] Provisioning failed or timed out");
        break;

    case WIFI_MGR_EVENT_ERROR: {
        wifi_mgr_error_info_t *e = data;
        ESP_LOGE(TAG, "[wifi] Error in state '%s': %s (consecutive: %d)",
                 wifi_mgr_state_str(e->state),
                 esp_err_to_name(e->code),
                 e->consecutive_errors);
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Network task — blocks until connected, then does application work
 * ------------------------------------------------------------------------- */

static void network_task(void *arg)
{
    ESP_LOGI(TAG, "Network task: waiting for WiFi connection...");

    /* Pattern D from 09_event_system.md: reconnect loop */
    while (true) {
        esp_err_t ret = wifi_mgr_wait_for_connection(0); /* block forever */
        if (ret == ESP_ERR_WIFI_MGR_CANCELED) {
            ESP_LOGI(TAG, "Network task: wait cancelled — exiting");
            break;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Network task: wait returned %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Network task: connected — doing application work");

        /* ----------------------------------------------------------------
         * Application work goes here: MQTT publish, HTTP request, OTA, …
         * This loop runs until the connection drops.
         * ---------------------------------------------------------------- */
        while (wifi_mgr_is_connected()) {
            ESP_LOGI(TAG, "Network task: heartbeat (connected)");
            vTaskDelay(pdMS_TO_TICKS(10000));
        }

        ESP_LOGW(TAG, "Network task: connection lost — re-waiting");
        /* Loop back — wifi_mgr_wait_for_connection() blocks until the
         * manager reconnects or provisioning succeeds. */
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * app_main
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "basic_provisioning example starting");

    /* 1. NVS — must be done by the application before wifi_mgr_init() */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network interface + default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Register wifi_manager event handler */
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_MGR_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL));

    /* 4. Initialise wifi_manager with default config.
     * Per wifi_mgr_config_t's doc comment: zero-initializing means every
     * field falls back to its Kconfig default. */
    wifi_mgr_config_t config = {0};
    /* Optionally override defaults:
     *   config.known_ap_wait_ms = 60000;   // shorter tw for this product
     *   config.max_connect_attempts = 2;
     */
    ESP_ERROR_CHECK(wifi_mgr_init(&config));

    /* 5. Start — launches the manager task and begins scanning */
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_LOGI(TAG, "WiFi manager started");

    /* 6. Spawn network task — it blocks until connected, then does work */
    xTaskCreate(network_task, "net_task",
                4096, NULL,
                tskIDLE_PRIORITY + 1, NULL);

    /* app_main returns here — all subsequent work is in tasks */
}
