/**
 * @file wifi_mgr_state.h
 * @brief Thread-safe state get/set for wifi_manager.
 *
 * Private — do not include outside wifi_manager.
 *
 * Only wifi_manager.c (the manager task) and wifi_mgr_prov.c (for the
 * PROVISIONING transition in wifi_mgr_prov_start()) may call wifi_mgr_state_set().
 * Sub-modules (scan, connect) must not change state directly.
 */

#pragma once

#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the current state (thread-safe, uses state_mutex).
 */
wifi_mgr_state_t wifi_mgr_state_get(void);

/**
 * @brief Set the current state (thread-safe, uses state_mutex).
 *
 * Also clears CTX->scan_truncation_warned so the truncation warning fires
 * once per new SCANNING or TW_ACTIVE entry.
 * Does NOT post any events — caller is responsible for posting events.
 */
void wifi_mgr_state_set(wifi_mgr_state_t new_state);

/**
 * @brief Return a human-readable name for a state value.
 *
 * @return Non-null string; "UNKNOWN" for out-of-range values.
 */
const char *wifi_mgr_state_str(wifi_mgr_state_t state);

#ifdef __cplusplus
}
#endif
