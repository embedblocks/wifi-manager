/**
 * @file wifi_mgr_connect.h
 * @brief WiFi connection sub-module for wifi_manager.
 *
 * Handles the mechanics of initiating a connection attempt:
 *  - BSSID lock on first retry; SSID-only on subsequent retries
 *  - connect_timer arming
 *  - CONNECTING event posting (once per AP, not per retry)
 *
 * This module must NEVER block. All blocking lives in the manager task loop.
 * State transitions (CONNECTED, SCANNING) are handled by the manager task
 * calling on_connect_success() / on_connect_failure() / on_disconnect()
 * which are static in wifi_manager.c.
 *
 * Private — do not include outside wifi_manager.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start a connection attempt to the current AP in CTX->connect_list.
 *
 * Uses CTX->connect_list_index to select the AP and CTX->connect_retry
 * to determine BSSID lock (retry == 0 → locked, retry >= 1 → SSID-only).
 *
 * Posts WIFI_MGR_EVENT_CONNECTING only when connect_retry == 0.
 * Arms the connect_timer.
 *
 * Must be called from the manager task only.
 * Must not block.
 */
void wifi_mgr_connect_attempt(void);

#ifdef __cplusplus
}
#endif
