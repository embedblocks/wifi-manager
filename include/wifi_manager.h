/**
 * @file wifi_manager.h
 * @brief Public API for the wifi_manager ESP-IDF component.
 *
 * wifi_manager owns the full WiFi lifecycle: scanning, credential-based
 * reconnection, and BLE-based provisioning. Other components observe it
 * through this event/notification interface and never touch esp_wifi directly.
 *
 * Thread safety: all public functions are thread-safe unless noted otherwise.
 * Blocking functions (wifi_mgr_wait_for_connection) must not be called from
 * the default event loop task or from ISR context.
 */

#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Component error codes ──────────────────────────────────────────────── */

/**
 * @brief Base offset for wifi_manager-specific error codes.
 *
 * ESP-IDF's esp_err.h does not define a generic "operation cancelled" error
 * code, so wifi_manager defines its own small error namespace following the
 * standard IDF convention (cf. ESP_ERR_WIFI_BASE, ESP_ERR_MESH_BASE in
 * esp_err.h). Chosen to avoid collision with any IDF or third-party base.
 */
#define ESP_ERR_WIFI_MGR_BASE (0x8000)

/**
 * @brief A blocking wait (e.g. wifi_mgr_wait_for_connection) was cancelled.
 *
 * Returned when wifi_mgr_cancel_wait() is called while one or more tasks
 * are blocked in wifi_mgr_wait_for_connection(), or when wifi_mgr_stop()
 * tears down the manager while waiters are still blocked.
 */
#define ESP_ERR_WIFI_MGR_CANCELED (ESP_ERR_WIFI_MGR_BASE + 1)

/**
 * @brief BLE controller cannot be re-initialized in this boot session.
 *
 * Posted via WIFI_MGR_EVENT_ERROR's wifi_mgr_error_info_t.code, immediately
 * before this component calls esp_restart() on its own. Occurs only under
 * CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS, when BLE provisioning is
 * needed again after a prior successful session already freed the BLE
 * controller (see WIFI_PROV_BT_LIFECYCLE help text in Kconfig). This is a
 * documented ESP-IDF limitation — esp_bt_controller_init() is not
 * supported after a prior esp_bt_controller_deinit() in the same boot —
 * not a transient fault, so this code signals "restarting now to recover"
 * rather than "retry this operation." External consumers cannot prevent
 * the restart; the code exists purely so the event can be logged,
 * forwarded to telemetry, or used to flush state (e.g. an MQTT last-will)
 * in the brief window before the restart actually occurs.
 */
#define ESP_ERR_WIFI_MGR_BT_PERMANENTLY_RELEASED (ESP_ERR_WIFI_MGR_BASE + 2)

/* ── Event base ──────────────────────────────────────────────────────────── */

ESP_EVENT_DECLARE_BASE(WIFI_MGR_EVENT);

/* ── State enum ──────────────────────────────────────────────────────────── */

/**
 * @brief wifi_manager operational state.
 *
 * Note: there is no separate CONNECTING state. Connection attempts happen
 * inside TW_ACTIVE while the tw deadline runs. Callers see scanning, known-AP
 * detection, and connection attempts all as one TW_ACTIVE phase.
 */
typedef enum {
    WIFI_MGR_STATE_IDLE         = 0, /**< Initialized, not started */
    WIFI_MGR_STATE_SCANNING     = 1, /**< No live APs; scanning forever */
    WIFI_MGR_STATE_TW_ACTIVE    = 2, /**< Live APs found; tw deadline running */
    WIFI_MGR_STATE_CONNECTED    = 3, /**< IP obtained; link up */
    WIFI_MGR_STATE_PROVISIONING = 4, /**< BLE provisioning session active */
    WIFI_MGR_STATE_ERROR        = 5, /**< Internal error; auto-recovers to SCANNING */
    WIFI_MGR_STATE_RESCAN_ONCE  = 6, /**< Fast-reconnect: one scan for last AP */
} wifi_mgr_state_t;

/* ── Event ID enum ───────────────────────────────────────────────────────── */

typedef enum {
    WIFI_MGR_EVENT_STARTED              = 0,  /**< wifi_mgr_start() succeeded */
    WIFI_MGR_EVENT_SCANNING             = 1,  /**< Entered/re-entered SCANNING */
    WIFI_MGR_EVENT_LIVE_APS_FOUND       = 2,  /**< Live APs detected; tw timer started
                                                    data → wifi_mgr_scan_result_t */
    WIFI_MGR_EVENT_CONNECTING           = 3,  /**< Attempting connection (once per AP)
                                                    data → wifi_mgr_connect_info_t */
    WIFI_MGR_EVENT_CONNECTED            = 4,  /**< IP obtained; link up
                                                    data → wifi_mgr_connected_info_t */
    WIFI_MGR_EVENT_DISCONNECTED         = 5,  /**< Link dropped; cycle restarting
                                                    data → wifi_mgr_disconnected_info_t */
    WIFI_MGR_EVENT_NO_KNOWN_AP_IN_TW    = 6,  /**< tw expired; no connection made */
    WIFI_MGR_EVENT_PROVISIONING_STARTED = 7,  /**< BLE advertisement active */
    WIFI_MGR_EVENT_PROVISIONING_DONE    = 8,  /**< Credentials received and connected */
    WIFI_MGR_EVENT_PROVISIONING_FAILED  = 9,  /**< Prov timed out or errored */
    WIFI_MGR_EVENT_ERROR                = 10, /**< Internal ESP error
                                                    data → wifi_mgr_error_info_t */
} wifi_mgr_event_id_t;

/* ── Event data structures ───────────────────────────────────────────────── */

/**
 * @brief Posted with WIFI_MGR_EVENT_LIVE_APS_FOUND.
 *
 * total_live_aps    — how many APs the scan found (raw, may exceed SCAN_MAX_RESULTS)
 * known_aps_visible — how many match stored credentials
 *
 * If known_aps_visible > 0, connection attempts are starting immediately.
 * If known_aps_visible == 0, the tw timer is running; provisioning will start
 * if tw expires before a known AP appears in a subsequent scan.
 */
typedef struct {
    uint16_t total_live_aps;
    uint16_t known_aps_visible;
} wifi_mgr_scan_result_t;

/**
 * @brief Posted with WIFI_MGR_EVENT_CONNECTING.
 *
 * Posted once per AP (not once per retry).
 * ap_index is 1-based position in the sorted connect_list.
 */
typedef struct {
    char    ssid[33];   /**< SSID being attempted (null-terminated) */
    uint8_t bssid[6];  /**< BSSID being attempted */
    uint8_t ap_index;  /**< 1-based index in sorted connect_list */
} wifi_mgr_connect_info_t;

/**
 * @brief Posted with WIFI_MGR_EVENT_CONNECTED.
 *
 * Also stored internally; accessible via wifi_mgr_get_connection_info().
 * channel is the primary WiFi channel, populated after GOT_IP via
 * esp_wifi_get_channel(). Stable for the duration of the connection.
 */
typedef struct {
    char     ssid[33];  /**< SSID of connected AP (null-terminated) */
    uint8_t  bssid[6]; /**< BSSID of connected AP */
    uint32_t ip;        /**< IPv4 address, network byte order */
    uint8_t  channel;   /**< Primary WiFi channel (1-13) */
} wifi_mgr_connected_info_t;

/**
 * @brief Posted with WIFI_MGR_EVENT_DISCONNECTED.
 *
 * reason is the raw ESP-IDF wifi_reason_t value. Common values:
 *   WIFI_REASON_BEACON_TIMEOUT        (200) — AP out of range / powered off
 *   WIFI_REASON_AUTH_EXPIRE             (2) — Auth rejected (may be transient)
 *   WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT (15) — Likely wrong password
 *   WIFI_REASON_NO_AP_FOUND           (201) — SSID not visible on reconnect
 *   WIFI_REASON_ASSOC_LEAVE             (8) — Admin disconnect / roaming
 *
 * A single AUTH_EXPIRE or 4WAY_HANDSHAKE_TIMEOUT is not definitive. Let the
 * normal retry cycle run before prompting re-provisioning.
 */
typedef struct {
    char     ssid[33];  /**< SSID of disconnected AP */
    uint8_t  bssid[6]; /**< BSSID of disconnected AP */
    uint16_t reason;    /**< Raw wifi_reason_t value */
} wifi_mgr_disconnected_info_t;

/**
 * @brief Posted with WIFI_MGR_EVENT_ERROR.
 *
 * The component auto-recovers to SCANNING after a backoff delay.
 * consecutive_errors grows until the first successful scan resets it to 0.
 * Backoff: 1-2 errors=5s, 3-5=10s, 6-9=30s, 10+=60s.
 */
typedef struct {
    esp_err_t        code;               /**< ESP error code */
    wifi_mgr_state_t state;              /**< State where error occurred */
    uint8_t          consecutive_errors; /**< Count since last successful scan */
} wifi_mgr_error_info_t;

/**
 * @brief Configuration for wifi_mgr_init().
 *
 * Set any field to 0 / NULL to use the Kconfig default.
 * Stored internally; the caller does not need to keep the struct alive.
 */
typedef struct {
    uint32_t known_ap_wait_ms;        /**< tw duration (0 → Kconfig default) */
    uint32_t inter_scan_delay_ms;     /**< Delay between scans (0 → Kconfig) */
    uint32_t connect_timeout_ms;      /**< Per-AP connect timeout (0 → Kconfig) */
    uint32_t prov_timeout_ms;         /**< Prov session timeout (0 → Kconfig) */
    uint32_t ble_retry_backoff_ms;    /**< Backoff before retrying a synchronously
                                         *  failed network_prov_mgr_start_provisioning()
                                         *  call (0 → Kconfig). Deliberately separate
                                         *  from inter_scan_delay_ms — this paces BLE
                                         *  provisioning error recovery, not WiFi
                                         *  scanning, and the two have no reason to
                                         *  share a tuning knob. Only exercised on the
                                         *  rare path where the in-place provisioning
                                         *  restart itself fails synchronously; not
                                         *  used on the common timeout-driven retry. */
    uint8_t  max_connect_attempts;    /**< Retries per AP (0 → Kconfig) */
    uint8_t  use_count_save_interval; /**< Saves per N connections (0 → Kconfig) */
    char     device_name_prefix[16];  /**< BLE name prefix (empty → Kconfig) */
} wifi_mgr_config_t;

/* ── ap_info_t forward declaration for stored-AP queries ─────────────────── */
/* Full definition is in src/ap_records.h (private). This forward is only
 * needed if the application needs to receive stored AP data via
 * wifi_mgr_get_stored_aps(). Include ap_records.h directly in that case —
 * but only through the wifi_mgr public API wrapper, not by including the
 * private header. The struct below is the minimal public definition.       */

/**
 * @brief Public view of a stored AP credential entry.
 *
 * password field is NOT included: passwords are never exposed through the
 * public API. Use wifi_mgr_add_ap() to add credentials and
 * wifi_mgr_remove_ap() to remove them.
 */
typedef struct {
    char     ssid[33];    /**< SSID (null-terminated) */
    uint8_t  bssid[6];   /**< BSSID (all-zeros if not captured at add time) */
    uint32_t use_count;   /**< Successful connection count */
} wifi_mgr_ap_info_t;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the wifi_manager component.
 *
 * Creates all FreeRTOS primitives. Does NOT start the WiFi radio.
 * Calls ap_records_init() internally (do NOT call it separately).
 * nvs_flash_init() must have been called by the application before this.
 *
 * @param config  Runtime config overrides. NULL → all Kconfig defaults.
 * @return ESP_OK or ESP_ERR_INVALID_STATE (already initialized) or ESP_ERR_NO_MEM.
 */
esp_err_t wifi_mgr_init(const wifi_mgr_config_t *config);

/**
 * @brief Start the WiFi radio and manager task.
 *
 * Starts the scan/connect/provision cycle. Posts WIFI_MGR_EVENT_STARTED on
 * success.
 *
 * @return ESP_OK or ESP_ERR_INVALID_STATE (not initialized or already started).
 */
esp_err_t wifi_mgr_start(void);

/**
 * @brief Stop the manager and tear down the WiFi radio cleanly.
 *
 * Blocks until the manager task exits (up to 2000 ms). Returns
 * ESP_ERR_TIMEOUT if the task does not exit within the deadline — this is
 * a fatal state; do NOT call wifi_mgr_start() again. Restart the system.
 *
 * Must not be called from the default event loop task (deadlock).
 *
 * @return ESP_OK or ESP_ERR_TIMEOUT.
 */
esp_err_t wifi_mgr_stop(void);

/**
 * @brief Free all resources allocated by wifi_mgr_init().
 *
 * Must be called only when the manager is stopped (state == IDLE).
 *
 * @return ESP_OK or ESP_ERR_INVALID_STATE (still started).
 */
esp_err_t wifi_mgr_deinit(void);

/* ── State queries ───────────────────────────────────────────────────────── */

/**
 * @brief Return the current operational state (thread-safe).
 */
wifi_mgr_state_t wifi_mgr_get_state(void);

/**
 * @brief Return true if currently connected (state == CONNECTED).
 */
bool wifi_mgr_is_connected(void);

/**
 * @brief Return true if at least one AP credential is stored.
 */
bool wifi_mgr_is_provisioned(void);

/**
 * @brief Human-readable name for a state value.
 *
 * @return Non-null string; "UNKNOWN" for out-of-range values.
 */
const char *wifi_mgr_state_str(wifi_mgr_state_t state);

/**
 * @brief Copy current connection info (SSID / BSSID / IP / channel).
 *
 * Thread-safe. Valid only when state == CONNECTED; returns
 * ESP_ERR_INVALID_STATE otherwise.
 *
 * Prefer the event data in WIFI_MGR_EVENT_CONNECTED handlers; use this
 * from non-handler tasks that need to poll current connection details.
 *
 * @param[out] info  Destination struct; must not be NULL.
 */
esp_err_t wifi_mgr_get_connection_info(wifi_mgr_connected_info_t *info);

/* ── Blocking wait API ───────────────────────────────────────────────────── */

/**
 * @brief Block until connected, timed out, or cancelled.
 *
 * Returns immediately if already connected or if cancel is active.
 * Must not be called from the default event loop task.
 *
 * @param timeout_ms  0 = wait forever; >0 = max wait in ms.
 * @return ESP_OK (connected), ESP_ERR_TIMEOUT, or ESP_ERR_WIFI_MGR_CANCELED.
 */
esp_err_t wifi_mgr_wait_for_connection(uint32_t timeout_ms);

/**
 * @brief Unblock all current and future waiters with ESP_ERR_WIFI_MGR_CANCELED.
 *
 * The cancel flag persists until wifi_mgr_start(), wifi_mgr_stop(), or
 * wifi_mgr_reset_wait_cancel() clears it. Does NOT stop the WiFi manager.
 */
void wifi_mgr_cancel_wait(void);

/**
 * @brief Clear the cancel flag set by wifi_mgr_cancel_wait().
 *
 * Subsequent calls to wifi_mgr_wait_for_connection() will block normally.
 * Does not affect BIT_CONNECTED.
 */
void wifi_mgr_reset_wait_cancel(void);

/* ── Provisioning API ────────────────────────────────────────────────────── */

/**
 * @brief Immediately drop the current connection (if any) and start BLE provisioning.
 *
 * Valid from any state except PROVISIONING (returns ESP_ERR_INVALID_STATE).
 * The tw timer is not involved — provisioning starts unconditionally.
 * Use from a factory-reset GPIO handler, a button ISR task, or similar.
 *
 * @return ESP_OK or error.
 */
esp_err_t wifi_mgr_start_ble_provisioning(void);

/**
 * @brief Erase all stored credentials and restart the scan/provision cycle.
 *
 * Equivalent to: wifi_mgr_stop() → clear all APs → wifi_mgr_start().
 * The next boot cycle will trigger BLE provisioning (no stored APs).
 *
 * @return ESP_OK or error from stop/save/start.
 */
esp_err_t wifi_mgr_reset_provisioning(void);

/**
 * @brief Copy the BLE device name used for provisioning advertisement.
 *
 * Available after wifi_mgr_start() succeeds. The name is derived from the
 * configured prefix + the last 3 bytes of the WiFi STA eFuse MAC.
 *
 * @param[out] buf  Destination buffer (null-terminated on return).
 * @param[in]  len  Buffer length in bytes (must be >= 32).
 * @return ESP_OK or ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_ARG.
 */
esp_err_t wifi_mgr_get_prov_device_name(char *buf, size_t len);

/**
 * @brief Copy the PoP string for the current provisioning session.
 *
 * Available only during an active PROVISIONING session.
 * For SECURITY_0, returns an empty string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  len  Buffer length.
 * @return ESP_OK or ESP_ERR_INVALID_STATE.
 */
esp_err_t wifi_mgr_get_prov_pop(char *buf, size_t len);

/* ── Credential management API ───────────────────────────────────────────── */

/**
 * @brief Add or update an AP credential.
 *
 * If an entry with the same SSID exists, it is updated in place.
 * If the store is full, the least-recently-used entry is evicted (LRU).
 * Calls ap_records_save() synchronously before returning.
 * Blocks the calling task for ~10-20 ms during the NVS write.
 *
 * SSID is the primary key — duplicate SSIDs overwrite existing entries.
 * See L9 in 01_overview.md.
 *
 * @param ssid      SSID string (null-terminated; max 32 chars).
 * @param password  Password string (null-terminated; max 63 chars).
 * @return ESP_OK or error.
 */
esp_err_t wifi_mgr_add_ap(const char *ssid, const char *password);

/**
 * @brief Remove an AP credential by SSID.
 *
 * Calls ap_records_save() synchronously.
 *
 * @param ssid  SSID of the entry to remove.
 * @return ESP_OK or ESP_ERR_NOT_FOUND.
 */
esp_err_t wifi_mgr_remove_ap(const char *ssid);

/**
 * @brief Copy all stored AP entries (SSID, BSSID, use_count — no passwords).
 *
 * @param[out] aps    Destination array; caller allocates CONFIG_MAX_AP_COUNT entries.
 * @param[out] count  Number of entries written.
 * @return ESP_OK or error.
 */
esp_err_t wifi_mgr_get_stored_aps(wifi_mgr_ap_info_t *aps, uint8_t *count);

#ifdef __cplusplus
}
#endif
