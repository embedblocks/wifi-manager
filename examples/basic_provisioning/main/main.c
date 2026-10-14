/* main.c — basic_provisioning example
 *
 * Demonstrates the full wifi_manager boot sequence:
 *
 *   1. NVS / netif / event-loop init (application's responsibility)
 *   2. Register a wifi_manager event handler to log state transitions
 *   3. wifi_mgr_init() + wifi_mgr_start()
 *   4. Block in a network task until connected, then do work
 *   5. A Wi-Fi setup button wired to wifi_mgr_reboot_into_setup()
 *
 * Build and flash:
 *   idf.py -C examples/basic_provisioning build flash monitor
 *
 * First-boot provisioning:
 *   Install "ESP BLE Prov" app (Android/iOS). The device advertises as
 *   "PROV_<last3MAC>". Follow the app wizard to enter WiFi credentials.
 *   On subsequent boots the device reconnects automatically.
 *
 * Adding a network later (moved, new router, changed password):
 *   A device that already holds credentials never enters setup mode by
 *   itself. Hold the Wi-Fi setup button (default: the board's BOOT button)
 *   for 3 s and release to reboot into setup mode, keeping stored networks;
 *   hold 10 s and release to forget all networks first. The component has
 *   no GPIO code — this button is application code; copy and adapt it, or
 *   wire wifi_mgr_reboot_into_setup() to any other trigger you have.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "wifi_manager.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "sdkconfig.h"

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
        /* Since 0.3.0 this no longer implies provisioning is starting:
         * under the default auto_provision policy a device that already
         * has credentials just keeps scanning. Watch PROVISIONING_STARTED. */
        ESP_LOGW(TAG, "[wifi] Time window expired — no stored network visible");
        break;

    case WIFI_MGR_EVENT_KNOWN_AP_UNREACHABLE: {
        /* A stored network is in range but every attempt failed — often a
         * changed router password. The component never re-provisions on
         * its own for this (a look-alike AP causes the same failures);
         * a real product would prompt the user here and call
         * wifi_mgr_start_ble_provisioning() on their confirmation. */
        wifi_mgr_known_ap_unreachable_t *u = data;
        ESP_LOGW(TAG, "[wifi] '%s' visible but unreachable (%u attempts, last reason %u)",
                 u->ssid, u->attempts, u->last_reason);
        break;
    }

    case WIFI_MGR_EVENT_PROVISIONING_STARTED:
        ESP_LOGI(TAG, "[wifi] BLE provisioning active — waiting for credentials");
        ESP_LOGI(TAG, "[wifi] Open 'ESP BLE Prov' app and follow the wizard");
        break;

    case WIFI_MGR_EVENT_PROVISIONING_DONE:
        ESP_LOGI(TAG, "[wifi] Provisioning complete");

        /* NOTE: BT memory is already released automatically at this point.
         * wifi_mgr_prov.c configures network_prov_mgr_init() with
         * NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM, which frees both
         * classic BT and BLE memory as soon as provisioning completes —
         * no action needed here for the common case.
         *
         * If your application needs to keep classic Bluetooth running
         * after WiFi provisioning (rare), change that handler constant in
         * wifi_mgr_prov.c to NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE
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

/* -------------------------------------------------------------------------
 * Wi-Fi setup button (application code, not part of the component)
 *
 * Uses espressif/button (example dependency only; wifi_manager has no GPIO
 * code). BUTTON_LONG_PRESS_UP with several press_time thresholds calls only
 * the callback for the longest threshold reached, on release — so the 3 s
 * action can never fire before the user reaches 10 s. BUTTON_LONG_PRESS_START
 * with the same thresholds fires while still held: used here to tell the
 * user which action releasing will take (a product would blink an LED).
 *
 * espressif/button runs its callbacks in the esp_timer task, which must
 * stay short. Forget-all writes NVS before rebooting, so the callbacks only
 * hand the action to setup_action_task.
 *
 * Note for products using the BOOT button: holding it while powering up or
 * resetting enters the ROM download mode instead of the application. That
 * is fine after boot; say so in the user manual.
 * ------------------------------------------------------------------------- */

#if CONFIG_EXAMPLE_FORGET_HOLD_MS > 0 && \
    CONFIG_EXAMPLE_FORGET_HOLD_MS <= CONFIG_EXAMPLE_SETUP_HOLD_MS
#error "EXAMPLE_FORGET_HOLD_MS must be longer than EXAMPLE_SETUP_HOLD_MS (or 0 to disable)"
#endif

enum { ACTION_SETUP = 1, ACTION_FORGET_ALL = 2 };
static TaskHandle_t s_setup_action_task;

static void setup_action_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t action = 0;
        xTaskNotifyWait(0, UINT32_MAX, &action, portMAX_DELAY);
        if (action == ACTION_FORGET_ALL) {
            esp_err_t err = wifi_mgr_forget_all_and_reboot_into_setup();
            /* Only returns on failure (networks NOT forgotten). */
            ESP_LOGE(TAG, "[button] forget-all failed: %s", esp_err_to_name(err));
        } else if (action == ACTION_SETUP) {
            wifi_mgr_reboot_into_setup(); /* does not return */
        }
    }
}

/* Callbacks: esp_timer task context — keep them short. */
static void on_release_setup(void *btn, void *ctx)
{
    xTaskNotify(s_setup_action_task, ACTION_SETUP, eSetValueWithOverwrite);
}
static void on_hold_setup(void *btn, void *ctx)
{
    ESP_LOGW(TAG, "[button] release now: Wi-Fi setup (networks kept)");
}
#if CONFIG_EXAMPLE_FORGET_HOLD_MS > 0
static void on_release_forget(void *btn, void *ctx)
{
    xTaskNotify(s_setup_action_task, ACTION_FORGET_ALL, eSetValueWithOverwrite);
}
static void on_hold_forget(void *btn, void *ctx)
{
    ESP_LOGW(TAG, "[button] release now: FORGET ALL networks, then setup");
}
#endif

static void setup_button_init(void)
{
    xTaskCreate(setup_action_task, "setup_act", 3072, NULL,
                tskIDLE_PRIORITY + 2, &s_setup_action_task);

    const button_config_t btn_cfg = {
        /* Long-press detection starts at the first threshold. */
        .long_press_time = CONFIG_EXAMPLE_SETUP_HOLD_MS,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num     = CONFIG_EXAMPLE_SETUP_BUTTON_GPIO,
        .active_level = CONFIG_EXAMPLE_SETUP_BUTTON_ACTIVE_LOW ? 0 : 1,
    };
    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));

    /* Register thresholds in ascending press_time order: the library walks
     * them in registration order. */
    button_event_args_t setup_args = {
        .long_press.press_time = CONFIG_EXAMPLE_SETUP_HOLD_MS,
    };
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START,
                                           &setup_args, on_hold_setup, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_UP,
                                           &setup_args, on_release_setup, NULL));
#if CONFIG_EXAMPLE_FORGET_HOLD_MS > 0
    button_event_args_t forget_args = {
        .long_press.press_time = CONFIG_EXAMPLE_FORGET_HOLD_MS,
    };
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START,
                                           &forget_args, on_hold_forget, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_UP,
                                           &forget_args, on_release_forget, NULL));
#endif

    ESP_LOGI(TAG, "Wi-Fi setup button on GPIO %d: hold %d ms = setup, "
                  "%d ms = forget all (0 = off)",
             CONFIG_EXAMPLE_SETUP_BUTTON_GPIO, CONFIG_EXAMPLE_SETUP_HOLD_MS,
             CONFIG_EXAMPLE_FORGET_HOLD_MS);
}

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

    /* 2. Network interface layer + default event loop.
     * IMPORTANT: do NOT call esp_netif_create_default_wifi_sta() here —
     * wifi_mgr_start() creates and owns the STA netif. Calling it here
     * too would create a duplicate netif, which causes DHCP to fail
     * silently (connected at layer 2 but IP_EVENT_STA_GOT_IP never fires). */
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

    /* 6. Wi-Fi setup button — the application's trigger for adding a
     *    network once credentials are stored */
    setup_button_init();

    /* 7. Spawn network task — it blocks until connected, then does work */
    xTaskCreate(network_task, "net_task",
                4096, NULL,
                tskIDLE_PRIORITY + 1, NULL);

    /* app_main returns here — all subsequent work is in tasks */
}
