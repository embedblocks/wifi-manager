/**
 * @file wifi_mgr_prov.c
 * @brief BLE provisioning sub-module implementation for wifi_manager.
 *
 * See 08_provisioning_flow.md and implementation plan Phase 5 for details.
 *
 * Race condition fix #1 (from 08_provisioning_flow.md):
 * NETWORK_PROV_END is mostly a no-op (see fix #3 below for the one thing it
 * now does). Deinit happens only via prov_mgr_safe_deinit(), which is
 * called from exactly two sites: wifi_mgr_prov_stop_succeeded()
 * (conditionally, under CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS) and
 * wifi_mgr_prov_stop_shutdown() (always, unconditionally). It is
 * deliberately NOT called from wifi_mgr_prov_stop_failed_attempt() — see
 * fix #2 below. Calling deinit from the PROV_END handler would race when
 * PROV_END fires before CRED_SUCCESS is processed.
 *
 * Fix #2 (BT controller cannot be reinitialized after deinit — the actual
 * root cause of the original "BLE_INIT: controller init failed" field
 * bug): esp_bt_controller_init() is documented by Espressif as
 * unsupported after a prior esp_bt_controller_deinit() in the same boot.
 * network_prov_mgr_deinit() triggers exactly that deinit as a side effect of
 * the NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM scheme handler used
 * here. Once that has happened, no amount of waiting or retrying makes a
 * later network_prov_mgr_init() succeed — it is a hard wall, not a
 * transient race against an async teardown task.
 *
 * The fix is to avoid the deinit/reinit cycle entirely on the
 * failure-retry path: wifi_mgr_prov_stop_failed_attempt() never
 * deinitializes, so wifi_mgr_prov_start()'s retry-in-place branch can
 * restart the existing session directly, indefinitely, without ever
 * exercising esp_bt_controller_deinit()/init() again. See
 * CTX->bt_mem_freed and CONFIG_WIFI_PROV_BT_LIFECYCLE in Kconfig for how
 * this interacts with the *success* path, where deinit (to free BT
 * memory) is sometimes still desired despite the one-way cost.
 *
 * Fix #3 (real-hardware-discovered: network_prov_mgr_stop_provisioning()
 * does NOT wait for BLE teardown before returning, despite running in
 * its default "blocking" mode — network_prov_mgr_disable_auto_stop() is
 * never called here, which only means stop_provisioning() doesn't merely
 * schedule a delayed cleanup timer and return immediately; it does NOT
 * mean the call waits for the internal prov_stop_task() FreeRTOS task to
 * finish tearing down NimBLE GAP advertising/connections before
 * returning. This was the original async-teardown diagnosis from this
 * investigation's first pass — disproven at the time based on a
 * misreading of "blocking" vs. "non-blocking" mode in the official docs,
 * then *reconfirmed as correct after all* by a real device log showing
 * `network_prov_mgr: Provisioning service already started` /
 * `ESP_ERR_INVALID_STATE` on the very first retry-in-place attempt after
 * every failed/timed-out session, with the actual
 * `network_prov_mgr: Provisioning stopped` confirmation arriving roughly 1
 * second later. The earlier retry-storm backoff guard in on_prov_failed()
 * happened to paper over this by coincidence (its backoff duration
 * exceeded the observed ~1s teardown time), which is why the system
 * still worked but only by accident, not because the race was actually
 * closed.):
 *
 * wifi_mgr_prov_stop_failed_attempt() now waits — with a bound, not the
 * unbounded network_prov_mgr_wait() — for the NETWORK_PROV_END event to
 * actually fire before returning, using a binary semaphore
 * (CTX->prov_stop_confirm_sem, see wifi_mgr_ctx.h) given from the
 * NETWORK_PROV_END handler below. A semaphore (rather than a task
 * notification bit) is used because all three wifi_mgr_prov_stop_*()
 * functions share this wait, and wifi_mgr_prov_stop_shutdown() is called
 * from wifi_mgr_stop()'s caller context, not the manager task — a task
 * notification is only receivable by the specific task it's posted to,
 * but a semaphore give/take pair works from any task context. Giving
 * the semaphore from NETWORK_PROV_END is safe and does not reopen the race
 * fix #1 addresses: fix #1 is specifically about not calling
 * prov_mgr_safe_deinit() (which mutates shared teardown state) from this
 * handler; giving a semaphore mutates nothing in network_prov_mgr's
 * internal state and has no interaction with CRED_SUCCESS processing.
 * If the bound expires with no NETWORK_PROV_END seen, wifi_mgr_prov_start()'s
 * retry-in-place branch may still race the teardown and get
 * ESP_ERR_INVALID_STATE — but this is now a rare residual case the
 * existing backoff-and-retry-once guard in on_prov_failed() already
 * handles correctly, rather than the common case it was accidentally
 * covering for before.
 *
 * ESP-IDF v6.0 port: wifi_provisioning was removed from ESP-IDF v6.0 and
 * replaced by the espressif/network_provisioning registry component. The
 * API is a 1:1 rename (wifi_prov_* -> network_prov_*, WIFI_PROV_CRED_* ->
 * NETWORK_PROV_WIFI_CRED_*, WIFI_PROV_END -> NETWORK_PROV_END) and the
 * lifecycle semantics the fixes above depend on are unchanged. Its
 * header now states outright that network_prov_mgr_stop_provisioning()
 * "will initiate a process to stop the service and return", with
 * NETWORK_PROV_END emitted once the service actually stops — which is
 * exactly the async behaviour fix #3 was written to handle.
 */

#include "wifi_mgr_prov.h"
#include "wifi_mgr_ctx.h"
#include "wifi_mgr_state.h"
#include "wifi_mgr_events.h"
#include "ap_records.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "wifi_mgr";

/* ESP-IDF v6.0: protocomm security versions 0 and 1 default to disabled.
 * Kconfig selects the right one from CONFIG_WIFI_PROV_AUTH_MODE_*, but if
 * that select is ever bypassed (e.g. a hand-edited sdkconfig), fail the
 * build here instead of letting network_prov_mgr_start_provisioning()
 * return ESP_ERR_NOT_SUPPORTED at runtime with no log output. */
#if defined(CONFIG_WIFI_PROV_AUTH_MODE_OPEN) && \
    !defined(CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0)
#error "WIFI_PROV_AUTH_MODE_OPEN requires CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0=y (off by default in ESP-IDF v6.0)"
#endif
#if (defined(CONFIG_WIFI_PROV_AUTH_MODE_STATIC_POP) || defined(CONFIG_WIFI_PROV_AUTH_MODE_MAC_POP)) && \
    !defined(CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1)
#error "PoP-based provisioning requires CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y (off by default in ESP-IDF v6.0)"
#endif

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
    switch ((network_prov_cb_event_t)event_id) {

    case NETWORK_PROV_WIFI_CRED_RECV: {
        /* Credentials received from the provisioning app, NOT yet verified.
         * Hold them in RAM only. They reach ap_records in on_prov_done(),
         * after NETWORK_PROV_WIFI_CRED_SUCCESS proves they work. Adding them
         * here (0.1.x/0.2.x behaviour) let anyone in BLE range plant a
         * credential at use_count = max + 1 — top priority, evicting a real
         * one when the store was full — whether or not it ever connected.
         *
         * wifi_sta_config_t.ssid / .password are fixed-size and need not be
         * NUL-terminated at full length; copy bounded and terminate. */
        wifi_sta_config_t *sta = (wifi_sta_config_t *)event_data;
        memset(CTX->held_ssid, 0, sizeof(CTX->held_ssid));
        memset(CTX->held_password, 0, sizeof(CTX->held_password));
        memset(CTX->held_bssid, 0, sizeof(CTX->held_bssid));
        memcpy(CTX->held_ssid, sta->ssid,
               sizeof(sta->ssid) < sizeof(CTX->held_ssid) - 1
                   ? sizeof(sta->ssid) : sizeof(CTX->held_ssid) - 1);
        memcpy(CTX->held_password, sta->password,
               sizeof(sta->password) < sizeof(CTX->held_password) - 1
                   ? sizeof(sta->password) : sizeof(CTX->held_password) - 1);
        CTX->held_confirmed = false;
        CTX->held_valid     = (CTX->held_ssid[0] != '\0');
        ESP_LOGI(TAG, "Credentials received for SSID: %s (held until connection "
                      "is confirmed)", CTX->held_ssid);
        break;
    }

    case NETWORK_PROV_WIFI_CRED_SUCCESS: {
        /* network_prov_mgr has connected to the AP and obtained an IP.
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

        /* Mark the held credential as confirmed. Record the BSSID only when
         * the AP we ended up on carries the SSID we were given — otherwise
         * store the credential without a BSSID rather than a wrong one. */
        if (CTX->held_valid &&
            strncmp((const char *)ap_info.ssid, CTX->held_ssid,
                    sizeof(CTX->held_ssid)) == 0) {
            memcpy(CTX->held_bssid, ap_info.bssid, sizeof(CTX->held_bssid));
        } else if (CTX->held_valid) {
            ESP_LOGW(TAG, "Connected SSID '%.32s' differs from provisioned "
                          "SSID '%s' — saving without BSSID",
                     (const char *)ap_info.ssid, CTX->held_ssid);
        }
        CTX->held_confirmed = CTX->held_valid;

        xTimerStop(CTX->prov_timer, 0);
        xTaskNotify(CTX->task_handle, NOTIFY_PROV_DONE, eSetBits);
        ESP_LOGI(TAG, "Provisioning credentials accepted; IP obtained");
        break;
    }

    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "Provisioning credential failure, reason: %s",
                 (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR)
                     ? "Auth error (wrong password?)"
                     : "AP not found");
        xTaskNotify(CTX->task_handle, NOTIFY_PROV_FAILED, eSetBits);
        break;
    }

    case NETWORK_PROV_END:
        /* No deinit here — see fix #1 in the file header. Giving this
         * semaphore is safe and unrelated to that guarantee: it mutates
         * no network_prov_mgr-internal state and has no interaction with
         * CRED_SUCCESS processing, unlike prov_mgr_safe_deinit() would.
         * A semaphore (not a task notification) is used because all
         * three wifi_mgr_prov_stop_*() functions consume it, and one of
         * them (wifi_mgr_prov_stop_shutdown()) runs in wifi_mgr_stop()'s
         * caller context, not the manager task — a task notification
         * only works for the specific task it's posted to, but a
         * semaphore give/take pair works regardless of which task is on
         * either end. Binary semaphore: this give is a no-op if it's
         * already given (e.g. two NETWORK_PROV_END events in a row with no
         * intervening take — cannot happen with the current call
         * pattern, but xSemaphoreGive() simply returns pdFALSE in that
         * case rather than corrupting state, since the count is capped
         * at 1). See CTX->prov_stop_confirm_sem in wifi_mgr_ctx.h. */
        xSemaphoreGive(CTX->prov_stop_confirm_sem);
        ESP_LOGD(TAG, "NETWORK_PROV_END received");
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
 * Called from exactly two sites: wifi_mgr_prov_stop_succeeded()
 * (conditionally) and wifi_mgr_prov_stop_shutdown() (always).
 * Deliberately NOT called from wifi_mgr_prov_stop_failed_attempt() —
 * see file header comment, fix #2.
 */
static void prov_mgr_safe_deinit(void)
{
    if (!CTX->prov_mgr_initialized) {
        return;
    }
    CTX->prov_mgr_initialized = false;
    esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                 wifi_mgr_prov_event_handler);
    network_prov_mgr_deinit();
    ESP_LOGD(TAG, "prov_mgr deinitialized");
}

/* ── Driver credential hygiene ───────────────────────────────────────────── */

/*
 * network_provisioning undermines WIFI_STORAGE_RAM, which wifi_mgr_start()
 * relies on to keep ap_records the only credential store:
 *   - every network_prov_mgr_start_provisioning() call leaves the driver in
 *     WIFI_STORAGE_FLASH on exit, unconditionally (manager.c, "Restore WiFi
 *     storage back to FLASH" — the IDF Wi-Fi API has no storage getter, so
 *     it cannot restore the previous mode);
 *   - on NETWORK_PROV_WIFI_CRED_RECV it switches to FLASH and calls
 *     esp_wifi_set_config() with the received, unverified credentials,
 *     which writes them into the driver's own NVS namespace (nvs.net80211).
 * Left alone, every later wifi_mgr_connect_attempt() then writes to flash
 * whenever the config changes, and the driver's NVS keeps a password that
 * wifi_mgr_remove_ap() / wifi_mgr_reset_provisioning() never erase.
 *
 * Compensation:
 *   - wifi_mgr_driver_storage_ram(): after every start_provisioning() call
 *     and at the end of every session (any outcome). Only changes where
 *     future writes go; does not touch the current config or the link.
 *   - wifi_mgr_driver_wipe_config(): only while the station is NOT
 *     connected — after a failed/timed-out session, and in wifi_mgr_start()
 *     before esp_wifi_start(). The success path is not wiped live: the
 *     stored password is the verified one, and the next start-up wipe
 *     removes it (wifi_mgr_reset_provisioning() goes through stop/start).
 */
void wifi_mgr_driver_storage_ram(void)
{
    esp_err_t ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage(RAM) failed: %s",
                 esp_err_to_name(ret));
    }
}

esp_err_t wifi_mgr_driver_wipe_config(void)
{
    wifi_config_t empty;
    memset(&empty, 0, sizeof(empty));

    esp_err_t ret = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (ret == ESP_OK) {
        ret = esp_wifi_set_config(WIFI_IF_STA, &empty);
    }
    /* Always end in RAM mode, even if the overwrite failed. */
    wifi_mgr_driver_storage_ram();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not wipe driver-stored STA config: %s "
                      "(will retry at next start)", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Driver-stored STA config wiped");
    }
    return ret;
}

void wifi_mgr_prov_wipe_held_creds(void)
{
    /* volatile pointer so the compiler cannot elide the password wipe */
    volatile char *p = CTX->held_password;
    for (size_t i = 0; i < sizeof(CTX->held_password); i++) {
        p[i] = 0;
    }
    memset(CTX->held_ssid, 0, sizeof(CTX->held_ssid));
    memset(CTX->held_bssid, 0, sizeof(CTX->held_bssid));
    CTX->held_valid     = false;
    CTX->held_confirmed = false;
}

/* ── Public functions ────────────────────────────────────────────────────── */

esp_err_t wifi_mgr_prov_start(void)
{
    /* Drain any stale give on CTX->prov_stop_confirm_sem before starting
     * a new session. This guards against a late NETWORK_PROV_END from the
     * *previous* session's teardown arriving after whichever
     * wifi_mgr_prov_stop_*() call was waiting on it already gave up
     * (timed out) — without this drain, that stale give would sit
     * available and be wrongly consumed by a future, unrelated wait on
     * this same semaphore for an entirely different teardown. Binary
     * semaphore, so a zero-timeout take is sufficient regardless of how
     * many stale gives accumulated (they all collapse to the same single
     * "available" state — see CTX->prov_stop_confirm_sem in
     * wifi_mgr_ctx.h). Safe to call unconditionally, including on the
     * very first call this boot, when the semaphore is already empty by
     * construction (xSemaphoreCreateBinary() default) and this take
     * simply returns pdFALSE immediately. */
    xSemaphoreTake(CTX->prov_stop_confirm_sem, 0);

    /* Retry-in-place: if a session already exists (survived a prior
     * failure/timeout without being deinitialized — see
     * prov_mgr_stop_failed_attempt() below), skip straight to
     * (re)starting the session rather than re-running network_prov_mgr_init().
     * This is what makes failed/timed-out attempts retry indefinitely
     * without ever touching the controller deinit/reinit cycle that
     * triggers the documented ESP-IDF wall (esp_bt_controller_init() is
     * not supported after a prior esp_bt_controller_deinit() in the same
     * boot). Device name / PoP were already set on the first call and do
     * not change between retries, so they are not recomputed here. */
    if (CTX->prov_mgr_initialized) {
        esp_err_t ret = network_prov_mgr_start_provisioning(
            CTX->prov_security, CTX->prov_pop[0] != '\0' ? CTX->prov_pop : NULL,
            CTX->prov_device_name, NULL);
        wifi_mgr_driver_storage_ram(); /* start_provisioning() exits in FLASH mode */
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "network_prov_mgr_start_provisioning (retry) failed: %s",
                     esp_err_to_name(ret));
            wifi_mgr_error_info_t err_info = {
                .code               = ret,
                .state              = WIFI_MGR_STATE_PROVISIONING,
                .consecutive_errors = ++CTX->consecutive_errors,
            };
            wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));
            /* Do NOT fall back to SCANNING — stay in PROVISIONING and let
             * the caller (on_prov_failed(), via the prov_timer or another
             * NOTIFY_PROV_FAILED) try again. The session is still alive;
             * a transient start failure here is not the BT dead end
             * (that can only be hit via network_prov_mgr_init(), below). */
            return ret;
        }
        if (CTX->config.prov_timeout_ms > 0) {
            xTimerStart(CTX->prov_timer, 0);
        }
        wifi_mgr_events_post(WIFI_MGR_EVENT_PROVISIONING_STARTED, NULL, 0);
        ESP_LOGI(TAG, "BLE provisioning restarted in place (timeout: %"PRIu32" ms)",
                 CTX->config.prov_timeout_ms);
        return ESP_OK;
    }

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
    network_prov_security_t security;
    const char *pop = NULL;
    const char *auth_str;
    bool log_pop = false;

#if defined(CONFIG_WIFI_PROV_AUTH_MODE_OPEN)
    security = NETWORK_PROV_SECURITY_0;
    CTX->prov_pop[0] = '\0';
    auth_str = "Open (no security)";

#elif defined(CONFIG_WIFI_PROV_AUTH_MODE_STATIC_POP)
    security = NETWORK_PROV_SECURITY_1;
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
    auth_str = "Security 1 (X25519 + AES-CTR) / static PoP";
    /* Not logged by default: the integrator already set this value in
     * their own sdkconfig.defaults (that's the point of static mode for
     * headless devices), so printing it to the console adds nothing and
     * is needless exposure if logs are ever captured/uploaded. Flip this
     * to true if you do want it echoed back for local bench debugging. */
    log_pop = false;

#else /* CONFIG_WIFI_PROV_AUTH_MODE_MAC_POP (default) */
    security = NETWORK_PROV_SECURITY_1;
    snprintf(CTX->prov_pop, sizeof(CTX->prov_pop),
             "%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
    pop = CTX->prov_pop;
    auth_str = "Security 1 (X25519 + AES-CTR) / MAC-derived PoP";
    /* Must be logged: this is the only way the operator can learn a
     * value that's unique per device and not known ahead of time. */
    log_pop = true;
#endif

    /* Stash security mode for use on retry-in-place above. */
    CTX->prov_security = security;

    /* Log to console — this is the component's only output responsibility.
     * The operator reads this and enters the PoP into the provisioning
     * app, or generates a QR code from it externally (e.g.
     * https://espressif.github.io/esp-jumpstart/qrcode.html). */
    ESP_LOGI(TAG, "-----------------------------------------");
    ESP_LOGI(TAG, "BLE Provisioning started");
    ESP_LOGI(TAG, "  Device Name : %s", CTX->prov_device_name);
    ESP_LOGI(TAG, "  Auth Mode   : %s", auth_str);
    /* pop is non-NULL exactly when a Security 1 mode is selected. Not
     * written as `security == NETWORK_PROV_SECURITY_1`: network_provisioning
     * only declares that enum member when protocomm security v1 is enabled,
     * so the comparison would not compile under AUTH_MODE_OPEN. */
    if (pop != NULL) {
        if (log_pop) {
            ESP_LOGI(TAG, "  PoP         : %s", pop);
        } else {
            ESP_LOGI(TAG, "  PoP         : (static — set via CONFIG_WIFI_PROV_POP)");
        }
    }
    ESP_LOGI(TAG, "  Transport   : BLE");
    ESP_LOGI(TAG, "-----------------------------------------");

    /* Initialize provisioning manager */
    network_prov_mgr_config_t prov_cfg = {
        .scheme               = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };

    esp_err_t ret = network_prov_mgr_init(prov_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_init failed: %s", esp_err_to_name(ret));

        if (CTX->bt_mem_freed) {
            /* The dead end: BT memory was released after a prior
             * successful FREE_ON_SUCCESS session, and esp_bt_controller_init()
             * does not support being called again in the same boot
             * (documented ESP-IDF limitation, not a transient fault — see
             * WIFI_PROV_BT_LIFECYCLE in Kconfig). Retrying this call would
             * fail identically forever. Restart instead of falling back
             * to SCANNING, so the device comes back up with the
             * controller available and walks the normal boot path back
             * into PROVISIONING. */
            ESP_LOGE(TAG, "BLE controller unavailable after prior BT memory "
                          "release (CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS) "
                          "— this cannot be retried in this boot. Restarting "
                          "to recover.");
            wifi_mgr_error_info_t err_info = {
                .code               = ESP_ERR_WIFI_MGR_BT_PERMANENTLY_RELEASED,
                .state              = WIFI_MGR_STATE_PROVISIONING,
                .consecutive_errors = ++CTX->consecutive_errors,
            };
            wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));
            esp_restart();
            /* esp_restart() does not return. */
        }

        /* Not the dead end — an ordinary init failure (e.g. transient
         * resource pressure on first-ever provisioning attempt). Recover
         * to SCANNING as before; the existing consecutive-error backoff
         * in 09_event_system.md governs the retry cadence from there. */
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
    ret = esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_mgr_prov_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register prov event handler: %s",
                 esp_err_to_name(ret));
        network_prov_mgr_deinit();
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        return ret;
    }

    /* Start provisioning */
    ret = network_prov_mgr_start_provisioning(security, pop,
                                           CTX->prov_device_name, NULL);
    wifi_mgr_driver_storage_ram(); /* start_provisioning() exits in FLASH mode */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_start_provisioning failed: %s",
                 esp_err_to_name(ret));
        esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_mgr_prov_event_handler);
        network_prov_mgr_deinit();
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


/* Bound for the prov_stop_confirm_sem wait in all three
 * wifi_mgr_prov_stop_*() functions (see "Retry Policy" in
 * 08_provisioning_flow.md). Real-hardware logs show two distinct teardown
 * time profiles: ~430ms when stopping with no active BLE connection (only
 * advertising was running), and >2000ms when stopping with an active
 * connection being terminated (the HCI disconnect round-trip to the peer
 * adds latency that varies with the peer's response time and BLE scheduler
 * state). 3000ms gives comfortable margin over both. Not a Kconfig value:
 * unlike WIFI_PROV_BLE_RETRY_BACKOFF_MS (a guard for a rare failure path
 * an integrator might reasonably want to tune), this bound is sized against
 * ESP-IDF's internal NimBLE teardown time — not a deployment-specific
 * tradeoff an integrator can meaningfully choose a different value for. */
#define WIFI_PROV_STOP_CONFIRM_WAIT_MS 3000

/* Poll granularity for the wait above. The wait is polled in short
 * increments, checking CTX->stop_requested between each one, rather than
 * a single xSemaphoreTake() blocking for the full bound — a plain
 * xSemaphoreTake() has no way to wake early on stop_requested changing
 * (it only wakes on the semaphore being given, or its own timeout), and
 * wifi_mgr_stop()'s overall shutdown budget (2000ms — see
 * xSemaphoreTake(CTX->stopped_sem, ...) in wifi_mgr_stop()) does not have
 * 3000ms of slack to spare if this wait were allowed to run to its full
 * bound uninterrupted on every stop. 100ms keeps the worst-case added
 * shutdown latency small relative to that 2000ms budget while still
 * being coarse enough not to busy-poll meaningfully. */
#define WIFI_PROV_STOP_CONFIRM_POLL_MS 100

bool wifi_mgr_prov_stop_failed_attempt(void)
{
    xTimerStop(CTX->prov_timer, 0);
    network_prov_mgr_stop_provisioning();

    /* Deliberately do NOT call prov_mgr_safe_deinit() here. Leaving the
     * session (and the BLE controller) initialized is what allows
     * wifi_mgr_prov_start()'s retry-in-place branch to restart the
     * session directly next call, without ever exercising the
     * esp_bt_controller_deinit()-then-init() cycle that fails on
     * ESP-IDF (see file header comment, race condition fix #2, and
     * WIFI_PROV_BT_LIFECYCLE in Kconfig). This applies under both
     * WIFI_PROV_BT_LIFECYCLE policies — the policies differ only in
     * what happens after a *successful* session (see
     * wifi_mgr_prov_stop_succeeded() below), not after a failure. */

    /* Wait for confirmed teardown before returning, so the caller's
     * subsequent wifi_mgr_prov_start() retry-in-place call doesn't race
     * prov_stop_task() and get ESP_ERR_INVALID_STATE (fix #3 in the file
     * header — this is the real-hardware-discovered bug this wait
     * exists to close). Bounded, not network_prov_mgr_wait(): that API has
     * no timeout parameter at all and would block this task indefinitely
     * if NETWORK_PROV_END somehow never arrives, which is worse than the
     * rare-residual-race this wait is meant to shrink, not eliminate at
     * any cost.
     *
     * Polled in WIFI_PROV_STOP_CONFIRM_POLL_MS increments rather than one
     * long xSemaphoreTake() — see that constant's comment above for why.
     * CTX->stop_requested is checked between polls so a concurrent
     * wifi_mgr_stop() is noticed within one poll interval, not only
     * after the full WIFI_PROV_STOP_CONFIRM_WAIT_MS bound elapses.
     * Returning true here tells the caller (on_prov_failed()) to
     * propagate a stop upward immediately rather than proceeding to post
     * PROVISIONING_FAILED and attempt a restart — same convention
     * on_prov_failed() already uses for its own backoff wait, for the
     * same reason: the manager is shutting down, not retrying. */
    TickType_t waited_ticks = 0;
    const TickType_t poll_ticks = pdMS_TO_TICKS(WIFI_PROV_STOP_CONFIRM_POLL_MS);
    const TickType_t bound_ticks = pdMS_TO_TICKS(WIFI_PROV_STOP_CONFIRM_WAIT_MS);
    bool confirmed = false;
    while (waited_ticks < bound_ticks) {
        if (CTX->stop_requested) {
            ESP_LOGD(TAG, "Stop requested while waiting for BLE teardown "
                          "confirmation — abandoning wait");
            return true;
        }
        TickType_t this_wait = (bound_ticks - waited_ticks < poll_ticks)
                              ? (bound_ticks - waited_ticks)
                              : poll_ticks;
        if (xSemaphoreTake(CTX->prov_stop_confirm_sem, this_wait) == pdTRUE) {
            confirmed = true;
            break;
        }
        waited_ticks += this_wait;
    }
    if (!confirmed) {
        /* Bound expired without seeing confirmation. Proceed anyway —
         * wifi_mgr_prov_start()'s existing backoff-and-retry-once guard
         * in on_prov_failed() remains as the safety net for this rare
         * residual case, exactly as it already was before this wait
         * existed (see file header fix #3: that guard is what made the
         * system self-heal on real hardware even before this fix). */
        ESP_LOGW(TAG, "Timed out waiting %dms for BLE teardown confirmation "
                      "— proceeding to retry anyway",
                 WIFI_PROV_STOP_CONFIRM_WAIT_MS);
    }

    /* Session failed: whatever arrived was never verified. Drop it from
     * RAM, and overwrite the copy network_provisioning wrote into the
     * driver's NVS at CRED_RECV. The station is not connected on this
     * path; disconnect first anyway so a connect still in flight inside
     * the provisioning manager cannot make esp_wifi_set_config() fail with
     * ESP_ERR_WIFI_STATE. The resulting STA_DISCONNECTED is ignored in
     * PROVISIONING. wifi_mgr_driver_wipe_config() also restores RAM mode. */
    wifi_mgr_prov_wipe_held_creds();
    esp_wifi_disconnect();
    wifi_mgr_driver_wipe_config();

    ESP_LOGD(TAG, "Provisioning attempt stopped (session kept alive for retry)");
    return false;
}

void wifi_mgr_prov_stop_succeeded(void)
{
    xTimerStop(CTX->prov_timer, 0);
    network_prov_mgr_stop_provisioning();

    /* CRED_RECV left the driver in FLASH mode. Restore RAM mode so later
     * reconnects don't write to flash. The driver-stored config is not
     * wiped here — the station is connected, and it holds the verified
     * credential; the next wifi_mgr_start() wipes it. */
    wifi_mgr_driver_storage_ram();

#if defined(CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS)
    /* Wait for confirmed BLE teardown before deinitializing — same race
     * as fix #3 (see file header), but here the consequence of racing is
     * a potential heap_caps_free assertion crash (see IDF issue #18065),
     * not just a logged ESP_ERR_INVALID_STATE. A single bounded
     * xSemaphoreTake() is sufficient here (unlike stop_failed_attempt(),
     * this function is not inside the manager task's dispatch loop, so
     * there's no NOTIFY_STOP bit that could be silently consumed by a
     * competing xTaskNotifyWait() call — and even if stop_requested were
     * relevant here, this path only runs once per boot on success, not
     * in a retry loop). If the bound expires, proceed anyway: deinit
     * *after* a stale partial teardown is still safer than skipping it
     * and leaking BT memory, since prov_mgr_safe_deinit() is idempotent. */
    xSemaphoreTake(CTX->prov_stop_confirm_sem,
                   pdMS_TO_TICKS(WIFI_PROV_STOP_CONFIRM_WAIT_MS));
    prov_mgr_safe_deinit();
    CTX->bt_mem_freed = true;
    ESP_LOGI(TAG, "Provisioning succeeded — BLE controller deinitialized and "
                  "BT memory released (CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS). "
                  "Re-entering BLE provisioning in this boot will require a restart.");
#else /* CONFIG_WIFI_PROV_BT_LIFECYCLE_KEEP_ALIVE (default) */
    /* Deliberately left initialized. See WIFI_PROV_BT_LIFECYCLE_KEEP_ALIVE
     * help text in Kconfig: keeps BT memory reserved so this boot can
     * re-enter BLE provisioning later (credential revocation, factory
     * reset button) with no restart required. */
    ESP_LOGD(TAG, "Provisioning succeeded — BLE controller left initialized "
                  "(CONFIG_WIFI_PROV_BT_LIFECYCLE_KEEP_ALIVE)");
#endif
}

void wifi_mgr_prov_stop_shutdown(void)
{
    /* Called only from wifi_mgr_stop() — the manager is being torn down
     * entirely, not retried. Always fully deinitialize regardless of
     * WIFI_PROV_BT_LIFECYCLE policy or CTX->prov_mgr_initialized state;
     * there is no "next attempt" to keep the controller alive for.
     *
     * Wait for confirmed BLE teardown before deinitializing — same race
     * as fix #3 (see file header). This function is called from
     * wifi_mgr_stop()'s caller context, not the manager task, which is
     * why a semaphore (not a task notification) is used: the semaphore
     * give/take pair works regardless of which task is on either end.
     * No stop_requested polling here: this function IS the stop path,
     * and the single bounded xSemaphoreTake() runs in the caller's own
     * task context rather than the manager task, so there's no dispatch
     * loop behind us that needs to unblock quickly. If the bound expires,
     * proceed anyway — deinit is still safer than skipping it. */
    xTimerStop(CTX->prov_timer, 0);
    network_prov_mgr_stop_provisioning();
    wifi_mgr_prov_wipe_held_creds();
    /* No driver wipe here: wifi_mgr_stop() deinitializes Wi-Fi next, and
     * the next wifi_mgr_start() wipes any stored config before starting. */
    wifi_mgr_driver_storage_ram();
    xSemaphoreTake(CTX->prov_stop_confirm_sem,
                   pdMS_TO_TICKS(WIFI_PROV_STOP_CONFIRM_WAIT_MS));
    prov_mgr_safe_deinit();
    ESP_LOGD(TAG, "Provisioning stopped (manager shutdown)");
}
