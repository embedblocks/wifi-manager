/**
 * @file ap_records_stub.c
 * @brief In-RAM stub implementation of ap_records for unit tests.
 *
 * Replaces ap_records.c and blob_storage.c in the test_app build.
 * No NVS dependency. Mutex present on all state-touching functions.
 */

#include "ap_records_stub.h"
#include "blob_storage.h"
#include "esp_log.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ap_records_stub";

/* ── Stub state ──────────────────────────────────────────────────────────── */

static ap_record_t      s_store          = {0};
static bool             s_initialized    = false;
static esp_err_t        s_save_result    = ESP_OK;
static uint8_t          s_save_count     = 0;
static SemaphoreHandle_t s_stub_mutex    = NULL;

/* ── Stub control API ────────────────────────────────────────────────────── */

void stub_ap_records_inject(const ap_info_t *entries, uint8_t count)
{
    if (!s_stub_mutex) return;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    if (count > CONFIG_MAX_AP_COUNT) count = CONFIG_MAX_AP_COUNT;
    memcpy(s_store.ap_list, entries, count * sizeof(ap_info_t));
    s_store.available_records = count;
    xSemaphoreGive(s_stub_mutex);
}

void stub_ap_records_set_save_result(esp_err_t result)
{
    if (!s_stub_mutex) return;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    s_save_result = result;
    xSemaphoreGive(s_stub_mutex);
}

uint8_t stub_ap_records_save_call_count(void)
{
    if (!s_stub_mutex) return 0;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    uint8_t c = s_save_count;
    xSemaphoreGive(s_stub_mutex);
    return c;
}

void stub_ap_records_reset(void)
{
    if (!s_stub_mutex) {
        s_stub_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    memset(&s_store, 0, sizeof(s_store));
    s_save_result = ESP_OK;
    s_save_count  = 0;
    xSemaphoreGive(s_stub_mutex);
}

/* ── ap_records API ──────────────────────────────────────────────────────── */

esp_err_t ap_records_init(void)
{
    if (!s_stub_mutex) {
        s_stub_mutex = xSemaphoreCreateMutex();
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ap_records_deinit(void)
{
    s_initialized = false;
    /* Do not delete mutex — may be reused across init/deinit cycles in tests */
    return ESP_OK;
}

esp_err_t ap_records_load(void)
{
    return ESP_ERR_NOT_FOUND; /* No NVS in stub — always first-boot */
}

esp_err_t ap_records_save(void)
{
    if (!s_stub_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    s_save_count++;
    esp_err_t ret = s_save_result;
    xSemaphoreGive(s_stub_mutex);
    return ret;
}

esp_err_t ap_records_get_all(ap_info_t *out, uint8_t *out_count)
{
    if (!out || !out_count) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    *out_count = (uint8_t)s_store.available_records;
    memcpy(out, s_store.ap_list, s_store.available_records * sizeof(ap_info_t));
    xSemaphoreGive(s_stub_mutex);
    return ESP_OK;
}

esp_err_t ap_records_set_all(const ap_record_t *records)
{
    if (!records) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    memcpy(&s_store, records, sizeof(ap_record_t));
    xSemaphoreGive(s_stub_mutex);
    return ESP_OK;
}

static uint32_t get_max_use_count_locked(void)
{
    uint32_t m = 0;
    for (int i = 0; i < s_store.available_records; i++) {
        if (s_store.ap_list[i].use_count > m) m = s_store.ap_list[i].use_count;
    }
    return m;
}

static inline uint32_t sat_inc(uint32_t v) { return (v < UINT32_MAX) ? v + 1 : UINT32_MAX; }

esp_err_t ap_records_add(const char *ssid, const char *password, const uint8_t *bssid)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);

    /* Update existing */
    for (int i = 0; i < s_store.available_records; i++) {
        if (strcmp(s_store.ap_list[i].ssid, ssid) == 0) {
            strncpy(s_store.ap_list[i].password, password, sizeof(s_store.ap_list[i].password) - 1);
            if (bssid) memcpy(s_store.ap_list[i].bssid, bssid, 6);
            s_store.ap_list[i].use_count = sat_inc(get_max_use_count_locked());
            xSemaphoreGive(s_stub_mutex);
            return ESP_OK;
        }
    }

    /* New entry */
    int target;
    if (s_store.available_records < CONFIG_MAX_AP_COUNT) {
        target = s_store.available_records++;
    } else {
        int min_idx = 0;
        for (int i = 1; i < CONFIG_MAX_AP_COUNT; i++) {
            if (s_store.ap_list[i].use_count < s_store.ap_list[min_idx].use_count)
                min_idx = i;
        }
        target = min_idx;
    }

    memset(&s_store.ap_list[target], 0, sizeof(ap_info_t));
    strncpy(s_store.ap_list[target].ssid,     ssid,     sizeof(s_store.ap_list[target].ssid) - 1);
    strncpy(s_store.ap_list[target].password, password, sizeof(s_store.ap_list[target].password) - 1);
    if (bssid) memcpy(s_store.ap_list[target].bssid, bssid, 6);
    s_store.ap_list[target].use_count = sat_inc(get_max_use_count_locked());

    xSemaphoreGive(s_stub_mutex);
    return ESP_OK;
}

esp_err_t ap_records_get(int index, ap_info_t *ap_info)
{
    if (!ap_info) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_store.available_records) {
        xSemaphoreGive(s_stub_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(ap_info, &s_store.ap_list[index], sizeof(ap_info_t));
    xSemaphoreGive(s_stub_mutex);
    return ESP_OK;
}

int ap_records_get_count(void)
{
    if (!s_stub_mutex) return 0;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    int c = s_store.available_records;
    xSemaphoreGive(s_stub_mutex);
    return c;
}

esp_err_t ap_records_find_by_ssid(const char *ssid, ap_info_t *ap_info, int *index)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    for (int i = 0; i < s_store.available_records; i++) {
        if (strcmp(s_store.ap_list[i].ssid, ssid) == 0) {
            if (ap_info) memcpy(ap_info, &s_store.ap_list[i], sizeof(ap_info_t));
            if (index)   *index = i;
            xSemaphoreGive(s_stub_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_stub_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_find_by_bssid(const uint8_t *bssid, ap_info_t *ap_info, int *index)
{
    if (!bssid) return ESP_ERR_INVALID_ARG;
    static const uint8_t zero[6] = {0};
    if (memcmp(bssid, zero, 6) == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    for (int i = 0; i < s_store.available_records; i++) {
        if (memcmp(s_store.ap_list[i].bssid, bssid, 6) == 0) {
            if (ap_info) memcpy(ap_info, &s_store.ap_list[i], sizeof(ap_info_t));
            if (index)   *index = i;
            xSemaphoreGive(s_stub_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_stub_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_increment_use_count(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    for (int i = 0; i < s_store.available_records; i++) {
        if (strcmp(s_store.ap_list[i].ssid, ssid) == 0) {
            s_store.ap_list[i].use_count = sat_inc(s_store.ap_list[i].use_count);
            xSemaphoreGive(s_stub_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_stub_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_remove_by_ssid(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    for (int i = 0; i < s_store.available_records; i++) {
        if (strcmp(s_store.ap_list[i].ssid, ssid) == 0) {
            for (int j = i; j < s_store.available_records - 1; j++)
                s_store.ap_list[j] = s_store.ap_list[j + 1];
            s_store.available_records--;
            memset(&s_store.ap_list[s_store.available_records], 0, sizeof(ap_info_t));
            xSemaphoreGive(s_stub_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_stub_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_remove_by_index(int index)
{
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_store.available_records) {
        xSemaphoreGive(s_stub_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = index; i < s_store.available_records - 1; i++)
        s_store.ap_list[i] = s_store.ap_list[i + 1];
    s_store.available_records--;
    memset(&s_store.ap_list[s_store.available_records], 0, sizeof(ap_info_t));
    xSemaphoreGive(s_stub_mutex);
    return ESP_OK;
}

esp_err_t ap_records_clear_all(void)
{
    xSemaphoreTake(s_stub_mutex, portMAX_DELAY);
    memset(&s_store, 0, sizeof(s_store));
    xSemaphoreGive(s_stub_mutex);
    return ESP_OK;
}

esp_err_t ap_records_sort_by_usage(void) { return ESP_OK; }

esp_err_t ap_records_get_stats(const blob_storage_handle_t *handle,
                                size_t *used_size, size_t *max_size)
{
    if (!handle || !used_size || !max_size) return ESP_ERR_INVALID_ARG;
    *max_size  = handle->max_size;
    *used_size = sizeof(ap_record_t);
    return ESP_OK;
}

size_t ap_records_get_size(void) { return sizeof(ap_record_t); }
bool   ap_records_is_initialized(void) { return s_initialized; }
void   ap_records_print_all(void) { ESP_LOGI(TAG, "ap_records_stub: %d entries", s_store.available_records); }

/* ── blob_storage stub (no-op — ap_records_stub handles all NVS) ─────────── */

esp_err_t blob_storage_init(void) { return ESP_OK; }

esp_err_t blob_storage_create_handle(blob_storage_handle_t *handle,
                                     const char *ns, const char *key,
                                     size_t max_size)
{
    if (!handle || !ns || !key) return ESP_ERR_INVALID_ARG;
    strncpy(handle->namespace, ns,  sizeof(handle->namespace) - 1);
    strncpy(handle->key, key, sizeof(handle->key) - 1);
    handle->max_size    = max_size;
    handle->initialized = true;
    return ESP_OK;
}

esp_err_t blob_storage_write(const blob_storage_handle_t *h, const void *d, size_t s) { return ESP_OK; }
esp_err_t blob_storage_read(const blob_storage_handle_t *h, void *d, size_t *s) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t blob_storage_exists(const blob_storage_handle_t *h, bool *e, size_t *s)
{
    if (e) *e = false;
    if (s) *s = 0;
    return ESP_OK;
}
esp_err_t blob_storage_delete(const blob_storage_handle_t *h) { return ESP_OK; }
esp_err_t blob_storage_get_stats(const blob_storage_handle_t *h, size_t *u, size_t *m)
{
    if (u) *u = 0;
    if (m) *m = h ? h->max_size : 0;
    return ESP_OK;
}
bool blob_storage_is_initialized(void) { return true; }
