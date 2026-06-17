/**
 * @file wifi_mgr_prov.h
 * @brief BLE provisioning sub-module for wifi_manager.
 *
 * Wraps wifi_prov_mgr. Translates provisioning library events into
 * NOTIFY_PROV_DONE / NOTIFY_PROV_FAILED task notifications.
 *
 * Constraints:
 *  - prov event handlers must NOT block
 *  - ap_records_save() is deferred to on_prov_done() in the manager task
 *    (not called from the system event task in the CRED_RECV handler)
 *  - wifi_prov_mgr_deinit() is only ever called from prov_mgr_safe_deinit()
 *  - WIFI_PROV_END handler is a confirmed no-op (see 08_provisioning_flow.md)
 *
 * Private — do not include outside wifi_manager.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start BLE provisioning.
 *
 * Builds device name from config prefix + MAC suffix.
 * Resolves auth mode from the CONFIG_WIFI_PROV_AUTH_MODE_* choice.
 * On failure, recovers state to SCANNING and posts WIFI_MGR_EVENT_ERROR.
 *
 * Must be called from the manager task only.
 *
 * @return ESP_OK or error.
 */
esp_err_t wifi_mgr_prov_start(void);

/**
 * @brief Stop an active provisioning session.
 *
 * Stops the prov_timer, calls wifi_prov_mgr_stop_provisioning(), and
 * calls prov_mgr_safe_deinit() (idempotent). Safe to call even if
 * provisioning is not active.
 */
void wifi_mgr_prov_stop(void);

#ifdef __cplusplus
}
#endif
