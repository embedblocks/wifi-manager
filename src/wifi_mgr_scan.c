/**
 * @file wifi_mgr_scan.c
 * @brief WiFi scan sub-module implementation for wifi_manager.
 *
 * See 06_scan_flow.md for the authoritative description of each function.
 */

#include "wifi_mgr_scan.h"
#include "wifi_mgr_ctx.h"
#include "wifi_mgr_state.h"
#include "wifi_mgr_events.h"
#include "ap_records.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "string.h"

static const char *TAG = "wifi_mgr";

/* ── AP matching ─────────────────────────────────────────────────────────── */

/**
 * @brief Determine whether a visible AP matches a stored credential.
 *
 * Priority:
 *   1. BSSID exact match (6-byte) — if stored BSSID is non-zero.
 *   2. SSID string match — fallback for zero-BSSID records and BSSID changes
 *      (roaming controller, MAC randomization, hardware replacement).
 *
 * The BSSID fallback to SSID prevents permanent reconnect failure when an
 * AP's BSSID changes while its SSID and password are unchanged.
 */
static bool ap_matches(const ap_info_t *stored, const wifi_ap_record_t *visible)
{
    static const uint8_t zero_bssid[6] = {0};

    if (memcmp(stored->bssid, zero_bssid, 6) != 0) {
        if (memcmp(stored->bssid, visible->bssid, 6) == 0) {
            return true;
        }
        /* BSSID mismatch — AP may have changed BSSID. Fall through to SSID. */
    }

    return strcmp(stored->ssid, (const char *)visible->ssid) == 0;
}

/* ── Insertion sort for connect_list (descending use_count) ──────────────── */

static void sort_known_list_by_use_count(ap_info_t *list, uint8_t count)
{
    for (int i = 1; i < count; i++) {
        ap_info_t key = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].use_count < key.use_count) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
}

/* ── Public functions ────────────────────────────────────────────────────── */

esp_err_t wifi_mgr_scan_start(void)
{
    wifi_scan_config_t scan_cfg = {
#ifdef CONFIG_WIFI_SCAN_HIDDEN
        .show_hidden = true,
#else
        .show_hidden = false,
#endif
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, /*block=*/false);
    /* Returns ESP_OK or error. Never waits. Caller must NOT enter
     * xTaskNotifyWait expecting SCAN_DONE if this returns an error. */
    return ret;
}

bool wifi_mgr_scan_handle_results(void)
{
    wifi_mgr_state_t state = wifi_mgr_state_get();

    /* Step 1: fetch result count */
    uint16_t fetched = 0;
    esp_wifi_scan_get_ap_num(&fetched);

    uint16_t count = (fetched > CONFIG_WIFI_SCAN_MAX_RESULTS)
                   ? CONFIG_WIFI_SCAN_MAX_RESULTS
                   : fetched;

    /* Truncation warning — once per SCANNING or TW_ACTIVE entry.
     * CTX->scan_truncation_warned is cleared by wifi_mgr_state_set(). */
    if (fetched > CONFIG_WIFI_SCAN_MAX_RESULTS && !CTX->scan_truncation_warned) {
        ESP_LOGW(TAG, "Scan results truncated: %u APs visible, only %u fetched "
                 "(CONFIG_WIFI_SCAN_MAX_RESULTS=%u). Credentials for APs ranked "
                 "below position %u by signal strength are not evaluated this "
                 "cycle. Raise CONFIG_WIFI_SCAN_MAX_RESULTS if this is the "
                 "target deployment environment. See L2 in 01_overview.md.",
                 fetched, count, CONFIG_WIFI_SCAN_MAX_RESULTS,
                 CONFIG_WIFI_SCAN_MAX_RESULTS);
        CTX->scan_truncation_warned = true;
    }

    /* Step 2: fetch records into stack buffer */
    wifi_ap_record_t scan_buf[CONFIG_WIFI_SCAN_MAX_RESULTS];
    memset(scan_buf, 0, sizeof(scan_buf));

    uint16_t actual = count;
    esp_wifi_scan_get_ap_records(&actual, scan_buf);
    /* actual now holds how many records were written */
    count = actual;

    /* Step 3: no live APs */
    if (count == 0) {
        if (state == WIFI_MGR_STATE_TW_ACTIVE) {
            xTimerStop(CTX->tw_timer, 0);
            wifi_mgr_state_set(WIFI_MGR_STATE_SCANNING);
            wifi_mgr_events_post(WIFI_MGR_EVENT_SCANNING, NULL, 0);
            return true; /* state changed */
        }
        /* state == SCANNING: no change */
        return false;
    }

    /* Step 4: count > 0 — at least one live AP visible.
     * Snapshot stored credentials into a local stack buffer.
     * ap_records_get_all() holds the mutex only for the memcpy.
     * From this point the manager task owns the data; concurrent
     * add/remove calls from other tasks cannot affect this copy. */
    ap_info_t stored_snapshot[CONFIG_MAX_AP_COUNT];
    uint8_t   stored_count = 0;
    ap_records_get_all(stored_snapshot, &stored_count);

    /* Build known_list: stored APs that match a visible AP */
    ap_info_t known_list[CONFIG_MAX_AP_COUNT];
    uint8_t   known_count = 0;

    static const uint8_t zero_bssid[6] = {0};
    for (uint8_t i = 0; i < stored_count; i++) {
        for (uint16_t j = 0; j < count; j++) {
            if (ap_matches(&stored_snapshot[i], &scan_buf[j])) {
                known_list[known_count] = stored_snapshot[i];

                /* BSSID lock only when that exact BSSID is in this scan.
                 * In a mesh / multi-AP home the stored radio may be off;
                 * locking to it would waste the first attempt. The
                 * zeroing applies to this connect_list copy only — the
                 * stored record keeps its BSSID. */
                ap_info_t *k = &known_list[known_count];
                if (memcmp(k->bssid, zero_bssid, 6) != 0) {
                    bool bssid_seen = false;
                    for (uint16_t m = 0; m < count; m++) {
                        if (memcmp(k->bssid, scan_buf[m].bssid, 6) == 0) {
                            bssid_seen = true;
                            break;
                        }
                    }
                    if (!bssid_seen) {
                        memset(k->bssid, 0, sizeof(k->bssid));
                    }
                }
                known_count++;
                break; /* don't add the same stored AP twice */
            }
        }
    }

    /* Step 5: build and post scan result event.
     * Report the raw fetched count (not capped) for accurate telemetry. */
    wifi_mgr_scan_result_t result = {
        .total_live_aps    = fetched,
        .known_aps_visible = known_count,
    };

    /* Step 6: known APs found → populate connect_list, begin connecting */
    if (known_count > 0) {
        /* Sort by use_count DESC so the most-used AP is tried first */
        sort_known_list_by_use_count(known_list, known_count);

        memcpy(CTX->connect_list, known_list, known_count * sizeof(ap_info_t));
        CTX->connect_list_count = known_count;
        CTX->connect_list_index = 0;
        CTX->connect_retry      = 0;

        /* Start tw_timer exactly once per TW_ACTIVE entry.
         * Do NOT restart if already TW_ACTIVE — that would extend the hard deadline. */
        if (state == WIFI_MGR_STATE_SCANNING) {
            wifi_mgr_tw_window_reset();
            xTimerStart(CTX->tw_timer, 0);
            wifi_mgr_state_set(WIFI_MGR_STATE_TW_ACTIVE);
        }
        /* Remembered for tw expiry: KNOWN_AP_UNREACHABLE vs NO_KNOWN_AP_IN_TW */
        CTX->tw_known_seen = true;

        wifi_mgr_events_post(WIFI_MGR_EVENT_LIVE_APS_FOUND,
                             &result, sizeof(result));
        return true; /* state changed or connect_list updated */
    }

    /* Step 7: live APs exist but none are known */
    wifi_mgr_events_post(WIFI_MGR_EVENT_LIVE_APS_FOUND,
                         &result, sizeof(result));

    if (state == WIFI_MGR_STATE_SCANNING) {
        wifi_mgr_tw_window_reset();
        xTimerStart(CTX->tw_timer, 0);
        wifi_mgr_state_set(WIFI_MGR_STATE_TW_ACTIVE);
        return true; /* state changed */
    }

    /* state == TW_ACTIVE, no known APs yet: tw_timer still running.
     * Caller will wait inter_scan_delay_ms then rescan. */
    return false;
}
