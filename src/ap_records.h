/**
 * @file ap_records.h
 * @brief Private AP credential store for wifi_manager.
 *
 * VENDORED — local copy, not upstream-tracked.
 *
 * This header is in PRIV_INCLUDE_DIRS. It must NOT be included by any
 * component outside wifi_manager. Other application components that need
 * access to stored credentials must use the wifi_manager public API:
 *   wifi_mgr_get_stored_aps(), wifi_mgr_add_ap(), wifi_mgr_remove_ap().
 *
 * SSID is the primary key. Duplicate SSIDs overwrite existing entries.
 * See L9 in 01_overview.md.
 */

#pragma once

#include "blob_storage.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Struct definitions ──────────────────────────────────────────────────── */

/**
 * @brief Single stored AP credential entry.
 *
 * SECURITY INVARIANT: passwords are NEVER logged at any level.
 * Only SSID, BSSID, and use_count may appear in log output.
 *
 * OTA note (L10): adding, removing, or reordering fields in this struct
 * changes sizeof(ap_info_t), which invalidates the NVS blob (size mismatch
 * causes ap_records_load() to zero all credentials). Add a schema version
 * field before the first production firmware is shipped to enable migration.
 */
typedef struct {
    char     ssid[33];      /**< SSID (null-terminated; max 32 chars) */
    char     password[64];  /**< Password (null-terminated; max 63 chars) */
    uint8_t  bssid[6];     /**< BSSID (all-zeros if not captured at add time) */
    uint32_t use_count;     /**< Successful connection count (saturating) */
} ap_info_t;

/**
 * @brief The full persistent credential store blob.
 *
 * Stored as a single NVS blob under AP_RECORDS_NAMESPACE / AP_RECORDS_KEY.
 * sizeof(ap_record_t) must fit within BLOB_STORAGE_MAX_SUPPORTED_SIZE (4000B).
 * At CONFIG_MAX_AP_COUNT=5: 5 * sizeof(ap_info_t) + 4 ≈ 620B. Well within limit.
 */
typedef struct {
    ap_info_t ap_list[CONFIG_MAX_AP_COUNT]; /**< Credential entries */
    int       available_records;             /**< Count of valid entries */
} ap_record_t;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the AP records module and load credentials from NVS.
 *
 * Creates the internal mutex and calls ap_records_load() internally.
 * Do NOT call ap_records_load() separately after this.
 * Returns ESP_OK even if no credentials are found (first boot).
 */
esp_err_t ap_records_init(void);

/**
 * @brief Release all resources. Must be called only when no task uses ap_records.
 */
esp_err_t ap_records_deinit(void);

/**
 * @brief Load credentials from NVS into the in-RAM store.
 *
 * Called internally by ap_records_init(). Do not call from wifi_manager.
 * Returns ESP_ERR_NOT_FOUND on first boot (no NVS blob yet).
 */
esp_err_t ap_records_load(void);

/**
 * @brief Save the in-RAM store to NVS.
 *
 * Blocks the calling context ~10-20 ms for the NVS write.
 * Must not be called from the system event task or a timer callback.
 * Uses a snapshot+dirty-flag pattern to close the TOCTOU gap (see L7).
 *
 * Returns ESP_ERR_NVS_NOT_ENOUGH_SPACE if the NVS partition is full (L11).
 * On that error, credentials remain correct in RAM but are not persisted.
 */
esp_err_t ap_records_save(void);

/* ── Bulk access ─────────────────────────────────────────────────────────── */

/**
 * @brief Copy the full in-RAM store into caller-supplied buffers.
 *
 * The mutex is held only for the duration of the internal memcpy.
 * The caller owns the data on its own stack and is immune to concurrent
 * add/remove operations.
 *
 * @param[out] out       Caller-allocated array of CONFIG_MAX_AP_COUNT entries.
 * @param[out] out_count Number of valid entries written.
 */
esp_err_t ap_records_get_all(ap_info_t *out, uint8_t *out_count);

/**
 * @brief Overwrite the full in-RAM store from caller-supplied data.
 *
 * For testing and migration only. Does not save to NVS.
 */
esp_err_t ap_records_set_all(const ap_record_t *records);

/* ── Individual access ───────────────────────────────────────────────────── */

/**
 * @brief Add a new credential or update an existing one (matched by SSID).
 *
 * When the store is full, the LRU entry (lowest use_count) is evicted.
 * The new/updated entry receives use_count = max_existing + 1 (saturating).
 *
 * @param ssid      SSID string (max 32 chars).
 * @param password  Password string (max 63 chars).
 * @param bssid     BSSID (6 bytes) or NULL to store all-zeros.
 */
esp_err_t ap_records_add(const char *ssid, const char *password, const uint8_t *bssid);

/**
 * @brief Copy a single entry by index.
 */
esp_err_t ap_records_get(int index, ap_info_t *ap_info);

/**
 * @brief Return the number of stored credentials (-1 if not initialized).
 */
int ap_records_get_count(void);

/**
 * @brief Find a credential entry by SSID. Returns the first match.
 *
 * @param[out] ap_info  Destination struct (may be NULL to check existence only).
 * @param[out] index    Store index of the match (may be NULL).
 * @return ESP_OK or ESP_ERR_NOT_FOUND.
 */
esp_err_t ap_records_find_by_ssid(const char *ssid, ap_info_t *ap_info, int *index);

/**
 * @brief Find a credential entry by BSSID. Returns ESP_ERR_INVALID_ARG for
 *        all-zero BSSID (would match any record stored without a BSSID).
 */
esp_err_t ap_records_find_by_bssid(const uint8_t *bssid, ap_info_t *ap_info, int *index);

/**
 * @brief Increment the use_count for the named AP (saturating at UINT32_MAX).
 */
esp_err_t ap_records_increment_use_count(const char *ssid);

/**
 * @brief Remove a credential by SSID.
 *
 * Sets the internal dirty flag so ap_records_save() can close the TOCTOU gap.
 */
esp_err_t ap_records_remove_by_ssid(const char *ssid);

/**
 * @brief Remove a credential by store index.
 */
esp_err_t ap_records_remove_by_index(int index);

/**
 * @brief Zero all credentials in RAM. Sets dirty flag.
 */
esp_err_t ap_records_clear_all(void);

/**
 * @brief Sort in-RAM store descending by use_count.
 *
 * DIAGNOSTIC / TEST USE ONLY. wifi_manager internals never call this.
 * The connect_list used for connection attempts is sorted from a snapshot
 * at scan time; the persistent sort order has no effect on AP selection.
 */
esp_err_t ap_records_sort_by_usage(void);

/* ── Stats / diagnostics ─────────────────────────────────────────────────── */

/**
 * @brief Get storage stats (used bytes, max bytes).
 */
esp_err_t ap_records_get_stats(const blob_storage_handle_t *handle,
                                size_t *used_size, size_t *max_size);

/**
 * @brief Return sizeof(ap_record_t).
 */
size_t ap_records_get_size(void);

/**
 * @brief Return true if ap_records_init() has been called.
 */
bool ap_records_is_initialized(void);

/**
 * @brief Log all stored entries at INFO level.
 *
 * SECURITY INVARIANT: passwords are NEVER logged. Only SSID, BSSID, use_count.
 */
void ap_records_print_all(void);

/* ── Unsafe access (test/debug only) ────────────────────────────────────────*/

#ifdef AP_RECORDS_ALLOW_UNSAFE_ACCESS
/**
 * @brief Return a raw pointer into the internal store WITHOUT holding the mutex.
 *
 * This symbol must NEVER be defined in production builds. It creates a data
 * race with all mutating operations. Use ap_records_get_all() instead.
 */
const ap_record_t *ap_records_get_readonly(void);
#endif

#ifdef __cplusplus
}
#endif
