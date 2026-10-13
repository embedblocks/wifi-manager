#pragma once
/* wifi_event_stub.h — test-only stubs for injecting WiFi/IP events
 *
 * These functions simulate the ESP-IDF WiFi and IP event handlers that
 * would normally be fired by the driver. Call them from Unity test cases
 * to drive the wifi_manager state machine without real hardware.
 *
 * All functions post to the default event loop and/or directly call
 * xTaskNotify on the manager task, matching exactly what the real
 * WIFI_EVENT_SCAN_DONE / IP_EVENT_STA_GOT_IP handlers do.
 */

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Scan result injection
 * --------------------------------------------------------------------------
 * Load fake AP records into the stub's internal buffer, then call
 * stub_inject_scan_done() to fire WIFI_EVENT_SCAN_DONE.
 * The stub's esp_wifi_scan_get_ap_num / esp_wifi_scan_get_ap_records
 * overrides return data from this buffer.
 */

/**
 * @brief Set the scan result buffer that the stub will return.
 *
 * Must be called BEFORE stub_inject_scan_done().
 *
 * @param records   Array of wifi_ap_record_t to return. May be NULL
 *                  if count == 0 (simulates empty scan).
 * @param count     Number of records in the array.
 */
void stub_set_scan_results(const wifi_ap_record_t *records, uint16_t count);

/**
 * @brief Fire WIFI_EVENT_SCAN_DONE, causing the manager to process results.
 *
 * Sends NOTIFY_SCAN_DONE to the manager task via xTaskNotify, exactly as
 * the real wifi_event_handler does.
 */
void stub_inject_scan_done(void);

/* --------------------------------------------------------------------------
 * Connection event injection
 * -------------------------------------------------------------------------- */

/**
 * @brief Fire IP_EVENT_STA_GOT_IP with the given IPv4 address.
 *
 * Sends NOTIFY_CONNECTED to the manager task, which drives the
 * CONNECTING → CONNECTED transition.
 *
 * @param ip   IPv4 address in host byte order (e.g. 0xC0A80101 = 192.168.1.1).
 *             Pass 0 to use a default address (10.0.0.1).
 */
void stub_inject_got_ip(uint32_t ip);

/**
 * @brief Fire WIFI_EVENT_STA_DISCONNECTED with the given reason code.
 *
 * Sends NOTIFY_DISCONNECTED to the manager task.
 *
 * @param reason   WiFi disconnect reason (wifi_err_reason_t). Use
 *                 WIFI_REASON_BEACON_TIMEOUT (200) for a typical loss-of-signal
 *                 disconnect, or WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT (15) for
 *                 a wrong-password disconnect.
 */
void stub_inject_disconnected(uint8_t reason);

/* --------------------------------------------------------------------------
 * Provisioning event injection
 * -------------------------------------------------------------------------- */

/**
 * @brief Simulate successful provisioning credential receipt + end.
 *
 * Fires NETWORK_PROV_WIFI_CRED_RECV then NETWORK_PROV_WIFI_CRED_SUCCESS then NETWORK_PROV_END
 * in sequence, causing the manager to attempt a connection with the supplied
 * credentials and transition out of PROVISIONING state.
 *
 * @param ssid      SSID string (max 32 chars).
 * @param password  Password string (max 64 chars).
 */
void stub_inject_prov_success(const char *ssid, const char *password);

/**
 * @brief Simulate a provisioning failure.
 *
 * Fires NETWORK_PROV_WIFI_CRED_FAIL, causing the manager to post
 * WIFI_MGR_EVENT_PROVISIONING_FAILED and transition back to SCANNING.
 */
void stub_inject_prov_failed(void);

/* --------------------------------------------------------------------------
 * Timer injection
 * -------------------------------------------------------------------------- */

/**
 * @brief Manually fire the tw (Time Window) timer expiry.
 *
 * Sends NOTIFY_TW_EXPIRED to the manager task, simulating the tw_timer
 * callback firing. Use to drive TW_ACTIVE → PROVISIONING transition
 * without waiting CONFIG_WIFI_KNOWN_AP_WAIT_MS.
 */
void stub_inject_tw_expired(void);

/**
 * @brief Manually fire the connect timer expiry.
 *
 * Sends NOTIFY_CONNECT_TIMEOUT to the manager task, simulating a
 * connection attempt timing out.
 */
void stub_inject_connect_timeout(void);

/* --------------------------------------------------------------------------
 * Stub state / reset
 * -------------------------------------------------------------------------- */

/**
 * @brief Reset all stub state.
 *
 * Clears the scan result buffer, resets call counters, and resets any
 * injected IP address. Call from Unity setUp() before each test case.
 */
void stub_wifi_event_reset(void);

/**
 * @brief Return number of times esp_wifi_connect() has been called.
 *
 * Useful for asserting retry counts in connect-flow tests.
 */
int stub_wifi_connect_call_count(void);

/**
 * @brief Return number of times esp_wifi_disconnect() has been called.
 */
int stub_wifi_disconnect_call_count(void);

/**
 * @brief Return number of times esp_wifi_scan_start() has been called.
 */
int stub_wifi_scan_start_call_count(void);

/**
 * @brief Return the last wifi_config_t passed to esp_wifi_set_config().
 *
 * Allows tests to assert SSID / BSSID / password of the connection attempt.
 */
const wifi_config_t *stub_get_last_wifi_config(void);

#ifdef __cplusplus
}
#endif
