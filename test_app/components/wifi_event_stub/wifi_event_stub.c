/* wifi_event_stub.c — Test stubs for WiFi/IP event injection
 *
 * Provides:
 *  - Fake implementations of esp_wifi_* functions so the wifi_manager
 *    component links without real hardware drivers.
 *  - Injection helpers that directly call xTaskNotify on the manager
 *    task, replicating exactly what the real event handlers do.
 *
 * Link this component into test_app; do NOT link it into production firmware.
 */

#include "wifi_event_stub.h"
#include "wifi_mgr_ctx.h"      /* for CTX->task_handle, NOTIFY_* bits */
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "WIFI_EVENT_STUB";

/* -------------------------------------------------------------------------
 * Internal stub state
 * ------------------------------------------------------------------------- */

static wifi_ap_record_t  s_scan_buf[CONFIG_WIFI_SCAN_MAX_RESULTS];
static uint16_t          s_scan_count = 0;

static wifi_config_t     s_last_wifi_config;
static int               s_connect_calls    = 0;
static int               s_disconnect_calls = 0;
static int               s_scan_start_calls = 0;

/* -------------------------------------------------------------------------
 * Stub reset
 * ------------------------------------------------------------------------- */

void stub_wifi_event_reset(void)
{
    memset(s_scan_buf, 0, sizeof(s_scan_buf));
    s_scan_count       = 0;
    s_connect_calls    = 0;
    s_disconnect_calls = 0;
    s_scan_start_calls = 0;
    memset(&s_last_wifi_config, 0, sizeof(s_last_wifi_config));
    ESP_LOGD(TAG, "WiFi event stub reset");
}

/* -------------------------------------------------------------------------
 * Scan injection
 * ------------------------------------------------------------------------- */

void stub_set_scan_results(const wifi_ap_record_t *records, uint16_t count)
{
    uint16_t cap = count < CONFIG_WIFI_SCAN_MAX_RESULTS
                   ? count : CONFIG_WIFI_SCAN_MAX_RESULTS;
    s_scan_count = cap;
    if (records && cap > 0) {
        memcpy(s_scan_buf, records, cap * sizeof(wifi_ap_record_t));
    } else {
        memset(s_scan_buf, 0, sizeof(s_scan_buf));
        s_scan_count = 0;
    }
    ESP_LOGD(TAG, "Scan results set: %u records", s_scan_count);
}

void stub_inject_scan_done(void)
{
    /* Replicate what wifi_event_handler does for WIFI_EVENT_SCAN_DONE */
    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_SCAN_DONE, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_SCAN_DONE");
    } else {
        ESP_LOGW(TAG, "stub_inject_scan_done: manager task not started");
    }
}

/* -------------------------------------------------------------------------
 * Connection event injection
 * ------------------------------------------------------------------------- */

void stub_inject_got_ip(uint32_t ip)
{
    /* Mirror what IP_EVENT_STA_GOT_IP handler does:
     * fill conn_info then send NOTIFY_CONNECTED */
    if (ip == 0) ip = 0x0A000001u; /* 10.0.0.1 */

    CTX->conn_info.ip      = ip;
    CTX->conn_info.channel = 6; /* plausible default for tests */
    /* ssid is already set by set_config stub below */

    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_CONNECTED, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_CONNECTED (ip=0x%08lx)", (unsigned long)ip);
    } else {
        ESP_LOGW(TAG, "stub_inject_got_ip: manager task not started");
    }
}

void stub_inject_disconnected(uint8_t reason)
{
    strncpy(CTX->conn_info.ssid,
            (const char *)s_last_wifi_config.sta.ssid,
            sizeof(CTX->conn_info.ssid) - 1);

    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_DISCONNECTED, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_DISCONNECTED (reason=%u)", reason);
    } else {
        ESP_LOGW(TAG, "stub_inject_disconnected: manager task not started");
    }
    (void)reason;
}

/* -------------------------------------------------------------------------
 * Provisioning event injection
 * ------------------------------------------------------------------------- */

void stub_inject_prov_success(const char *ssid, const char *password)
{
    /* Simulate the sequence: CRED_RECV → CRED_SUCCESS → PROV_END
     * The wifi_mgr_prov handler stores deferred info in CTX and sends
     * NOTIFY_PROV_DONE. We replicate the net effect directly. */

    if (ssid)     strncpy(CTX->prov_ssid,     ssid,     sizeof(CTX->prov_ssid)     - 1);
    if (password) strncpy(CTX->prov_password,  password, sizeof(CTX->prov_password) - 1);
    CTX->prov_cred_received = true;

    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_PROV_DONE, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_PROV_DONE (ssid=%s)", ssid ? ssid : "");
    } else {
        ESP_LOGW(TAG, "stub_inject_prov_success: manager task not started");
    }
}

void stub_inject_prov_failed(void)
{
    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_PROV_FAILED, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_PROV_FAILED");
    } else {
        ESP_LOGW(TAG, "stub_inject_prov_failed: manager task not started");
    }
}

/* -------------------------------------------------------------------------
 * Timer injection
 * ------------------------------------------------------------------------- */

void stub_inject_tw_expired(void)
{
    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_TW_EXPIRED, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_TW_EXPIRED");
    } else {
        ESP_LOGW(TAG, "stub_inject_tw_expired: manager task not started");
    }
}

void stub_inject_connect_timeout(void)
{
    TaskHandle_t h = CTX->task_handle;
    if (h) {
        xTaskNotify(h, NOTIFY_CONNECT_TIMEOUT, eSetBits);
        ESP_LOGD(TAG, "Injected NOTIFY_CONNECT_TIMEOUT");
    } else {
        ESP_LOGW(TAG, "stub_inject_connect_timeout: manager task not started");
    }
}

/* -------------------------------------------------------------------------
 * Call count accessors
 * ------------------------------------------------------------------------- */

int stub_wifi_connect_call_count(void)    { return s_connect_calls;    }
int stub_wifi_disconnect_call_count(void) { return s_disconnect_calls; }
int stub_wifi_scan_start_call_count(void) { return s_scan_start_calls; }

const wifi_config_t *stub_get_last_wifi_config(void)
{
    return &s_last_wifi_config;
}

/* =========================================================================
 * Fake esp_wifi_* implementations
 * These replace the real driver functions during linking for tests.
 * ========================================================================= */

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    (void)mode;
    return ESP_OK;
}

esp_err_t esp_wifi_get_mode(wifi_mode_t *mode)
{
    if (mode) *mode = WIFI_MODE_STA;
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    (void)storage;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    (void)config;
    (void)block;
    s_scan_start_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_stop(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number)
{
    if (number) *number = s_scan_count;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number,
                                        wifi_ap_record_t *ap_records)
{
    if (!number || !ap_records) return ESP_ERR_INVALID_ARG;
    uint16_t n = *number < s_scan_count ? *number : s_scan_count;
    memcpy(ap_records, s_scan_buf, n * sizeof(wifi_ap_record_t));
    *number = n;
    return ESP_OK;
}

esp_err_t esp_wifi_connect(void)
{
    s_connect_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_disconnect(void)
{
    s_disconnect_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_set_config(wifi_interface_t ifx, wifi_config_t *conf)
{
    (void)ifx;
    if (conf) {
        memcpy(&s_last_wifi_config, conf, sizeof(wifi_config_t));
        /* mirror SSID into conn_info for later stub_inject_got_ip use */
        strncpy(CTX->conn_info.ssid,
                (const char *)conf->sta.ssid,
                sizeof(CTX->conn_info.ssid) - 1);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_get_config(wifi_interface_t ifx, wifi_config_t *conf)
{
    (void)ifx;
    if (conf) memcpy(conf, &s_last_wifi_config, sizeof(wifi_config_t));
    return ESP_OK;
}

esp_err_t esp_wifi_set_auto_connect(bool en)
{
    (void)en;
    return ESP_OK;
}

esp_err_t esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{
    if (primary) *primary = 6;
    if (second)  *second  = WIFI_SECOND_CHAN_NONE;
    return ESP_OK;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)type;
    if (mac) {
        /* Deterministic fake MAC for tests */
        mac[0] = 0xDE; mac[1] = 0xAD; mac[2] = 0xBE;
        mac[3] = 0xEF; mac[4] = 0x00; mac[5] = 0x01;
    }
    return ESP_OK;
}

/* Minimal esp_netif stubs */
esp_err_t esp_netif_init(void)                    { return ESP_OK; }
esp_err_t esp_netif_deinit(void)                  { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return (esp_netif_t *)1; }
void esp_netif_destroy_default_wifi(esp_netif_t *n) { (void)n; }
