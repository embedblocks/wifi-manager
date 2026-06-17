/**
 * @file ap_records_stub.h
 * @brief Test stub for ap_records — in-RAM only, no NVS dependency.
 *
 * Provides the same API as ap_records.h. Replaces ap_records.c and
 * blob_storage.c in the test_app build.
 *
 * Inject test data before calling wifi_mgr_init(). Reset in setUp().
 */

#pragma once

#include "esp_err.h"
#include "sdkconfig.h"
#include "blob_storage.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ap_info_t / ap_record_t (must match src/ap_records.h exactly) ─────── */

typedef struct {
    char     ssid[33];
    char     password[64];
    uint8_t  bssid[6];
    uint32_t use_count;
} ap_info_t;

typedef struct {
    ap_info_t ap_list[CONFIG_MAX_AP_COUNT];
    int       available_records;
} ap_record_t;

/* ── Stub control API (not present in real ap_records.h) ─────────────────── */

/**
 * @brief Pre-load entries into the stub store.
 *
 * use_count is stored as-is (no max+1 rule — tests need precise control).
 * Call stub_ap_records_reset() in setUp() between tests.
 */
void stub_ap_records_inject(const ap_info_t *entries, uint8_t count);

/**
 * @brief Control what ap_records_save() returns.
 */
void stub_ap_records_set_save_result(esp_err_t result);

/**
 * @brief Return the number of ap_records_save() calls since last reset.
 */
uint8_t stub_ap_records_save_call_count(void);

/**
 * @brief Reset all stub state: clear entries, reset save result to ESP_OK,
 *        zero all call counters.
 */
void stub_ap_records_reset(void);

/* ── ap_records API (same signatures as src/ap_records.h) ─────────────── */

esp_err_t ap_records_init(void);
esp_err_t ap_records_deinit(void);
esp_err_t ap_records_load(void);
esp_err_t ap_records_save(void);
esp_err_t ap_records_get_all(ap_info_t *out, uint8_t *out_count);
esp_err_t ap_records_set_all(const ap_record_t *records);
esp_err_t ap_records_add(const char *ssid, const char *password, const uint8_t *bssid);
esp_err_t ap_records_get(int index, ap_info_t *ap_info);
int       ap_records_get_count(void);
esp_err_t ap_records_find_by_ssid(const char *ssid, ap_info_t *ap_info, int *index);
esp_err_t ap_records_find_by_bssid(const uint8_t *bssid, ap_info_t *ap_info, int *index);
esp_err_t ap_records_increment_use_count(const char *ssid);
esp_err_t ap_records_remove_by_ssid(const char *ssid);
esp_err_t ap_records_remove_by_index(int index);
esp_err_t ap_records_clear_all(void);
esp_err_t ap_records_sort_by_usage(void);
esp_err_t ap_records_get_stats(const blob_storage_handle_t *handle,
                                size_t *used_size, size_t *max_size);
size_t    ap_records_get_size(void);
bool      ap_records_is_initialized(void);
void      ap_records_print_all(void);

#ifdef __cplusplus
}
#endif
