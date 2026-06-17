/**
 * @file wifi_mgr_prov.c
 * @brief BLE provisioning sub-module implementation for wifi_manager.
 *
 * See 08_provisioning_flow.md and implementation plan Phase 5 for details.
 *
 * Race condition fix (from 08_provisioning_flow.md):
 * WIFI_PROV_END is a no-op. All deinit happens via prov_mgr_safe_deinit()
 * called from exactly three call sites: on_prov_done(), on_prov_failed(),
 * and wifi_mgr_prov_stop(). Calling deinit from PROV_END handler causes
 * a double-free race when PROV_END fires before CRED_SUCCESS is processed.
 */

#include "wifi_mgr_prov.h"
#include "wifi_mgr_ctx.h"
#include "wifi_mgr_state.h"
#include "wifi_mgr_events.h"
#include "ap_records.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "wifi_mgr";

/* Catch an oversized static PoP at compile time rather than silently
 * truncating it at runtime (strncpy would otherwise cut it to fit, and
 * the device would advertise a value that doesn't match what the
 * operator typed into the provisioning app, with no way to tell why
 * provisioning is failing). Always checked, even when MAC_POP or OPEN
 * mode is active, since CONFIG_WIFI_PROV_POP is always #define'd (see
 * its Kconfig help text) and switching modes shouldn't surface a
 * latent length bug only when STATIC_POP is later enabled. */
_Static_assert(
    sizeof(CONFIG_WIFI_PROV_POP) - 1 < WIFI_MGR_PROV_POP_BUF_SIZE,
    "CONFIG_WIFI_PROV_POP is too long and would be silently truncated. "
    "Shorten the string in sdkconfig.defaults / menuconfig, or raise "
    "WIFI_MGR_PROV_POP_BUF_SIZE in wifi_mgr_ctx.h to match."
);

/* ── prov_mgr_safe_deinit ────────────────────────────────────────────────── */

/* Forward declaration for use in prov event handler */
static void prov_mgr_safe_deinit(void);

/* ── Provisioning event handler ──────────────────────────────────────────── */

static void wifi_mgr_prov_event_handler(void *arg,
                                         esp_event_base_t event_base,
                                         int32_t event_id,
                                         void *event_data)
{
    switch ((wifi_prov_cb_event_t)event_id) {

    case WIFI_PROV_CRED_RECV: {
        /* Credentials received from the provisioning app.
         * Add to ap_records in RAM; defer NVS save to on_prov_done()
         * which runs in the manager task context (not the system event task).
         * The system event task must not block on NVS writes (~10-20ms). */
        wifi_sta_config_t *sta = (wifi_sta_config_t *)event_data;
        esp_err_t ret = ap_records_add((const char *)sta->ssid,
                                       (const char *)sta->password,
                                       NULL);
        if (ret == ESP_OK) {
            CTX->pending_nvs_save = true;
            ESP_LOGI(TAG, "Credentials received for SSID: %s (NVS save deferred)",
                     sta->ssid);
        } else {
            ESP_LOGE(TAG, "ap_records_add failed: %s", esp_err_to_name(ret));
        }
        break;
    }

    case WIFI_PROV_CRED_SUCCESS: {
        /* wifi_prov_mgr has connected to the AP and obtained an IP.
         * Pre-fill conn_info before xTaskNotify to satisfy the
         * happens-before contract on dual-core (05_internal_architecture.md). */
        wifi_ap_record_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        esp_wifi_sta_get_ap_info(&ap_info);

        /* conn_info.ip will be populated from IP_EVENT_STA_GOT_IP,
         * but that event may arrive before or after CRED_SUCCESS.
         * Use whatever is already in CTX->conn_info.ip (set by the
         * IP event handler). Only overwrite SSID/BSSID here. */
        memcpy(CTX->conn_info.ssid, ap_info.ssid, sizeof(CTX->conn_info.ssid));
        memcpy(CTX->conn_info.bssid, ap_info.bssid, 6);

        xTimerStop(CTX->prov_timer, 0);
        xTaskNotify(CTX->task_handle, NOTIFY_PROV_DONE, eSetBits);
        ESP_LOGI(TAG, "Provisioning credentials accepted; IP obtained");
        break;
    }

    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "Provisioning credential failure, reason: %s",
                 (*reason == WIFI_PROV_STA_AUTH_ERROR)
                     ? "Auth error (wrong password?)"
                     : "AP not found");
        xTaskNotify(CTX->task_handle, NOTIFY_PROV_FAILED, eSetBits);
        break;
    }

    case WIFI_PROV_END:
        /* Intentional no-op. All deinit is via prov_mgr_safe_deinit()
         * from on_prov_done() / on_prov_failed() / wifi_mgr_prov_stop().
         * Calling deinit here would race with CRED_SUCCESS processing. */
        ESP_LOGD(TAG, "WIFI_PROV_END received (no-op)");
        break;

    default:
        break;
    }
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * @brief Idempotent provisioning manager cleanup.
 *
 * Guards against double-deinit via CTX->prov_mgr_initialized.
 * Called from exactly three sites: on_prov_done(), on_prov_failed(),
 * and wifi_mgr_prov_stop().
 */
static void prov_mgr_safe_deinit(void)
{
    if (!CTX->prov_mgr_initialized) {
        return;
    }
    CTX->prov_mgr_initialized = false;
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                 wifi_mgr_prov_event_handler);
    wifi_prov_mgr_deinit();
    ESP_LOGD(TAG, "prov_mgr deinitialized");
}

/* ── Public functions ────────────────────────────────────────────────────── */

esp_err_t wifi_mgr_prov_start(void)
{
    /* Read the stable eFuse MAC — use esp_read_mac() not esp_wifi_get_mac()
     * to bypass MAC randomization and always get the hardware address. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Build device name: prefix + last 3 MAC bytes as hex */
    const char *prefix = (CTX->config.device_name_prefix[0] != '\0')
                       ? CTX->config.device_name_prefix
                       : CONFIG_WIFI_PROV_DEVICE_NAME_PREFIX;
    snprintf(CTX->prov_device_name, sizeof(CTX->prov_device_name),
             "%s%02X%02X%02X", prefix, mac[3], mac[4], mac[5]);

    /* Resolve auth mode and PoP. Branches are compile-time (#if, not
     * switch) since CONFIG_WIFI_PROV_AUTH_MODE is now a Kconfig choice —
     * each option is its own bool symbol rather than one int value. */
    wifi_prov_security_t security;
    const char *pop = NULL;
    const char *auth_str;
    bool log_pop = false;

#if defined(CONFIG_WIFI_PROV_AUTH_MODE_OPEN)
    security = WIFI_PROV_SECURITY_0;
    CTX->prov_pop[0] = '\0';
    auth_str = "Open (no security)";

#elif defined(CONFIG_WIFI_PROV_AUTH_MODE_STATIC_POP)
    security = WIFI_PROV_SECURITY_1;
    if (strlen(CONFIG_WIFI_PROV_POP) >= sizeof(CTX->prov_pop)) {
        /* Should be unreachable — the _Static_assert above this function
         * already catches this at compile time. Kept as defense in depth
         * in case that guard is ever weakened independently of this
         * runtime path in a future change. */
        ESP_LOGE(TAG, "CONFIG_WIFI_PROV_POP (%d chars) exceeds buffer "
                       "(%d chars) and would be truncated — refusing to "
                       "start with a corrupted PoP. Shorten the string.",
                 (int)strlen(CONFIG_WIFI_PROV_POP), (int)sizeof(CTX->prov_pop) - 1);
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        wifi_mgr_error_info_t err_info = {
            .code               = ESP_ERR_INVALID_SIZE,
            .state              = WIFI_MGR_STATE_PROVISIONING,
            .consecutive_errors = ++CTX->consecutive_errors,
        };
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));
        return ESP_ERR_INVALID_SIZE;
    }
    strncpy(CTX->prov_pop, CONFIG_WIFI_PROV_POP, sizeof(CTX->prov_pop) - 1);
    CTX->prov_pop[sizeof(CTX->prov_pop) - 1] = '\0';
    pop = CTX->prov_pop;
    auth_str = "SRP6a / static";
    /* Not logged by default: the integrator already set this value in
     * their own sdkconfig.defaults (that's the point of static mode for
     * headless devices), so printing it to the console adds nothing and
     * is needless exposure if logs are ever captured/uploaded. Flip this
     * to true if you do want it echoed back for local bench debugging. */
    log_pop = false;

#else /* CONFIG_WIFI_PROV_AUTH_MODE_MAC_POP (default) */
    security = WIFI_PROV_SECURITY_1;
    snprintf(CTX->prov_pop, sizeof(CTX->prov_pop),
             "%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
    pop = CTX->prov_pop;
    auth_str = "SRP6a / MAC-derived";
    /* Must be logged: this is the only way the operator can learn a
     * value that's unique per device and not known ahead of time. */
    log_pop = true;
#endif

    /* Log to console — this is the component's only output responsibility.
     * The operator reads this and enters the PoP into the provisioning
     * app, or generates a QR code from it externally (e.g.
     * https://espressif.github.io/esp-jumpstart/qrcode.html). */
    ESP_LOGI(TAG, "-----------------------------------------");
    ESP_LOGI(TAG, "BLE Provisioning started");
    ESP_LOGI(TAG, "  Device Name : %s", CTX->prov_device_name);
    ESP_LOGI(TAG, "  Auth Mode   : %s", auth_str);
    if (security == WIFI_PROV_SECURITY_1) {
        if (log_pop) {
            ESP_LOGI(TAG, "  PoP         : %s", pop);
        } else {
            ESP_LOGI(TAG, "  PoP         : (static — set via CONFIG_WIFI_PROV_POP)");
        }
    }
    ESP_LOGI(TAG, "  Transport   : BLE");
    ESP_LOGI(TAG, "-----------------------------------------");

    /* Initialize provisioning manager */
    wifi_prov_mgr_config_t prov_cfg = {
        .scheme               = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };

    esp_err_t ret = wifi_prov_mgr_init(prov_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(ret));
        /* Recover to SCANNING */
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        wifi_mgr_error_info_t err_info = {
            .code               = ret,
            .state              = WIFI_MGR_STATE_PROVISIONING,
            .consecutive_errors = ++CTX->consecutive_errors,
        };
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));
        return ret;
    }

    /* Register prov event handler */
    ret = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_mgr_prov_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register prov event handler: %s",
                 esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        return ret;
    }

    /* Start provisioning */
    ret = wifi_prov_mgr_start_provisioning(security, pop,
                                           CTX->prov_device_name, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s",
                 esp_err_to_name(ret));
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_mgr_prov_event_handler);
        wifi_prov_mgr_deinit();
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        wifi_mgr_error_info_t err_info = {
            .code               = ret,
            .state              = WIFI_MGR_STATE_PROVISIONING,
            .consecutive_errors = ++CTX->consecutive_errors,
        };
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));
        return ret;
    }

    CTX->prov_mgr_initialized = true;

    /* Start prov timeout timer (only if timeout > 0) */
    if (CTX->config.prov_timeout_ms > 0) {
        xTimerStart(CTX->prov_timer, 0);
    }

    wifi_mgr_events_post(WIFI_MGR_EVENT_PROVISIONING_STARTED, NULL, 0);
    ESP_LOGI(TAG, "BLE provisioning active (timeout: %"PRIu32" ms)",
             CTX->config.prov_timeout_ms);
    return ESP_OK;
}

void wifi_mgr_prov_stop(void)
{
    xTimerStop(CTX->prov_timer, 0);
    wifi_prov_mgr_stop_provisioning();
    prov_mgr_safe_deinit();
    ESP_LOGD(TAG, "Provisioning stopped");
}
