/**
 * @file wifi_mgr_ctx.h
 * @brief Internal shared singleton context for wifi_manager.
 *
 * Shared across all wifi_mgr_*.c source files. Never exposed outside
 * the component — in PRIV_INCLUDE_DIRS only.
 *
 * All mutable state lives in the single global g_wifi_mgr_ctx.
 * State transitions are protected by state_mutex.
 * No heap allocation occurs after wifi_mgr_init() returns (N5).
 */

#pragma once

#include "ap_records.h"
#include "wifi_manager.h"
#include "network_provisioning/manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include "esp_netif.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Internal task notification bits ────────────────────────────────────── */

/* Checked in this exact priority order by the manager task.
 * Higher bit position = higher priority. */
#define NOTIFY_SCAN_DONE        (1u << 0)   /* priority 6 (lowest) */
#define NOTIFY_TW_EXPIRED       (1u << 1)   /* priority 2 */
#define NOTIFY_CONNECTED        (1u << 2)   /* priority 3 */
#define NOTIFY_DISCONNECTED     (1u << 3)   /* priority 5 */
#define NOTIFY_CONNECT_TIMEOUT  (1u << 4)   /* priority 4 */
#define NOTIFY_PROV_DONE        (1u << 5)   /* priority 7 */
#define NOTIFY_PROV_FAILED      (1u << 6)   /* priority 8 (lowest among prov) */
#define NOTIFY_STOP             (1u << 7)   /* priority 1 (highest) */
#define NOTIFY_WAKE             (1u << 9)   /* "re-read state": sent when another
                                             * task changed state while the
                                             * manager may be blocked (e.g. a
                                             * setup-mode start that failed) */
#define NOTIFY_SETUP_DEADLINE   (1u << 8)   /* setup_timer: setup-mode deadline
                                             * reached (credentials stored).
                                             * Checked before PROV_DONE in
                                             * PROVISIONING so the cap is a
                                             * true cap. */

/* ── EventGroup bits ─────────────────────────────────────────────────────── */

#define BIT_CONNECTED      (1u << 0)
#define BIT_WAIT_CANCELLED (1u << 1)

/* ── Watchdog formula ────────────────────────────────────────────────────── */

/* Minimum time between scan start and SCAN_DONE notification.
 * Semantics: this is a FLOOR (minimum), not a cap.
 * At default inter_scan_delay of 3000ms: 8000ms floor.
 * At 10000ms: 20000ms. At 30000ms: 60000ms.
 * A long scan interval is expected — do not cap the watchdog below it. */
#define WIFI_MGR_SCAN_WATCHDOG_MS(delay_ms) \
    ((uint32_t)((((delay_ms) * 2) > 8000u) ? ((delay_ms) * 2) : 8000u))

/* ── Compile-time safety guards ──────────────────────────────────────────── */

/* Guard 1: scan buffer must fit within half the task stack.
 * Peak usage in wifi_mgr_scan_handle_results():
 *   scan_buf        = CONFIG_WIFI_SCAN_MAX_RESULTS * 76 bytes
 *   stored_snapshot = CONFIG_MAX_AP_COUNT * sizeof(ap_info_t)
 *   known_list      = CONFIG_MAX_AP_COUNT * sizeof(ap_info_t)
 *   Total peak = SCAN_MAX * 76 + 2 * MAX_AP * sizeof(ap_info_t)
 *
 * This fires whenever your sdkconfig deviates from the documented
 * defaults (SCAN_MAX_RESULTS=20, MAX_AP_COUNT=5, TASK_STACK_SIZE=6144),
 * most commonly because:
 *   (a) CONFIG_WIFI_TASK_STACK_SIZE was left at a smaller pre-existing
 *       value from before this component was added to your project
 *       (sdkconfig.defaults only seeds a *new* sdkconfig — it does not
 *       retroactively raise an existing one), or
 *   (b) CONFIG_MAX_AP_COUNT or CONFIG_WIFI_SCAN_MAX_RESULTS was raised
 *       via menuconfig without proportionally raising the stack size.
 *
 * Fix: run `idf.py menuconfig` -> "WiFi Manager" and either raise
 * CONFIG_WIFI_TASK_STACK_SIZE, or lower CONFIG_WIFI_SCAN_MAX_RESULTS /
 * CONFIG_MAX_AP_COUNT, until
 *   (SCAN_MAX_RESULTS * 76) + (2 * MAX_AP_COUNT * 108) <= STACK_SIZE / 2
 * holds. (108 is sizeof(ap_info_t) on a typical 32-bit toolchain; your
 * actual value may differ slightly with padding.)
 */
_Static_assert(
    (CONFIG_WIFI_SCAN_MAX_RESULTS * 76u +
     2u * CONFIG_MAX_AP_COUNT * sizeof(ap_info_t)) <=
    (CONFIG_WIFI_TASK_STACK_SIZE / 2u),
    "Scan buffers exceed half the task stack. At your current "
    "CONFIG_WIFI_SCAN_MAX_RESULTS / CONFIG_MAX_AP_COUNT / "
    "CONFIG_WIFI_TASK_STACK_SIZE values, peak stack usage inside "
    "wifi_mgr_scan_handle_results() would exceed the safety margin. "
    "Run 'idf.py menuconfig' -> WiFi Manager and either raise "
    "CONFIG_WIFI_TASK_STACK_SIZE, or reduce CONFIG_WIFI_SCAN_MAX_RESULTS "
    "/ CONFIG_MAX_AP_COUNT. See the comment above this assert in "
    "wifi_mgr_ctx.h for the exact formula and common causes."
);

/* Guard 2: scan result cap must be at least the credential count.
 * Otherwise stored APs beyond the cap are silently skipped each cycle. */
_Static_assert(
    CONFIG_WIFI_SCAN_MAX_RESULTS >= CONFIG_MAX_AP_COUNT,
    "CONFIG_WIFI_SCAN_MAX_RESULTS must be >= CONFIG_MAX_AP_COUNT. "
    "Otherwise stored credentials for APs beyond the cap are silently "
    "skipped on every scan. See L2 in 01_overview.md."
);

/* Sized for human-chosen static PoP strings (CONFIG_WIFI_PROV_POP), not
 * just the 8-hex-char MAC-derived case. wifi_mgr_prov.c enforces this
 * bound at compile time via _Static_assert against CONFIG_WIFI_PROV_POP's
 * length — keep both in sync if you change this. */
#define WIFI_MGR_PROV_POP_BUF_SIZE 64

/* ── Context struct ──────────────────────────────────────────────────────── */

typedef struct {
    /* Runtime config (resolved from wifi_mgr_config_t + Kconfig defaults) */
    wifi_mgr_config_t   config;

    /* State machine */
    wifi_mgr_state_t    state;
    SemaphoreHandle_t   state_mutex;

    /* Manager task */
    TaskHandle_t        task_handle;
    SemaphoreHandle_t   stopped_sem;    /**< Given as task's last action before vTaskDelete */
    esp_netif_t        *sta_netif;      /**< Default STA netif — created in wifi_mgr_start(),
                                          *  destroyed in wifi_mgr_stop(). The caller must NOT
                                          *  create their own STA netif; the component owns it. */

    /* Timers (all one-shot) */
    TimerHandle_t       tw_timer;       /**< Time-window hard deadline */
    TimerHandle_t       connect_timer;  /**< Per-AP connection timeout */
    TimerHandle_t       prov_timer;     /**< BLE provisioning attempt timeout */
    TimerHandle_t       setup_timer;    /**< Setup-mode deadline / grace (credentials stored) */

    /* Blocking-wait EventGroup */
    EventGroupHandle_t  event_group;    /**< BIT_CONNECTED | BIT_WAIT_CANCELLED */

    /* Connection info — written by event handlers, read by manager task */
    wifi_mgr_connected_info_t conn_info;

    /* Provisioning */
    char  prov_device_name[32];
    char  prov_pop[WIFI_MGR_PROV_POP_BUF_SIZE];
    network_prov_security_t prov_security; /**< Set once on first wifi_mgr_prov_start() call,
                                           *  reused on retry-in-place restarts so the
                                           *  security mode doesn't need recomputing from
                                           *  Kconfig each retry. */
    bool  prov_mgr_initialized;     /**< Guards prov_mgr_safe_deinit idempotency */
    bool  bt_mem_freed;             /**< Set only under CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS,
                                       *  after a successful provisioning session frees the BLE
                                       *  controller. Once true, BLE provisioning cannot be
                                       *  re-entered in this boot (esp_bt_controller_init() is not
                                       *  supported after a prior deinit) — wifi_mgr_prov_start()
                                       *  checks this flag to detect the dead end and restart
                                       *  instead of retrying a call that cannot succeed. Never set
                                       *  under CONFIG_WIFI_PROV_BT_LIFECYCLE_KEEP_ALIVE. Initialized
                                       *  false at boot; never cleared (a restart is required to
                                       *  clear it, by design). */
    SemaphoreHandle_t prov_stop_confirm_sem; /**< Given from the NETWORK_PROV_END handler;
                                                *  taken (bounded) by all three
                                                *  wifi_mgr_prov_stop_*() functions to wait
                                                *  for confirmed BLE teardown before
                                                *  proceeding — network_prov_mgr_stop_provisioning()
                                                *  does not wait for this itself despite
                                                *  running in blocking mode (real-hardware-
                                                *  discovered; see "Retry Policy" in
                                                *  08_provisioning_flow.md). A semaphore (not
                                                *  a task notification) is used because
                                                *  wifi_mgr_prov_stop_shutdown() is called
                                                *  from wifi_mgr_stop()'s caller context, not
                                                *  the manager task — a task notification
                                                *  only works for the specific task it's
                                                *  posted to. Binary semaphore: starts empty
                                                *  (xSemaphoreCreateBinary() default); drained
                                                *  with a zero-timeout take at the start of
                                                *  wifi_mgr_prov_start() to discard any stale
                                                *  give left over from a prior session's
                                                *  NETWORK_PROV_END (binary, so multiple stale
                                                *  gives still collapse to a single drain). */

    /* Connect list — populated by wifi_mgr_scan_handle_results() */
    ap_info_t connect_list[CONFIG_MAX_AP_COUNT];
    uint8_t   connect_list_count;   /**< Number of valid entries */
    uint8_t   connect_list_index;   /**< Current AP being attempted */
    uint8_t   connect_retry;        /**< Retry count for current AP */

    /* use_count persistence throttle */
    uint8_t   connect_count_since_save; /**< Connections since last NVS save */

    /* RESCAN_ONCE context — cleared when leaving RESCAN_ONCE */
    char    last_connected_ssid[33];
    uint8_t last_connected_bssid[6]; /**< All-zeros if credential stored without BSSID */

    /* Disconnect reason (captured in event handler, used in DISCONNECTED event) */
    uint16_t disconnect_reason;

    /* Error tracking */
    uint8_t  consecutive_errors;    /**< Incremented on ERROR entry; reset on first successful scan */

    /* Scan truncation warning — emitted at most once per state entry */
    bool     scan_truncation_warned; /**< Cleared by wifi_mgr_state_set() */

    /* Per-tw-window bookkeeping (reset by wifi_mgr_tw_window_reset() each
     * time a window starts or is re-armed). Read when tw expires to choose
     * between NO_KNOWN_AP_IN_TW and KNOWN_AP_UNREACHABLE. Manager task only. */
    bool     tw_known_seen;          /**< A stored AP was visible in this window */
    uint16_t tw_attempts;            /**< Connection attempts made in this window */
    uint16_t tw_last_reason;         /**< Last failure reason (0 = connect timeout) */
    char     tw_last_ssid[33];       /**< SSID of the last attempt */

    /* Provisioned credentials held in RAM until the connection is confirmed.
     * Written by the NETWORK_PROV_WIFI_CRED_RECV / _CRED_SUCCESS handlers
     * (event task) strictly before NOTIFY_PROV_DONE is sent; consumed and
     * wiped by on_prov_done() (manager task). Wiped on CRED_FAIL, timeout
     * and shutdown. Nothing reaches ap_records until CRED_SUCCESS. */
    char     held_ssid[33];
    char     held_password[65];
    uint8_t  held_bssid[6];          /**< AP BSSID at CRED_SUCCESS, if SSID matched */
    bool     held_valid;             /**< held_ssid/held_password populated */
    bool     held_confirmed;         /**< CRED_SUCCESS seen for the held credential */
    /* Setup mode bookkeeping (0.4.0). setup_bounded is decided on entry to
     * PROVISIONING: true when credentials are stored, which enables the
     * deadline / grace / cap via setup_timer. Manager task, plus the entry
     * paths (wifi_mgr_start_ble_provisioning() runs in the caller's task). */
    bool       setup_bounded;
    TickType_t setup_started;           /**< xTaskGetTickCount() at entry */
    volatile bool ble_client_connected; /**< From PROTOCOMM_TRANSPORT_BLE_EVENT */
    bool       setup_requested_at_boot; /**< RTC flag seen (ESP_RST_SW) in wifi_mgr_init() */
    bool       no_trigger_hint_logged;  /**< Once-per-boot "add a setup trigger" hint */

    bool     driver_wipe_pending;    /**< A failed session left an unverified password
                                       *  in the driver's NVS and the wipe could not
                                       *  run (station still busy). Retried before the
                                       *  next connect attempt, at the next session end
                                       *  and at start-up. See wifi_mgr_prov.c. */

    /* Lifecycle guards */
    bool     initialized;
    bool     started;
    bool     stop_requested;
} wifi_mgr_ctx_t;

/* ── Global singleton ────────────────────────────────────────────────────── */

extern wifi_mgr_ctx_t g_wifi_mgr_ctx;

/* Convenience alias used throughout the implementation. */
#define CTX (&g_wifi_mgr_ctx)

/* Start a fresh tw window's bookkeeping. Call whenever tw_timer is (re)started:
 * on TW_ACTIVE entry (wifi_mgr_scan.c) and on a soft re-arm (on_tw_expired()).
 * Manager task only. */
static inline void wifi_mgr_tw_window_reset(void)
{
    CTX->tw_known_seen  = false;
    CTX->tw_attempts    = 0;
    CTX->tw_last_reason = 0;
    memset(CTX->tw_last_ssid, 0, sizeof(CTX->tw_last_ssid));
}

#ifdef __cplusplus
}
#endif
