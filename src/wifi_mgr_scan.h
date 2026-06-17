/**
 * @file wifi_mgr_scan.h
 * @brief WiFi scan sub-module for wifi_manager.
 *
 * Responsibilities:
 *  - Start a non-blocking scan
 *  - Fetch and process results; compare against ap_records
 *  - Populate CTX->connect_list when known APs are found
 *  - Drive state transitions: SCANNING ↔ TW_ACTIVE
 *  - Post WIFI_MGR_EVENT_LIVE_APS_FOUND and WIFI_MGR_EVENT_SCANNING
 *
 * Constraints (cross-cutting):
 *  - Sub-module functions must NEVER block (no xTaskNotifyWait, no vTaskDelay)
 *  - State transitions are driven by returning bool to the manager task;
 *    the manager task calls wifi_mgr_state_set() — this module does not
 *    (exception: tw_timer start is handled here per 06_scan_flow.md)
 *
 * Private — do not include outside wifi_manager.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start a non-blocking WiFi scan.
 *
 * Returns ESP_OK or an error. If it returns an error, the manager task must
 * NOT enter xTaskNotifyWait expecting NOTIFY_SCAN_DONE — the notification
 * will never arrive and the task would block indefinitely.
 *
 * Uses CONFIG_WIFI_SCAN_HIDDEN for show_hidden.
 */
esp_err_t wifi_mgr_scan_start(void);

/**
 * @brief Process scan results after receiving NOTIFY_SCAN_DONE.
 *
 * Fetches results from the WiFi driver, compares against stored credentials,
 * populates CTX->connect_list, and drives SCANNING ↔ TW_ACTIVE transitions.
 *
 * @return true if state changed (or connect_list was updated in TW_ACTIVE),
 *         false if no state change occurred.
 */
bool wifi_mgr_scan_handle_results(void);

#ifdef __cplusplus
}
#endif
