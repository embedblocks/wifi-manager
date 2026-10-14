/**
 * @file wifi_mgr_connect.c
 * @brief WiFi connection attempt sub-module for wifi_manager.
 *
 * See 07_connect_flow.md and the implementation plan Phase 4 for details.
 */

#include "wifi_mgr_connect.h"
#include "wifi_mgr_ctx.h"
#include "wifi_mgr_events.h"
#include "wifi_mgr_prov.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "string.h"

static const char *TAG = "wifi_mgr";

void wifi_mgr_connect_attempt(void)
{
    const ap_info_t *ap = &CTX->connect_list[CTX->connect_list_index];

    /* Safe point for a deferred wipe of an unverified password left in the
     * driver's NVS by a failed provisioning session: the station is not
     * connected here, and this is normally seconds after the failure — the
     * only safe point that comes soon on a device that then resumes normal
     * operation for weeks. Costs nothing when nothing is pending. */
    wifi_mgr_driver_wipe_if_pending();

    /* Build wifi_config from stored credential */
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));

    strncpy((char *)wifi_cfg.sta.ssid, ap->ssid,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, ap->password,
            sizeof(wifi_cfg.sta.password) - 1);

    /* BSSID lock: first attempt uses stored BSSID (if non-zero).
     * Subsequent retries fall back to SSID-only to tolerate BSSID changes
     * (roaming controller, MAC randomization, hardware replacement). */
    static const uint8_t zero_bssid[6] = {0};
    if (CTX->connect_retry == 0 &&
        memcmp(ap->bssid, zero_bssid, 6) != 0) {
        wifi_cfg.sta.bssid_set = true;
        memcpy(wifi_cfg.sta.bssid, ap->bssid, 6);
    } else {
        wifi_cfg.sta.bssid_set = false;
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        /* Still arm connect_timer: the manager task waits for an outcome
         * with portMAX_DELAY, and without the timer this attempt would
         * never produce one. The timeout then takes the normal failure
         * path (retry / next AP). */
        xTimerChangePeriod(CTX->connect_timer,
                           pdMS_TO_TICKS(CTX->config.connect_timeout_ms), 0);
        xTimerStart(CTX->connect_timer, 0);
        return;
    }

    /* Post CONNECTING event once per AP (not once per retry) */
    if (CTX->connect_retry == 0) {
        wifi_mgr_connect_info_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.ssid, ap->ssid, sizeof(info.ssid) - 1);
        memcpy(info.bssid, ap->bssid, 6);
        info.ap_index = (uint8_t)(CTX->connect_list_index + 1); /* 1-based */
        wifi_mgr_events_post(WIFI_MGR_EVENT_CONNECTING, &info, sizeof(info));
        ESP_LOGI(TAG, "Connecting to AP #%u: %s (BSSID lock: %s)",
                 info.ap_index, ap->ssid,
                 wifi_cfg.sta.bssid_set ? "yes" : "no");
    } else {
        ESP_LOGD(TAG, "Retry %u/%u for AP: %s",
                 CTX->connect_retry + 1,
                 CTX->config.max_connect_attempts,
                 ap->ssid);
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        /* connect_timer still armed below — timeout will trigger failure path */
    }

    /* Arm connect_timer. On expiry → NOTIFY_CONNECT_TIMEOUT → on_connect_failure() */
    xTimerChangePeriod(CTX->connect_timer,
                       pdMS_TO_TICKS(CTX->config.connect_timeout_ms),
                       0);
    xTimerStart(CTX->connect_timer, 0);
}
