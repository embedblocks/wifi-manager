/**
 * @file wifi_manager.c
 * @brief WiFi manager main implementation.
 *
 * Owns:
 *  - Global context singleton (g_wifi_mgr_ctx)
 *  - wifi_mgr_init() / wifi_mgr_start() / wifi_mgr_stop() / wifi_mgr_deinit()
 *  - WiFi and IP event handlers → task notifications
 *  - FreeRTOS timer callbacks → task notifications
 *  - Manager task: state machine loop
 *  - on_connect_success() / on_connect_failure() / on_disconnect()
 *  - on_prov_done() / on_prov_failed()
 *  - on_rescan_once_result()
 *  - on_tw_expired() / start_provisioning_on_tw_expiry()
 *  - All public API functions
 *
 * Design constraints enforced here:
 *  - No heap allocation after wifi_mgr_init() returns (N5)
 *  - Event handlers never block (N2)
 *  - Timer callbacks only call xTaskNotify (cross-cutting constraint)
 *  - Notification bits checked in priority order: STOP > TW_EXPIRED >
 *    CONNECTED > CONNECT_TIMEOUT > DISCONNECTED > SCAN_DONE > PROV_DONE >
 *    PROV_FAILED
 *  - tw_timer started exactly once per TW_ACTIVE entry
 *  - ERROR entry clears all transient connection state
 *
 * See 02_state_machine.md, 04_public_api.md, 05_internal_architecture.md,
 * and the implementation plan for the full specification.
 */

#include "wifi_manager.h"
#include "wifi_mgr_ctx.h"
#include "wifi_mgr_state.h"
#include "wifi_mgr_events.h"
#include "wifi_mgr_scan.h"
#include "wifi_mgr_connect.h"
#include "wifi_mgr_prov.h"
#include "ap_records.h"
#include "blob_storage.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "string.h"
#include "stdio.h"
#include "inttypes.h"

static const char *TAG = "wifi_mgr";

/* ── Global singleton ────────────────────────────────────────────────────── */

wifi_mgr_ctx_t g_wifi_mgr_ctx = {0};

/* ── Error backoff schedule ──────────────────────────────────────────────── */

static uint32_t error_backoff_ms(uint8_t consecutive_errors)
{
    if (consecutive_errors <= 2) return 5000u;
    if (consecutive_errors <= 5) return 10000u;
    if (consecutive_errors <= 9) return 30000u;
    return 60000u;
}

/* ── Timer callbacks (must only call xTaskNotify; no blocking) ───────────── */

static void tw_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (CTX->task_handle) {
        xTaskNotify(CTX->task_handle, NOTIFY_TW_EXPIRED, eSetBits);
    }
}

static void connect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (CTX->task_handle) {
        xTaskNotify(CTX->task_handle, NOTIFY_CONNECT_TIMEOUT, eSetBits);
    }
}

static void prov_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (CTX->task_handle) {
        xTaskNotify(CTX->task_handle, NOTIFY_PROV_FAILED, eSetBits);
    }
}

/* ── WiFi / IP event handlers ────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    switch ((wifi_event_t)event_id) {

    case WIFI_EVENT_SCAN_DONE:
        if (CTX->task_handle) {
            xTaskNotify(CTX->task_handle, NOTIFY_SCAN_DONE, eSetBits);
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* Intentional no-op — wait for IP_EVENT_STA_GOT_IP.
         * Association alone does not mean we have a usable link. */
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        if (CTX->stop_requested) {
            /* Disconnect triggered by wifi_mgr_stop() — ignore. */
            return;
        }
        {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)event_data;
            CTX->disconnect_reason = (uint16_t)disc->reason;
        }
        if (CTX->task_handle) {
            xTaskNotify(CTX->task_handle, NOTIFY_DISCONNECTED, eSetBits);
        }
        break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if ((ip_event_t)event_id == IP_EVENT_STA_GOT_IP) {
        /* Write conn_info BEFORE xTaskNotify to satisfy the happens-before
         * contract on dual-core (see 05_internal_architecture.md).
         * The manager task reads conn_info after NOTIFY_CONNECTED fires. */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        wifi_ap_record_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        esp_wifi_sta_get_ap_info(&ap_info);

        memcpy(CTX->conn_info.ssid, ap_info.ssid, sizeof(CTX->conn_info.ssid));
        memcpy(CTX->conn_info.bssid, ap_info.bssid, 6);
        CTX->conn_info.ip = e->ip_info.ip.addr;

        if (CTX->task_handle) {
            xTaskNotify(CTX->task_handle, NOTIFY_CONNECTED, eSetBits);
        }
    }
}

/* ── Connection outcome handlers (called from manager task only) ─────────── */

/**
 * @brief Handle a successful IP assignment.
 *
 * Cancels the connect and tw timers, increments use_count, conditionally
 * saves to NVS (throttled by use_count_save_interval), transitions to
 * CONNECTED, and sets BIT_CONNECTED.
 */
static void on_connect_success(void)
{
    xTimerStop(CTX->connect_timer, 0);

    /* Capture channel */
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);
    CTX->conn_info.channel = primary;

    /* Increment use_count for the successfully connected AP */
    const char *ssid = CTX->connect_list[CTX->connect_list_index].ssid;
    ap_records_increment_use_count(ssid);

    CTX->connect_count_since_save++;
    if (CTX->connect_count_since_save >= CTX->config.use_count_save_interval) {
        esp_err_t ret = ap_records_save();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ap_records_save failed: %s", esp_err_to_name(ret));
        }
        CTX->connect_count_since_save = 0;
    }

    wifi_mgr_state_set(WIFI_MGR_STATE_CONNECTED);
    wifi_mgr_events_post(WIFI_MGR_EVENT_CONNECTED,
                         &CTX->conn_info, sizeof(CTX->conn_info));
    xEventGroupSetBits(CTX->event_group, BIT_CONNECTED);

    ESP_LOGI(TAG, "Connected: SSID=%s channel=%u", ssid, primary);
}

/**
 * @brief Handle a failed connection attempt (timeout or disconnect during connect).
 *
 * Advances through retry → next AP → rescan logic.
 * All connect-tracking fields are owned by the manager task; no mutex needed.
 */
static void on_connect_failure(uint16_t reason)
{
    xTimerStop(CTX->connect_timer, 0);
    esp_wifi_disconnect(); /* ensure driver is in clean state */
    CTX->tw_last_reason = reason; /* 0 = connect_timeout_ms expired */

    CTX->connect_retry++;
    if (CTX->connect_retry < CTX->config.max_connect_attempts) {
        /* Retry same AP (next attempt will use SSID-only since retry >= 1) */
        ESP_LOGD(TAG, "Retry %u/%u for AP: %s",
                 CTX->connect_retry + 1,
                 CTX->config.max_connect_attempts,
                 CTX->connect_list[CTX->connect_list_index].ssid);
        return;
    }

    /* Retries exhausted for this AP — advance to next */
    CTX->connect_retry = 0;
    CTX->connect_list_index++;

    if (CTX->connect_list_index < CTX->connect_list_count) {
        ESP_LOGD(TAG, "Advancing to AP #%u: %s",
                 CTX->connect_list_index + 1,
                 CTX->connect_list[CTX->connect_list_index].ssid);
        return;
    }

    /* All APs exhausted — clear list; manager task will rescan */
    ESP_LOGD(TAG, "All APs exhausted — rescanning within TW window");
    CTX->connect_list_count = 0;
    CTX->connect_list_index = 0;
    CTX->connect_retry      = 0;
}

/**
 * @brief Handle a disconnect event from CONNECTED or during a connect attempt.
 *
 * From CONNECTED → RESCAN_ONCE (fast reconnect path).
 * From other states (e.g. tw_expired disconnects) → SCANNING.
 */
static void on_disconnect(void)
{
    xTimerStop(CTX->connect_timer, 0);
    xEventGroupClearBits(CTX->event_group, BIT_CONNECTED);

    /* Build and post DISCONNECTED event */
    wifi_mgr_disconnected_info_t disc_info;
    memset(&disc_info, 0, sizeof(disc_info));
    strncpy(disc_info.ssid, CTX->conn_info.ssid, sizeof(disc_info.ssid) - 1);
    memcpy(disc_info.bssid, CTX->conn_info.bssid, 6);
    disc_info.reason = CTX->disconnect_reason;
    wifi_mgr_events_post(WIFI_MGR_EVENT_DISCONNECTED,
                         &disc_info, sizeof(disc_info));

    ESP_LOGW(TAG, "Disconnected from %s (reason %u)",
             disc_info.ssid, disc_info.reason);

    wifi_mgr_state_t current = wifi_mgr_state_get();

    if (current == WIFI_MGR_STATE_CONNECTED && CTX->conn_info.ssid[0] != '\0') {
        /* Fast reconnect path: remember the last AP and do one immediate scan */
        strncpy(CTX->last_connected_ssid, CTX->conn_info.ssid,
                sizeof(CTX->last_connected_ssid) - 1);
        CTX->last_connected_ssid[sizeof(CTX->last_connected_ssid) - 1] = '\0';
        memcpy(CTX->last_connected_bssid, CTX->conn_info.bssid, 6);

        wifi_mgr_state_set(WIFI_MGR_STATE_RESCAN_ONCE);
    } else {
        /* Not from CONNECTED — full scan cycle */
        CTX->last_connected_ssid[0] = '\0';
        memset(CTX->last_connected_bssid, 0, 6);

        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
    }

    /* Clear connect list regardless */
    CTX->connect_list_count = 0;
    CTX->connect_list_index = 0;
    CTX->connect_retry      = 0;
}

/* ── Provisioning outcome handlers ───────────────────────────────────────── */

/**
 * @brief Handle successful provisioning: save credentials and transition to CONNECTED.
 *
 * Called from the manager task after NOTIFY_PROV_DONE.
 * ap_records_save() runs here (manager task context, not system event task).
 */
static void on_prov_done(void)
{
    wifi_mgr_prov_stop_succeeded();

    /* The connection is confirmed (NETWORK_PROV_WIFI_CRED_SUCCESS), so the
     * held credential is now proven and may enter ap_records. It gets
     * use_count = max + 1 inside ap_records_add() and is tried first next
     * time. The BSSID is the AP actually joined (zeros if the SSID did not
     * match); it is only used as a lock when seen in a fresh scan. */
    if (CTX->held_confirmed) {
        static const uint8_t zero_bssid[6] = {0};
        const uint8_t *bssid = memcmp(CTX->held_bssid, zero_bssid, 6) != 0
                             ? CTX->held_bssid : NULL;
        esp_err_t ret = ap_records_add(CTX->held_ssid, CTX->held_password, bssid);
        if (ret == ESP_OK) {
            ret = ap_records_save();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Saving provisioned credential failed: %s",
                     esp_err_to_name(ret));
            wifi_mgr_error_info_t err_info = {
                .code               = ret,
                .state              = WIFI_MGR_STATE_PROVISIONING,
                .consecutive_errors = CTX->consecutive_errors,
            };
            wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));
            /* State still transitions to CONNECTED — the link is up. */
        }
    } else {
        ESP_LOGW(TAG, "Provisioning reported success without a held credential "
                      "— nothing saved");
    }
    wifi_mgr_prov_wipe_held_creds();

    /* conn_info.channel was never filled on the provisioning path
     * (on_connect_success() does it for the normal path). */
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        CTX->conn_info.channel = primary;
    }

    wifi_mgr_state_set(WIFI_MGR_STATE_CONNECTED);
    /* PROVISIONING_DONE must fire before CONNECTED per event ordering guarantee */
    wifi_mgr_events_post(WIFI_MGR_EVENT_PROVISIONING_DONE, NULL, 0);
    wifi_mgr_events_post(WIFI_MGR_EVENT_CONNECTED,
                         &CTX->conn_info, sizeof(CTX->conn_info));
    xEventGroupSetBits(CTX->event_group, BIT_CONNECTED);

    ESP_LOGI(TAG, "Provisioning complete; connected");
}

/**
 * @brief Handle provisioning failure or timeout. Retries in place.
 *
 * Does NOT revert to SCANNING. The BLE session is stopped (via
 * wifi_mgr_prov_stop_failed_attempt(), which deliberately does not
 * deinitialize network_prov_mgr or the BLE controller, and also waits —
 * bounded — for confirmed BLE teardown before returning; see
 * wifi_mgr_prov.h and wifi_mgr_prov.c's file header fix #3) and
 * immediately restarted via wifi_mgr_prov_start(), which detects the
 * already-initialized session and restarts it in place. This retries
 * indefinitely until credentials are received successfully or the
 * device is power-cycled — there is no bounded retry count and no
 * escalation to SCANNING for this failure mode, by design (see
 * CONFIG_WIFI_PROV_BT_LIFECYCLE in Kconfig for why: reverting to
 * SCANNING would eventually require re-entering PROVISIONING via the tw
 * timer, which would hit the documented esp_bt_controller_init()-after-
 * deinit wall on FREE_ON_SUCCESS, or be pointless churn on KEEP_ALIVE
 * since the session is already alive).
 *
 * wifi_mgr_prov_start() can still itself fail or trigger esp_restart()
 * (the CTX->bt_mem_freed dead-end case) — those are handled inside
 * wifi_mgr_prov_start() itself, not here.
 *
 * @return true if a stop was detected during either the BLE-teardown
 *         wait inside wifi_mgr_prov_stop_failed_attempt() or this
 *         function's own retry backoff wait, and the caller (the
 *         PROVISIONING case in the manager task's dispatch loop) must
 *         `goto cleanup` immediately, exactly as it already does on its
 *         own direct NOTIFY_STOP checks. false in every other case
 *         (including the common path where no backoff wait was needed
 *         at all).
 */
static bool on_prov_failed(void)
{
    if (wifi_mgr_prov_stop_failed_attempt()) {
        /* Stop detected while waiting for BLE teardown confirmation —
         * propagate immediately. Do NOT post PROVISIONING_FAILED or
         * attempt a restart below; the manager is shutting down. */
        return true;
    }

    wifi_mgr_events_post(WIFI_MGR_EVENT_PROVISIONING_FAILED, NULL, 0);
    ESP_LOGW(TAG, "Provisioning attempt failed or timed out — retrying "
                  "(staying in PROVISIONING)");

    /* State remains WIFI_MGR_STATE_PROVISIONING throughout — only the
     * underlying BLE session is restarted, not the state machine state. */
    esp_err_t ret = wifi_mgr_prov_start();
    if (ret != ESP_OK) {
        /* wifi_mgr_prov_start() already logged and posted ERROR (and, in
         * the bt_mem_freed dead-end case, already called esp_restart()
         * and will not return at all — this line is then unreachable).
         *
         * If it did return, this call went through the retry-in-place
         * branch (CTX->prov_mgr_initialized was already true — every
         * call to wifi_mgr_prov_start() from inside on_prov_failed() does,
         * since on_prov_failed() is only reachable after PROVISIONING was
         * already entered once). That branch deliberately does NOT
         * recover state to SCANNING on failure (see wifi_mgr_prov.c) — so
         * unlike a cold-init failure at first entry into PROVISIONING,
         * state is still PROVISIONING here.
         *
         * Guard against a tight retry loop: if
         * network_prov_mgr_start_provisioning() itself fails synchronously
         * and immediately (rather than via the normal prov_timer
         * timeout), nothing else throttles the next attempt — the prov
         * event handler never fired, so no NOTIFY_PROV_FAILED is coming
         * on its own. Wait out CTX->config.ble_retry_backoff_ms here
         * (CONFIG_WIFI_PROV_BLE_RETRY_BACKOFF_MS by default — deliberately
         * not inter_scan_delay_ms; see that Kconfig entry's help text for
         * why these two delays are kept separate), still responsive to
         * NOTIFY_STOP, then retry once more. */
        ESP_LOGE(TAG, "Failed to restart provisioning after failure: %s — "
                      "backing off before next attempt", esp_err_to_name(ret));
        uint32_t notif = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notif,
                        pdMS_TO_TICKS(CTX->config.ble_retry_backoff_ms));
        if ((notif & NOTIFY_STOP) || CTX->stop_requested) {
            /* Tell the caller to goto cleanup directly — same as every
             * other NOTIFY_STOP check in the dispatch loop. No bit
             * re-posting needed: the bit we just consumed is reflected
             * in this return value instead. CTX->stop_requested is
             * checked in addition to the notif bit because
             * wifi_mgr_stop() sets it synchronously before NOTIFY_STOP
             * is ever sent (see wifi_mgr_stop()) — checking it here
             * catches a stop already in progress even if the
             * notification bit itself hasn't arrived in this exact
             * xTaskNotifyWait call. */
            return true;
        }
        ret = wifi_mgr_prov_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning restart still failing after backoff: "
                          "%s — will retry on next NOTIFY_PROV_FAILED",
                     esp_err_to_name(ret));
        }
    }
    return false;
}

/* ── tw expiry ───────────────────────────────────────────────────────────── */

/**
 * @brief Whether tw expiry may start BLE provisioning right now.
 *
 * Evaluated at expiry, not at TW_ACTIVE entry, so credentials added or
 * removed during the window are taken into account.
 */
static bool auto_provision_allowed(void)
{
    switch (CTX->config.auto_provision) {
    case WIFI_MGR_AUTO_PROV_ALWAYS:
        return true;
    case WIFI_MGR_AUTO_PROV_NEVER:
        return false;
    case WIFI_MGR_AUTO_PROV_WHEN_UNPROVISIONED:
    default:
        return ap_records_get_count() <= 0;
    }
}

/**
 * @brief Hard-deadline path: abort whatever is in progress and start BLE.
 *
 * tw expiry wins unconditionally (D4 in 02_state_machine.md). If NOTIFY_CONNECTED
 * and NOTIFY_TW_EXPIRED both arrive in the same xTaskNotifyWait call, the IP
 * is discarded in favour of provisioning. Only reached when
 * auto_provision_allowed() — under the default policy that means no
 * credentials are stored, so there is no "good" connection to lose.
 */
static void start_provisioning_on_tw_expiry(void)
{
    xTimerStop(CTX->tw_timer, 0);
    xTimerStop(CTX->connect_timer, 0);
    esp_wifi_scan_stop();

    esp_err_t disc_ret = esp_wifi_disconnect();
    if (disc_ret != ESP_OK) {
        /* Non-fatal — log and continue. The prov_mgr will reset WiFi state. */
        ESP_LOGW(TAG, "esp_wifi_disconnect in tw_expired: %s — proceeding",
                 esp_err_to_name(disc_ret));
    }

    wifi_mgr_state_set(WIFI_MGR_STATE_PROVISIONING);

    esp_err_t ret = wifi_mgr_prov_start();
    if (ret != ESP_OK) {
        /* prov_start() already recovered state to SCANNING and posted ERROR */
        ESP_LOGE(TAG, "BLE provisioning start failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Handle NOTIFY_TW_EXPIRED.
 *
 * Reports what the window saw (KNOWN_AP_UNREACHABLE if a stored AP was
 * visible, NO_KNOWN_AP_IN_TW otherwise), then either starts provisioning
 * (hard deadline) or re-arms the window and lets the caller carry on.
 *
 * Soft path guarantees: no esp_wifi_disconnect(), no scan abort, and the
 * caller must still act on any other bits in the same notification — in
 * particular a NOTIFY_CONNECTED that arrived together with the expiry.
 *
 * A stale expiry bit (timer fired just before scan results moved the state
 * back to SCANNING) is ignored outright: provisioning is never started
 * without live APs (R7), and the window must not be re-armed outside
 * TW_ACTIVE.
 *
 * @return true  if provisioning was started (state is now PROVISIONING, or
 *               SCANNING if the start failed) — caller must abandon its
 *               current wait and `continue` the outer loop.
 *         false if the bit was stale or the window was re-armed — caller
 *               continues what it was doing, ignoring the expiry bit.
 */
static bool on_tw_expired(void)
{
    if (wifi_mgr_state_get() != WIFI_MGR_STATE_TW_ACTIVE) {
        ESP_LOGD(TAG, "Stale tw expiry ignored (state %s)",
                 wifi_mgr_state_str(wifi_mgr_state_get()));
        return false;
    }

    /* tw_attempts > 0 also counts: after a soft re-arm the window's
     * tw_known_seen is cleared, but attempts may continue from the existing
     * connect_list without a fresh scan setting it again. */
    if (CTX->tw_known_seen || CTX->tw_attempts > 0) {
        wifi_mgr_known_ap_unreachable_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.ssid, CTX->tw_last_ssid, sizeof(info.ssid) - 1);
        info.last_reason = CTX->tw_last_reason;
        info.attempts    = CTX->tw_attempts;
        wifi_mgr_events_post(WIFI_MGR_EVENT_KNOWN_AP_UNREACHABLE,
                             &info, sizeof(info));
        ESP_LOGW(TAG, "tw expired: stored AP '%s' visible but unreachable "
                      "(%u attempts, last reason %u)",
                 info.ssid, info.attempts, info.last_reason);
    } else {
        wifi_mgr_events_post(WIFI_MGR_EVENT_NO_KNOWN_AP_IN_TW, NULL, 0);
    }

    if (auto_provision_allowed()) {
        start_provisioning_on_tw_expiry();
        return true;
    }

    ESP_LOGI(TAG, "tw expired; auto-provisioning not allowed (policy %d, %d "
                  "stored APs) — re-arming window and continuing",
             (int)CTX->config.auto_provision, ap_records_get_count());
    wifi_mgr_tw_window_reset();
    xTimerStart(CTX->tw_timer, 0);
    return false;
}

/* ── RESCAN_ONCE result handler ──────────────────────────────────────────── */

/**
 * @brief Process scan results in RESCAN_ONCE state.
 *
 * Checks for the last connected AP (BSSID preferred over SSID).
 * If found: populates connect_list with one entry and calls connect_attempt().
 * If not found: returns to SCANNING.
 *
 * State guard: if state is no longer RESCAN_ONCE when this runs (e.g. because
 * wifi_mgr_start_ble_provisioning() was called), the result is a no-op.
 */
static void on_rescan_once_result(void)
{
    if (wifi_mgr_state_get() != WIFI_MGR_STATE_RESCAN_ONCE) {
        ESP_LOGD(TAG, "RESCAN_ONCE result ignored — state changed");
        return;
    }

    uint16_t count = CONFIG_WIFI_SCAN_MAX_RESULTS;
    esp_wifi_scan_get_ap_num(&count);
    if (count > CONFIG_WIFI_SCAN_MAX_RESULTS) {
        count = CONFIG_WIFI_SCAN_MAX_RESULTS;
    }

    wifi_ap_record_t scan_buf[CONFIG_WIFI_SCAN_MAX_RESULTS];
    memset(scan_buf, 0, sizeof(scan_buf));
    uint16_t actual = count;
    esp_wifi_scan_get_ap_records(&actual, scan_buf);
    count = actual;

    /* BSSID-preference matching: prefer exact BSSID match when non-zero;
     * fall back to SSID to handle BSSID changes (roaming, hardware swap). */
    static const uint8_t zero_bssid[6] = {0};
    bool bssid_available = (memcmp(CTX->last_connected_bssid, zero_bssid, 6) != 0);
    int  bssid_match_idx = -1;
    int  ssid_match_idx  = -1;

    for (uint16_t j = 0; j < count; j++) {
        if (bssid_available &&
            memcmp(scan_buf[j].bssid, CTX->last_connected_bssid, 6) == 0) {
            bssid_match_idx = (int)j;
            break; /* exact BSSID match wins unconditionally */
        }
        if (ssid_match_idx == -1 &&
            strcmp((char *)scan_buf[j].ssid, CTX->last_connected_ssid) == 0) {
            ssid_match_idx = (int)j;
            /* Continue — a BSSID match later in the buffer takes priority. */
        }
    }

    int match_idx = (bssid_match_idx >= 0) ? bssid_match_idx : ssid_match_idx;

    if (match_idx == -1) {
        ESP_LOGD(TAG, "RESCAN_ONCE: last AP '%s' not found — returning to SCANNING",
                 CTX->last_connected_ssid);
        CTX->last_connected_ssid[0] = '\0';
        memset(CTX->last_connected_bssid, 0, 6);
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
        return;
    }

    /* AP found — look up full credential (including password) from ap_records */
    ap_info_t ap;
    int idx;
    esp_err_t ret = ap_records_find_by_ssid(CTX->last_connected_ssid, &ap, &idx);
    if (ret != ESP_OK) {
        /* Credential removed while we were scanning — fall to SCANNING */
        ESP_LOGW(TAG, "RESCAN_ONCE: AP '%s' found in scan but not in ap_records",
                 CTX->last_connected_ssid);
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
        return;
    }

    /* BSSID lock only if that BSSID was in this scan (see wifi_mgr_scan.c). */
    if (bssid_match_idx < 0 ||
        memcmp(ap.bssid, scan_buf[bssid_match_idx].bssid, 6) != 0) {
        memset(ap.bssid, 0, sizeof(ap.bssid));
    }

    /* Populate connect_list with this single AP and start connecting */
    CTX->connect_list[0]    = ap;
    CTX->connect_list_count = 1;
    CTX->connect_list_index = 0;
    CTX->connect_retry      = 0;

    ESP_LOGD(TAG, "RESCAN_ONCE: AP '%s' found (%s match) — connecting",
             CTX->last_connected_ssid,
             (bssid_match_idx >= 0) ? "BSSID" : "SSID");

    wifi_mgr_connect_attempt();
}

/* ── Manager task ────────────────────────────────────────────────────────── */

/**
 * @brief Enter ERROR state with proper cleanup.
 *
 * Per D7 (02_state_machine.md): clears all transient connection state on ERROR entry.
 */
static void enter_error_state(esp_err_t code, wifi_mgr_state_t from_state)
{
    /* Mandatory ERROR entry cleanup — prevents stale connect state surviving */
    CTX->connect_list_count = 0;
    CTX->connect_list_index = 0;
    CTX->connect_retry      = 0;
    xTimerStop(CTX->connect_timer, 0);

    CTX->consecutive_errors++;
    wifi_mgr_state_set(WIFI_MGR_STATE_ERROR);

    wifi_mgr_error_info_t err_info = {
        .code               = code,
        .state              = from_state,
        .consecutive_errors = CTX->consecutive_errors,
    };
    wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err_info, sizeof(err_info));

    ESP_LOGE(TAG, "Error in state %s: %s (consecutive: %u)",
             wifi_mgr_state_str(from_state),
             esp_err_to_name(code),
             CTX->consecutive_errors);
}

static void manager_task(void *arg)
{
    (void)arg;

    /* Log tw sizing info at startup for operator awareness (L5/L6 mitigation) */
    uint32_t min_exhaustive = (uint32_t)CONFIG_MAX_AP_COUNT
                            * CTX->config.max_connect_attempts
                            * CTX->config.connect_timeout_ms;
    ESP_LOGI(TAG, "tw=%"PRIu32"ms  min_exhaustive=%"PRIu32"ms "
             "(APs=%d * attempts=%u * timeout=%"PRIu32"ms)",
             CTX->config.known_ap_wait_ms, min_exhaustive,
             CONFIG_MAX_AP_COUNT,
             CTX->config.max_connect_attempts,
             CTX->config.connect_timeout_ms);
    if (CTX->config.known_ap_wait_ms < min_exhaustive) {
        ESP_LOGW(TAG, "tw (%"PRIu32"ms) < min_exhaustive (%"PRIu32"ms). "
                 "Some stored APs may not be tried before provisioning starts. "
                 "Increase CONFIG_WIFI_KNOWN_AP_WAIT_MS. See L5 in 01_overview.md.",
                 CTX->config.known_ap_wait_ms, min_exhaustive);
    }

    uint32_t scan_watchdog_ms = WIFI_MGR_SCAN_WATCHDOG_MS(CTX->config.inter_scan_delay_ms);

    while (true) {
        if (CTX->stop_requested) {
            goto cleanup;
        }

        uint32_t notif = 0;
        wifi_mgr_state_t state = wifi_mgr_state_get();

        /* ── SCANNING ───────────────────────────────────────────────────── */
        if (state == WIFI_MGR_STATE_SCANNING) {
            esp_err_t ret = wifi_mgr_scan_start();
            if (ret != ESP_OK) {
                enter_error_state(ret, WIFI_MGR_STATE_SCANNING);
                uint32_t backoff = error_backoff_ms(CTX->consecutive_errors);
                xTaskNotifyWait(0, UINT32_MAX, &notif, pdMS_TO_TICKS(backoff));
                if (notif & NOTIFY_STOP) goto cleanup;
                wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
                wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
                continue;
            }

            xTaskNotifyWait(0, UINT32_MAX, &notif, pdMS_TO_TICKS(scan_watchdog_ms));
            if (notif & NOTIFY_STOP) goto cleanup;

            if (notif & NOTIFY_SCAN_DONE) {
                wifi_mgr_scan_handle_results();
                CTX->consecutive_errors = 0; /* reset on successful scan */

                /* If still in SCANNING after handling results, wait inter-scan delay */
                if (wifi_mgr_state_get() == WIFI_MGR_STATE_SCANNING) {
                    xTaskNotifyWait(0, UINT32_MAX, &notif,
                                    pdMS_TO_TICKS(CTX->config.inter_scan_delay_ms));
                    if (notif & NOTIFY_STOP) goto cleanup;
                    if (notif & NOTIFY_TW_EXPIRED) { on_tw_expired(); } /* stale → ignored */
                }
                continue;
            }

            /* Watchdog: scan hung — stop and retry */
            ESP_LOGW(TAG, "Scan watchdog fired (%"PRIu32"ms) — stopping scan and retrying",
                     scan_watchdog_ms);
            esp_wifi_scan_stop();
            continue;
        }

        /* ── TW_ACTIVE ──────────────────────────────────────────────────── */
        if (state == WIFI_MGR_STATE_TW_ACTIVE) {

            if (CTX->connect_list_count > 0) {
                /* Known APs available — attempt connection */
                wifi_mgr_connect_attempt();
                CTX->tw_attempts++;
                strncpy(CTX->tw_last_ssid,
                        CTX->connect_list[CTX->connect_list_index].ssid,
                        sizeof(CTX->tw_last_ssid) - 1);
                CTX->tw_last_ssid[sizeof(CTX->tw_last_ssid) - 1] = '\0';

                /* Wait for this attempt's outcome. A soft tw expiry (window
                 * re-armed, no provisioning) must not end the wait: the
                 * attempt is still in flight, and starting another one on
                 * top of it would race the driver. connect_timer is always
                 * armed by wifi_mgr_connect_attempt(), so the loop ends. */
                for (;;) {
                    notif = 0;
                    xTaskNotifyWait(0, UINT32_MAX, &notif, portMAX_DELAY);
                    if (notif & NOTIFY_STOP) goto cleanup;
                    if (notif & NOTIFY_TW_EXPIRED) {
                        const bool soft = !auto_provision_allowed();
                        if ((notif & NOTIFY_CONNECTED) && soft) {
                            /* Expiry and IP in the same wakeup, and the
                             * deadline is soft: the connection wins and the
                             * expiry is moot (tw stopped just below). No
                             * KNOWN_AP_UNREACHABLE for an AP that connected. */
                        } else if (soft &&
                                   (notif & (NOTIFY_CONNECT_TIMEOUT | NOTIFY_DISCONNECTED))) {
                            /* Expiry and this attempt's failure together:
                             * record the failure first, so it is counted in
                             * the window that is ending (reason included),
                             * not carried into the re-armed one. */
                            on_connect_failure((notif & NOTIFY_DISCONNECTED)
                                               ? CTX->disconnect_reason : 0);
                            on_tw_expired();
                            break;
                        } else if (on_tw_expired()) {
                            break; /* hard deadline: provisioning started (D4) */
                        }
                    }
                    if (notif & NOTIFY_CONNECTED) {
                        xTimerStop(CTX->tw_timer, 0);
                        on_connect_success();
                        break;
                    }
                    if (notif & NOTIFY_CONNECT_TIMEOUT) { on_connect_failure(0); break; }
                    if (notif & NOTIFY_DISCONNECTED) {
                        on_connect_failure(CTX->disconnect_reason);
                        break;
                    }
                    /* Nothing actionable (soft expiry, stale bits) — keep waiting. */
                }
                continue;

            } else {
                /* No known APs — rescan within TW window */
                esp_err_t ret = wifi_mgr_scan_start();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Scan failed in TW_ACTIVE: %s — waiting inter-scan delay",
                             esp_err_to_name(ret));
                    xTaskNotifyWait(0, UINT32_MAX, &notif,
                                    pdMS_TO_TICKS(CTX->config.inter_scan_delay_ms));
                    if (notif & NOTIFY_STOP)       goto cleanup;
                    if (notif & NOTIFY_TW_EXPIRED) { on_tw_expired(); }
                    continue;
                }

                /* Wait for SCAN_DONE, bounded by the scan watchdog. A soft
                 * tw expiry does not abort the scan in flight. */
                const TickType_t wd_ticks = pdMS_TO_TICKS(scan_watchdog_ms);
                const TickType_t t0 = xTaskGetTickCount();
                bool provisioning = false;
                bool scan_done    = false;
                for (;;) {
                    TickType_t elapsed = xTaskGetTickCount() - t0;
                    if (elapsed >= wd_ticks) break;
                    notif = 0;
                    xTaskNotifyWait(0, UINT32_MAX, &notif, wd_ticks - elapsed);
                    if (notif & NOTIFY_STOP) goto cleanup;
                    if ((notif & NOTIFY_TW_EXPIRED) && on_tw_expired()) {
                        provisioning = true;
                        break;
                    }
                    if (notif & NOTIFY_SCAN_DONE) { scan_done = true; break; }
                }
                if (provisioning) continue;

                if (scan_done) {
                    wifi_mgr_scan_handle_results();
                    CTX->consecutive_errors = 0;

                    /* Honour TW_EXPIRED during inter-scan delay too */
                    if (wifi_mgr_state_get() == WIFI_MGR_STATE_TW_ACTIVE
                        && CTX->connect_list_count == 0) {
                        xTaskNotifyWait(0, UINT32_MAX, &notif,
                                        pdMS_TO_TICKS(CTX->config.inter_scan_delay_ms));
                        if (notif & NOTIFY_STOP)       goto cleanup;
                        if (notif & NOTIFY_TW_EXPIRED) { on_tw_expired(); }
                    }
                    continue;
                }

                /* Watchdog in TW_ACTIVE */
                esp_wifi_scan_stop();
                continue;
            }
        }

        /* ── CONNECTED ──────────────────────────────────────────────────── */
        if (state == WIFI_MGR_STATE_CONNECTED) {
            xTaskNotifyWait(0, UINT32_MAX, &notif, portMAX_DELAY);
            if (notif & NOTIFY_STOP)        goto cleanup;
            if (notif & NOTIFY_DISCONNECTED) { on_disconnect(); continue; }
            /* All other notifications discarded in CONNECTED state */
            continue;
        }

        /* ── RESCAN_ONCE ────────────────────────────────────────────────── */
        if (state == WIFI_MGR_STATE_RESCAN_ONCE) {
            esp_err_t ret = wifi_mgr_scan_start();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Scan failed in RESCAN_ONCE: %s — returning to SCANNING",
                         esp_err_to_name(ret));
                CTX->last_connected_ssid[0] = '\0';
                memset(CTX->last_connected_bssid, 0, 6);
                wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
                wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
                continue;
            }

            xTaskNotifyWait(0, UINT32_MAX, &notif, pdMS_TO_TICKS(scan_watchdog_ms));
            if (notif & NOTIFY_STOP) goto cleanup;

            if (notif & NOTIFY_SCAN_DONE) {
                on_rescan_once_result();

                /* If connect_list was populated, wait for connect outcome */
                if (CTX->connect_list_count > 0 &&
                    wifi_mgr_state_get() == WIFI_MGR_STATE_RESCAN_ONCE) {

                    xTaskNotifyWait(0, UINT32_MAX, &notif, portMAX_DELAY);
                    if (notif & NOTIFY_STOP)           goto cleanup;
                    if (notif & NOTIFY_CONNECTED)       { on_connect_success(); continue; }
                    if (notif & NOTIFY_CONNECT_TIMEOUT) {
                        on_connect_failure(0);
                        if (CTX->connect_list_count == 0) {
                            /* All retries exhausted */
                            CTX->last_connected_ssid[0] = '\0';
                            memset(CTX->last_connected_bssid, 0, 6);
                            wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
                            wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
                        }
                        continue;
                    }
                    if (notif & NOTIFY_DISCONNECTED) {
                        /* Connection attempt disconnected — give up, go to SCANNING */
                        CTX->last_connected_ssid[0] = '\0';
                        memset(CTX->last_connected_bssid, 0, 6);
                        CTX->connect_list_count = 0;
                        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
                        wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
                        continue;
                    }
                }
                continue;
            }

            /* Watchdog: scan hung in RESCAN_ONCE */
            ESP_LOGW(TAG, "Scan watchdog in RESCAN_ONCE — clearing last AP and returning to SCANNING");
            esp_wifi_scan_stop();
            CTX->last_connected_ssid[0] = '\0';
            memset(CTX->last_connected_bssid, 0, 6);
            wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
            wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
            continue;
        }

        /* ── PROVISIONING ───────────────────────────────────────────────── */
        if (state == WIFI_MGR_STATE_PROVISIONING) {
            xTaskNotifyWait(0, UINT32_MAX, &notif, portMAX_DELAY);
            if (notif & NOTIFY_STOP)        goto cleanup;
            if (notif & NOTIFY_PROV_DONE)   { on_prov_done();   continue; }
            if (notif & NOTIFY_PROV_FAILED) { if (on_prov_failed()) goto cleanup; continue; }
            /* NOTIFY_CONNECTED and NOTIFY_DISCONNECTED are silently discarded
             * in PROVISIONING state. network_prov_mgr owns the WiFi stack.
             * Acting on CONNECTED would call on_connect_success() with an
             * empty connect_list — undefined behaviour. */
            continue;
        }

        /* ── ERROR ──────────────────────────────────────────────────────── */
        if (state == WIFI_MGR_STATE_ERROR) {
            uint32_t backoff = error_backoff_ms(CTX->consecutive_errors);
            xTaskNotifyWait(0, UINT32_MAX, &notif, pdMS_TO_TICKS(backoff));
            if (notif & NOTIFY_STOP) goto cleanup;
            wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
            wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
            continue;
        }

        /* Unknown state — should not happen */
        ESP_LOGE(TAG, "Manager task in unknown state %d — recovering to SCANNING",
                 (int)state);
        wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

cleanup:
    ESP_LOGI(TAG, "Manager task exiting");
    /* stopped_sem given as the absolute last action before vTaskDelete. */
    xSemaphoreGive(CTX->stopped_sem);
    vTaskDelete(NULL);
}

/* ── Public API — Lifecycle ──────────────────────────────────────────────── */

esp_err_t wifi_mgr_init(const wifi_mgr_config_t *config)
{
    if (CTX->initialized) {
        ESP_LOGE(TAG, "wifi_mgr_init: already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Resolve config: caller value or Kconfig default */
    if (config) {
        CTX->config = *config;
    }
    if (CTX->config.known_ap_wait_ms == 0)
        CTX->config.known_ap_wait_ms = CONFIG_WIFI_KNOWN_AP_WAIT_MS;
    if (CTX->config.inter_scan_delay_ms == 0)
        CTX->config.inter_scan_delay_ms = CONFIG_WIFI_INTER_SCAN_DELAY_MS;
    if (CTX->config.connect_timeout_ms == 0)
        CTX->config.connect_timeout_ms = CONFIG_WIFI_CONNECT_TIMEOUT_MS;
    if (CTX->config.prov_timeout_ms == 0)
        CTX->config.prov_timeout_ms = CONFIG_WIFI_PROV_TIMEOUT_MS;
    if (CTX->config.ble_retry_backoff_ms == 0)
        CTX->config.ble_retry_backoff_ms = CONFIG_WIFI_PROV_BLE_RETRY_BACKOFF_MS;
    if (CTX->config.max_connect_attempts == 0)
        CTX->config.max_connect_attempts = CONFIG_WIFI_MAX_CONNECT_ATTEMPTS;
    if (CTX->config.use_count_save_interval == 0)
        CTX->config.use_count_save_interval = CONFIG_WIFI_USE_COUNT_SAVE_INTERVAL;
    if (CTX->config.auto_provision == WIFI_MGR_AUTO_PROV_DEFAULT) {
#if defined(CONFIG_WIFI_AUTO_PROVISION_ALWAYS)
        CTX->config.auto_provision = WIFI_MGR_AUTO_PROV_ALWAYS;
#elif defined(CONFIG_WIFI_AUTO_PROVISION_NEVER)
        CTX->config.auto_provision = WIFI_MGR_AUTO_PROV_NEVER;
#else
        CTX->config.auto_provision = WIFI_MGR_AUTO_PROV_WHEN_UNPROVISIONED;
#endif
    }
    if (CTX->config.device_name_prefix[0] == '\0')
        strncpy(CTX->config.device_name_prefix,
                CONFIG_WIFI_PROV_DEVICE_NAME_PREFIX,
                sizeof(CTX->config.device_name_prefix) - 1);

    /* Initialize ap_records (loads credentials from NVS internally) */
    esp_err_t ret = ap_records_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ap_records_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create FreeRTOS objects — destroy in reverse order on any failure */
    CTX->state_mutex = xSemaphoreCreateMutex();
    if (!CTX->state_mutex) goto oom;

    CTX->event_group = xEventGroupCreate();
    if (!CTX->event_group) goto oom;

    CTX->stopped_sem = xSemaphoreCreateBinary();
    if (!CTX->stopped_sem) goto oom;

    CTX->prov_stop_confirm_sem = xSemaphoreCreateBinary();
    if (!CTX->prov_stop_confirm_sem) goto oom;

    CTX->tw_timer = xTimerCreate("tw_timer",
                                  pdMS_TO_TICKS(CTX->config.known_ap_wait_ms),
                                  pdFALSE, NULL, tw_timer_cb);
    if (!CTX->tw_timer) goto oom;

    CTX->connect_timer = xTimerCreate("connect_timer",
                                       pdMS_TO_TICKS(CTX->config.connect_timeout_ms),
                                       pdFALSE, NULL, connect_timer_cb);
    if (!CTX->connect_timer) goto oom;

    /* prov_timer period irrelevant when timeout == 0 (never started) */
    uint32_t prov_period = (CTX->config.prov_timeout_ms > 0)
                         ? CTX->config.prov_timeout_ms : 1u;
    CTX->prov_timer = xTimerCreate("prov_timer",
                                    pdMS_TO_TICKS(prov_period),
                                    pdFALSE, NULL, prov_timer_cb);
    if (!CTX->prov_timer) goto oom;

    /* Register WiFi and IP event handlers */
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s",
                 esp_err_to_name(ret));
        goto cleanup_timers;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s",
                 esp_err_to_name(ret));
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler);
        goto cleanup_timers;
    }

    CTX->state       = WIFI_MGR_STATE_IDLE;
    CTX->initialized = true;

    ESP_LOGI(TAG, "wifi_manager initialized");
    return ESP_OK;

oom:
    ESP_LOGE(TAG, "wifi_mgr_init: out of memory creating FreeRTOS objects");
    ret = ESP_ERR_NO_MEM;
cleanup_timers:
    if (CTX->prov_timer)    { xTimerDelete(CTX->prov_timer, 0);    CTX->prov_timer    = NULL; }
    if (CTX->connect_timer) { xTimerDelete(CTX->connect_timer, 0); CTX->connect_timer = NULL; }
    if (CTX->tw_timer)      { xTimerDelete(CTX->tw_timer, 0);      CTX->tw_timer      = NULL; }
    if (CTX->prov_stop_confirm_sem) { vSemaphoreDelete(CTX->prov_stop_confirm_sem); CTX->prov_stop_confirm_sem = NULL; }
    if (CTX->stopped_sem)   { vSemaphoreDelete(CTX->stopped_sem);  CTX->stopped_sem   = NULL; }
    if (CTX->event_group)   { vEventGroupDelete(CTX->event_group); CTX->event_group   = NULL; }
    if (CTX->state_mutex)   { vSemaphoreDelete(CTX->state_mutex);  CTX->state_mutex   = NULL; }
    ap_records_deinit();
    return ret;
}

esp_err_t wifi_mgr_start(void)
{
    if (!CTX->initialized) {
        ESP_LOGE(TAG, "wifi_mgr_start: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (CTX->started) {
        ESP_LOGE(TAG, "wifi_mgr_start: already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure wait-cancel bit is clear on a fresh start */
    xEventGroupClearBits(CTX->event_group, BIT_CONNECTED | BIT_WAIT_CANCELLED);

    /* Create the default STA network interface. This must happen before
     * esp_wifi_init() and is the application-visible DHCP client anchor —
     * without it, IP_EVENT_STA_GOT_IP never fires even after successful
     * WiFi association, because lwIP has no netif to run DHCP on. The
     * component owns this netif; callers must NOT create their own STA
     * netif before calling wifi_mgr_start(). */
    CTX->sta_netif = esp_netif_create_default_wifi_sta();
    if (!CTX->sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    /* Initialize WiFi driver */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        wifi_mgr_error_info_t err = {.code = ret, .state = WIFI_MGR_STATE_IDLE};
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err, sizeof(err));
        esp_netif_destroy_default_wifi(CTX->sta_netif);
        CTX->sta_netif = NULL;
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(CTX->sta_netif);
        CTX->sta_netif = NULL;
        wifi_mgr_error_info_t err = {.code = ret, .state = WIFI_MGR_STATE_IDLE};
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err, sizeof(err));
        return ret;
    }

    /* Wipe any STA config the driver kept in its own NVS (nvs.net80211).
     * network_provisioning writes received credentials there (see
     * wifi_mgr_driver_wipe_config() in wifi_mgr_prov.c), so units that
     * provisioned under 0.1.x/0.2.x — or lost power mid-session — carry a
     * stale or unverified password. Done here, after set_mode(STA) (needed
     * for the STA interface) and before esp_wifi_start(), so no connection
     * can be in progress. The driver is still in its default FLASH storage
     * mode at this point; the wipe ends in RAM mode. Only writes when
     * something is stored, so normal boots cost no flash write. */
    {
        wifi_config_t stored;
        memset(&stored, 0, sizeof(stored));
        if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK &&
            stored.sta.ssid[0] != '\0') {
            ESP_LOGI(TAG, "Wiping STA config stored in driver NVS");
            CTX->driver_wipe_pending = (wifi_mgr_driver_wipe_config() != ESP_OK);
        } else {
            CTX->driver_wipe_pending = false;
        }
    }

    /* CRITICAL: ap_records is the sole credential authority.
     * WIFI_STORAGE_RAM keeps esp_wifi_set_config() calls (one per connect
     * attempt) out of flash. Set before esp_wifi_start(). network_provisioning
     * flips this back to FLASH; wifi_mgr_prov.c re-applies RAM afterwards. */
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage(RAM) failed: %s — FATAL",
                 esp_err_to_name(ret));
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(CTX->sta_netif);
        CTX->sta_netif = NULL;
        wifi_mgr_error_info_t err = {.code = ret, .state = WIFI_MGR_STATE_IDLE};
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err, sizeof(err));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(CTX->sta_netif);
        CTX->sta_netif = NULL;
        wifi_mgr_error_info_t err = {.code = ret, .state = WIFI_MGR_STATE_IDLE};
        wifi_mgr_events_post(WIFI_MGR_EVENT_ERROR, &err, sizeof(err));
        return ret;
    }

    /* Build provisioning device name (stored for later use by prov_start) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const char *prefix = CTX->config.device_name_prefix[0] != '\0'
                       ? CTX->config.device_name_prefix
                       : CONFIG_WIFI_PROV_DEVICE_NAME_PREFIX;
    snprintf(CTX->prov_device_name, sizeof(CTX->prov_device_name),
             "%s%02X%02X%02X", prefix, mac[3], mac[4], mac[5]);

    /* Create manager task */
    BaseType_t task_ret = xTaskCreate(manager_task, "wifi_mgr",
                                       CONFIG_WIFI_TASK_STACK_SIZE,
                                       NULL,
                                       CONFIG_WIFI_TASK_PRIORITY,
                                       &CTX->task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(CTX->sta_netif);
        CTX->sta_netif = NULL;
        return ESP_ERR_NO_MEM;
    }

    CTX->started = true;
    wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
    wifi_mgr_events_post(WIFI_MGR_EVENT_STARTED, NULL, 0);

    ESP_LOGI(TAG, "wifi_manager started — device: %s", CTX->prov_device_name);
    return ESP_OK;
}

esp_err_t wifi_mgr_stop(void)
{
    if (!CTX->started) {
        return ESP_OK; /* idempotent */
    }

    /* Step 1 */
    CTX->stop_requested = true;

    /* Step 2: stop provisioning if active */
    if (wifi_mgr_state_get() == WIFI_MGR_STATE_PROVISIONING) {
        wifi_mgr_prov_stop_shutdown();
    }

    /* Step 3: stop any in-progress scan unconditionally */
    esp_wifi_scan_stop();

    /* Steps 4-5: stop timers and disconnect based on current state */
    wifi_mgr_state_t state = wifi_mgr_state_get();
    if (state == WIFI_MGR_STATE_TW_ACTIVE) {
        xTimerStop(CTX->tw_timer, 0);
        xTimerStop(CTX->connect_timer, 0);
        esp_wifi_disconnect();
    } else if (state == WIFI_MGR_STATE_CONNECTED) {
        esp_wifi_disconnect();
    }

    /* Step 6: signal manager task to exit */
    xTaskNotify(CTX->task_handle, NOTIFY_STOP, eSetBits);

    /* Step 7: wait for task to give stopped_sem */
    if (xSemaphoreTake(CTX->stopped_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "wifi_mgr_stop: task did not exit within 2000ms — FATAL. "
                 "Do not call wifi_mgr_start() again; restart the system.");
        return ESP_ERR_TIMEOUT;
    }

    /* Steps 8-9: tear down WiFi radio */
    esp_wifi_stop();
    esp_wifi_deinit();
    if (CTX->sta_netif) {
        esp_netif_destroy_default_wifi(CTX->sta_netif);
        CTX->sta_netif = NULL;
    }

    /* Steps 10-13: reset state */
    CTX->started       = false;
    CTX->stop_requested = false;
    CTX->task_handle   = NULL;
    wifi_mgr_state_set(WIFI_MGR_STATE_IDLE);
    xEventGroupClearBits(CTX->event_group, BIT_CONNECTED | BIT_WAIT_CANCELLED);

    ESP_LOGI(TAG, "wifi_manager stopped");
    return ESP_OK;
}

esp_err_t wifi_mgr_deinit(void)
{
    if (CTX->started) {
        ESP_LOGE(TAG, "wifi_mgr_deinit: still started — call wifi_mgr_stop() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CTX->initialized) {
        return ESP_OK; /* idempotent */
    }

    /* Unregister event handlers */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler);

    /* Delete timers */
    if (CTX->prov_timer)    { xTimerDelete(CTX->prov_timer, 0);    CTX->prov_timer    = NULL; }
    if (CTX->connect_timer) { xTimerDelete(CTX->connect_timer, 0); CTX->connect_timer = NULL; }
    if (CTX->tw_timer)      { xTimerDelete(CTX->tw_timer, 0);      CTX->tw_timer      = NULL; }

    /* Delete synchronization objects */
    if (CTX->state_mutex)   { vSemaphoreDelete(CTX->state_mutex);  CTX->state_mutex   = NULL; }
    if (CTX->event_group)   { vEventGroupDelete(CTX->event_group); CTX->event_group   = NULL; }
    if (CTX->stopped_sem)   { vSemaphoreDelete(CTX->stopped_sem);  CTX->stopped_sem   = NULL; }
    if (CTX->prov_stop_confirm_sem) { vSemaphoreDelete(CTX->prov_stop_confirm_sem); CTX->prov_stop_confirm_sem = NULL; }

    /* Deinit credential store */
    ap_records_deinit();

    CTX->initialized = false;
    ESP_LOGI(TAG, "wifi_manager deinitialized");
    return ESP_OK;
}

/* ── Public API — State queries ──────────────────────────────────────────── */

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    if (!CTX->initialized) return WIFI_MGR_STATE_IDLE;
    return wifi_mgr_state_get();
}

bool wifi_mgr_is_connected(void)
{
    return wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED;
}

bool wifi_mgr_is_provisioned(void)
{
    return ap_records_get_count() > 0;
}

/* wifi_mgr_state_str() is defined in wifi_mgr_state.c */

esp_err_t wifi_mgr_get_connection_info(wifi_mgr_connected_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    if (wifi_mgr_state_get() != WIFI_MGR_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    /* conn_info is only written from the manager task or event handler;
     * read here is safe because CONNECTED state means the last write happened. */
    memcpy(info, &CTX->conn_info, sizeof(wifi_mgr_connected_info_t));
    return ESP_OK;
}

/* ── Public API — Blocking wait ──────────────────────────────────────────── */

esp_err_t wifi_mgr_wait_for_connection(uint32_t timeout_ms)
{
    if (!CTX->initialized) return ESP_ERR_INVALID_STATE;

    /* Fast path: already connected or already cancelled */
    EventBits_t bits = xEventGroupGetBits(CTX->event_group);
    if (bits & BIT_CONNECTED)      return ESP_OK;
    if (bits & BIT_WAIT_CANCELLED) return ESP_ERR_WIFI_MGR_CANCELED;

    bits = xEventGroupWaitBits(
        CTX->event_group,
        BIT_CONNECTED | BIT_WAIT_CANCELLED,
        pdFALSE,  /* do not clear on exit — multiple waiters see the same bits */
        pdFALSE,  /* wait for ANY bit */
        (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)
    );

    if (bits & BIT_CONNECTED)      return ESP_OK;
    if (bits & BIT_WAIT_CANCELLED) return ESP_ERR_WIFI_MGR_CANCELED;
    return ESP_ERR_TIMEOUT;
}

void wifi_mgr_cancel_wait(void)
{
    if (CTX->event_group) {
        xEventGroupSetBits(CTX->event_group, BIT_WAIT_CANCELLED);
    }
}

void wifi_mgr_reset_wait_cancel(void)
{
    if (CTX->event_group) {
        xEventGroupClearBits(CTX->event_group, BIT_WAIT_CANCELLED);
    }
}

/* ── Public API — Provisioning ───────────────────────────────────────────── */

esp_err_t wifi_mgr_start_ble_provisioning(void)
{
    if (!CTX->started) return ESP_ERR_INVALID_STATE;

    wifi_mgr_state_t s = wifi_mgr_state_get();
    if (s == WIFI_MGR_STATE_PROVISIONING) {
        ESP_LOGW(TAG, "start_ble_provisioning: already in PROVISIONING");
        return ESP_ERR_INVALID_STATE;
    }

    if (s == WIFI_MGR_STATE_TW_ACTIVE) {
        xTimerStop(CTX->tw_timer, 0);
        xTimerStop(CTX->connect_timer, 0);
        esp_wifi_disconnect();
    }

    if (s == WIFI_MGR_STATE_CONNECTED) {
        xEventGroupClearBits(CTX->event_group, BIT_CONNECTED);
        esp_wifi_disconnect();
    }

    wifi_mgr_state_set(WIFI_MGR_STATE_PROVISIONING);
    return wifi_mgr_prov_start();
}

esp_err_t wifi_mgr_reset_provisioning(void)
{
    esp_err_t ret = wifi_mgr_stop();
    if (ret != ESP_OK) return ret;

    ap_records_clear_all();
    ret = ap_records_save();
    if (ret != ESP_OK) return ret;

    return wifi_mgr_start();
}

esp_err_t wifi_mgr_get_prov_device_name(char *buf, size_t len)
{
    if (!buf || len < 32) return ESP_ERR_INVALID_ARG;
    if (!CTX->started)    return ESP_ERR_INVALID_STATE;
    strncpy(buf, CTX->prov_device_name, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t wifi_mgr_get_prov_pop(char *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (wifi_mgr_state_get() != WIFI_MGR_STATE_PROVISIONING) {
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(buf, CTX->prov_pop, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}

/* ── Public API — Credential management ──────────────────────────────────── */

esp_err_t wifi_mgr_add_ap(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ap_records_add(ssid, password, NULL);
    if (ret == ESP_OK) {
        ret = ap_records_save();
    }
    return ret;
}

esp_err_t wifi_mgr_remove_ap(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ap_records_remove_by_ssid(ssid);
    if (ret == ESP_OK) {
        ret = ap_records_save();
    }
    return ret;
}

esp_err_t wifi_mgr_get_stored_aps(wifi_mgr_ap_info_t *aps, uint8_t *count)
{
    if (!aps || !count) return ESP_ERR_INVALID_ARG;

    ap_info_t buf[CONFIG_MAX_AP_COUNT];
    uint8_t   n = 0;
    esp_err_t ret = ap_records_get_all(buf, &n);
    if (ret != ESP_OK) return ret;

    *count = n;
    for (uint8_t i = 0; i < n; i++) {
        /* Copy public fields only — password is intentionally omitted */
        strncpy(aps[i].ssid, buf[i].ssid, sizeof(aps[i].ssid) - 1);
        aps[i].ssid[sizeof(aps[i].ssid) - 1] = '\0';
        memcpy(aps[i].bssid, buf[i].bssid, 6);
        aps[i].use_count = buf[i].use_count;
    }
    return ESP_OK;
}
