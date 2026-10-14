/**
 * @file wifi_mgr_prov.h
 * @brief BLE provisioning sub-module for wifi_manager.
 *
 * Wraps network_prov_mgr. Translates provisioning library events into
 * NOTIFY_PROV_DONE / NOTIFY_PROV_FAILED task notifications.
 *
 * Constraints:
 *  - prov event handlers must NOT block
 *  - ap_records_save() is deferred to on_prov_done() in the manager task
 *    (not called from the system event task in the CRED_RECV handler)
 *  - network_prov_mgr_deinit() is only ever called from prov_mgr_safe_deinit(),
 *    itself only called from wifi_mgr_prov_stop_succeeded() (conditionally,
 *    under CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS) and
 *    wifi_mgr_prov_stop_shutdown() (always) — never from
 *    wifi_mgr_prov_stop_failed_attempt()
 *  - NETWORK_PROV_END handler is a confirmed no-op (see 08_provisioning_flow.md)
 *  - wifi_mgr_prov_start() retries the existing session in place (no
 *    init/deinit cycle) whenever CTX->prov_mgr_initialized is already
 *    true on entry — this is what lets failed/timed-out attempts retry
 *    indefinitely without hitting the esp_bt_controller_init()-after-deinit
 *    wall documented by Espressif. See CONFIG_WIFI_PROV_BT_LIFECYCLE.
 *
 * Private — do not include outside wifi_manager.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize (or, if a session already exists, restart in place)
 *        BLE provisioning.
 *
 * On first call in a boot: builds device name from config prefix + MAC
 * suffix, resolves auth mode from CONFIG_WIFI_PROV_AUTH_MODE_*, calls
 * network_prov_mgr_init() + network_prov_mgr_start_provisioning().
 *
 * On subsequent calls where CTX->prov_mgr_initialized is already true
 * (i.e. the prior session was stopped via
 * wifi_mgr_prov_stop_failed_attempt(), not deinitialized): skips
 * network_prov_mgr_init() and calls network_prov_mgr_start_provisioning()
 * directly, reusing the device name / PoP / security mode computed on
 * the first call. This is the retry-in-place path that avoids the BT
 * controller deinit/reinit cycle.
 *
 * On a network_prov_mgr_init() failure: if CTX->bt_mem_freed is true (BT
 * memory was previously released by wifi_mgr_prov_stop_succeeded() under
 * CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS), this is the documented
 * unrecoverable-in-this-boot case — posts WIFI_MGR_EVENT_ERROR with
 * ESP_ERR_WIFI_MGR_BT_PERMANENTLY_RELEASED and calls esp_restart()
 * (does not return). Otherwise recovers state to SCANNING and posts
 * WIFI_MGR_EVENT_ERROR as before.
 *
 * Must be called from the manager task only.
 *
 * @return ESP_OK or error. May not return at all (esp_restart() case).
 */
esp_err_t wifi_mgr_prov_start(void);

/**
 * @brief Stop a provisioning session that failed or timed out.
 *
 * Stops the prov_timer and calls network_prov_mgr_stop_provisioning(), but
 * deliberately does NOT deinitialize network_prov_mgr or the BLE controller.
 * This keeps the session alive so wifi_mgr_prov_start()'s retry-in-place
 * branch can restart it directly on the next attempt, indefinitely,
 * without ever exercising the BT controller deinit/reinit cycle. Behavior
 * is identical under both CONFIG_WIFI_PROV_BT_LIFECYCLE policies — they
 * only differ on the success path (see wifi_mgr_prov_stop_succeeded()).
 *
 * Also waits (bounded — see WIFI_PROV_STOP_CONFIRM_WAIT_MS in
 * wifi_mgr_prov.c) for confirmation that the underlying NimBLE GAP
 * teardown has actually finished before returning, since
 * network_prov_mgr_stop_provisioning() does NOT wait for this itself despite
 * running in blocking mode (real-hardware-discovered — see file header
 * fix #3 in wifi_mgr_prov.c). This closes the common case of the
 * caller's subsequent retry-in-place start call racing the teardown and
 * getting ESP_ERR_INVALID_STATE.
 *
 * Call this from on_prov_failed() only.
 *
 * @return true if a stop was detected (NOTIFY_STOP or CTX->stop_requested)
 *         while waiting for teardown confirmation — the caller must
 *         propagate this upward so the manager task's dispatch loop can
 *         goto cleanup, exactly as on_prov_failed() already does for its
 *         own backoff wait. false in the normal case, including when the
 *         bounded wait times out without seeing confirmation (the caller
 *         proceeds to retry anyway in that case; see fix #3).
 */
bool wifi_mgr_prov_stop_failed_attempt(void);

/**
 * @brief Stop a provisioning session that completed successfully.
 *
 * Stops the prov_timer and calls network_prov_mgr_stop_provisioning().
 * Whether prov_mgr_safe_deinit() (and BT memory release) follows depends
 * on CONFIG_WIFI_PROV_BT_LIFECYCLE:
 *  - FREE_ON_SUCCESS: deinitializes and sets CTX->bt_mem_freed = true.
 *  - KEEP_ALIVE (default): leaves the session initialized.
 *
 * Call this from on_prov_done() only.
 */
void wifi_mgr_prov_stop_succeeded(void);

/**
 * @brief Stop provisioning unconditionally as part of full manager shutdown.
 *
 * Stops the prov_timer, calls network_prov_mgr_stop_provisioning(), and
 * always calls prov_mgr_safe_deinit() (idempotent) regardless of
 * CONFIG_WIFI_PROV_BT_LIFECYCLE policy or whether the most recent attempt
 * succeeded or failed — there is no "next attempt" to preserve state for,
 * since the whole manager is being torn down. Safe to call even if
 * provisioning is not active.
 *
 * Call this from wifi_mgr_stop() only.
 */
void wifi_mgr_prov_stop_shutdown(void);

/**
 * @brief Wipe the provisioned credential held in RAM (password zeroed with
 *        a non-elidable write). Manager task / stop paths only.
 */
void wifi_mgr_prov_wipe_held_creds(void);

/**
 * @brief Put the Wi-Fi driver back in WIFI_STORAGE_RAM.
 *
 * network_provisioning leaves it in WIFI_STORAGE_FLASH after every
 * start_provisioning() call and after receiving credentials. Only affects
 * where future esp_wifi_set_config() calls are written; safe while connected.
 */
void wifi_mgr_driver_storage_ram(void);

/**
 * @brief Overwrite the driver's NVS-stored STA config with an empty one,
 *        ending in WIFI_STORAGE_RAM.
 *
 * Call only while the station is not connected or connecting (after a
 * failed provisioning session, or in wifi_mgr_start() before
 * esp_wifi_start()). Failure is logged and non-fatal.
 */
esp_err_t wifi_mgr_driver_wipe_config(void);

#ifdef __cplusplus
}
#endif

