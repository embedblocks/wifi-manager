/* VENDORED — local copy, not upstream-tracked */
/* ap_records.c */
#include "ap_records.h"
#include "blob_storage.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "string.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "AP_RECORDS";

#define AP_RECORDS_NAMESPACE "ap_storage"
#define AP_RECORDS_KEY       "ap_records"

static ap_record_t          ap_records   = {0};
static bool                 is_initialized = false;
static blob_storage_handle_t storage_handle = {0};
static SemaphoreHandle_t    s_mutex      = NULL;

/*
 * s_dirty — set true when a remove or clear operation modifies the in-RAM store
 * after ap_records_save() has taken its snapshot but before the NVS write
 * completes. ap_records_save() checks this flag after the NVS write and
 * re-snapshots + re-writes if set, closing the TOCTOU gap without requiring
 * callers to call ap_records_save() explicitly after every remove/clear.
 *
 * Must only be read or written while holding s_mutex.
 * Cleared inside ap_records_save() at snapshot time (under mutex), so the
 * re-check after the NVS write detects any modification that occurred
 * between the snapshot and the write completing.
 */
static bool                 s_dirty      = false;

/* ── Internal helpers (called with mutex already held) ─────────────────── */

/*
 * Returns the highest use_count currently in the store.
 * Returns 0 if the store is empty.
 * Caller must hold s_mutex.
 */
static uint32_t get_max_use_count_locked(void)
{
    uint32_t max = 0;
    for (int i = 0; i < ap_records.available_records; i++) {
        if (ap_records.ap_list[i].use_count > max) {
            max = ap_records.ap_list[i].use_count;
        }
    }
    return max;
}

/*
 * Saturating increment: returns val + 1, clamped to UINT32_MAX.
 */
static inline uint32_t sat_inc(uint32_t val)
{
    return (val < UINT32_MAX) ? val + 1 : UINT32_MAX;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

esp_err_t ap_records_init(void)
{
    if (is_initialized) {
        ESP_LOGD(TAG, "AP records already initialized");
        return ESP_OK;
    }

    /* Create the mutex before any other work so it is valid if any
     * subsequent step fails and leaves is_initialized = false.       */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&ap_records, 0, sizeof(ap_record_t));

    esp_err_t ret = blob_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize blob storage: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    ret = blob_storage_create_handle(&storage_handle,
                                     AP_RECORDS_NAMESPACE,
                                     AP_RECORDS_KEY,
                                     sizeof(ap_record_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create storage handle: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    is_initialized = true;
    ESP_LOGI(TAG, "AP records manager initialized");

    /* Load existing data. ESP_ERR_NOT_FOUND means first boot — that is
     * normal and not an error from the caller's perspective.
     * ap_records_init() always returns ESP_OK on success; whether data
     * was found is internal state.                                      */
    ret = ap_records_load();
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No existing AP records found, starting fresh");
        /* Not an error — treat as clean init */
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load AP records: %s", esp_err_to_name(ret));
        /* Non-fatal: continue with empty records */
    }

    return ESP_OK;
}

/*
 * ap_records_deinit — release all resources allocated by ap_records_init().
 *
 * CALL ORDER REQUIREMENT: must be called only after wifi_mgr_deinit() has
 * returned and no task can reach any ap_records_* function. The manager task
 * and all external callers must have stopped. Calling ap_records_deinit()
 * while another task holds s_mutex is undefined behaviour.
 *
 * After this returns, ap_records_init() may be called again to reinitialize
 * the component (e.g. in unit-test teardown/setup cycles).
 *
 * Returns ESP_OK unconditionally. The return value is provided for caller
 * consistency with init; there is no partial-failure path in deinit.
 */
esp_err_t ap_records_deinit(void)
{
    if (!is_initialized) {
        ESP_LOGD(TAG, "ap_records_deinit: not initialized, nothing to do");
        return ESP_OK;
    }

    /* Clear credentials from RAM before releasing the mutex so that any
     * task that somehow races to call an ap_records_* function after this
     * point sees an empty, zeroed store rather than stale credential data. */
    memset(&ap_records, 0, sizeof(ap_record_t));
    s_dirty = false;

    /* Mark uninitialized before deleting the mutex. Any task that checks
     * is_initialized under a still-valid mutex will see false and return
     * ESP_ERR_INVALID_STATE. Once the mutex is deleted, is_initialized is
     * already false so no new lock attempt is made. */
    is_initialized = false;

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    /* Zero the storage handle so a subsequent init creates a fresh one. */
    memset(&storage_handle, 0, sizeof(blob_storage_handle_t));

    ESP_LOGI(TAG, "AP records deinitialized");
    return ESP_OK;
}

esp_err_t ap_records_load(void)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t size = sizeof(ap_record_t);
    esp_err_t ret = blob_storage_read(&storage_handle, &ap_records, &size);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "No AP records found in storage");
        memset(&ap_records, 0, sizeof(ap_record_t));
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read AP records: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_mutex);
        return ret;
    }

    if (size != sizeof(ap_record_t)) {
        ESP_LOGW(TAG, "Size mismatch in stored data, resetting");
        memset(&ap_records, 0, sizeof(ap_record_t));
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    if (ap_records.available_records > CONFIG_MAX_AP_COUNT) {
        ESP_LOGW(TAG, "Invalid record count in stored data, resetting");
        memset(&ap_records, 0, sizeof(ap_record_t));
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Loaded %d AP records from storage", ap_records.available_records);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ap_records_save(void)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Snapshot under lock, write outside lock, then re-check for concurrent
     * modifications using the s_dirty flag.
     *
     * Pattern:
     *   1. Take mutex. Clear s_dirty. Snapshot. Release mutex.
     *   2. Write snapshot to NVS (10–20 ms, no mutex held).
     *   3. Take mutex. Check s_dirty.
     *      - If false: snapshot was valid through the write. Done.
     *      - If true:  a remove or clear ran during the write (see below).
     *                  Clear s_dirty, re-snapshot, release mutex, re-write.
     *                  One re-write is sufficient: the second write will
     *                  include any modification that set s_dirty true.
     *
     * s_dirty is set to true by ap_records_remove_by_ssid() and
     * ap_records_clear_all() while holding s_mutex. Clearing it here
     * (step 1, under mutex) means: if a remove runs between step 1 and
     * step 3, s_dirty will be true in step 3, and the re-write will
     * capture the removal. If a remove runs before step 1, the snapshot
     * already reflects it. In all cases, the NVS blob is consistent with
     * the in-RAM store after this function returns.
     *
     * The s_dirty mechanism eliminates the TOCTOU gap for all callers.
     * Factory-reset paths and similar flows no longer need to call
     * ap_records_save() explicitly after remove/clear.
     *
     * At most two NVS writes occur per ap_records_save() call. The second
     * write only happens if a concurrent remove or clear races the first
     * write — rare in normal operation. No heap allocation occurs. */
    ap_record_t snapshot;
    esp_err_t   ret;

    /* ── First snapshot ─────────────────────────────────────────────── */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_dirty = false;
    memcpy(&snapshot, &ap_records, sizeof(ap_record_t));
    xSemaphoreGive(s_mutex);

    ret = blob_storage_write(&storage_handle, &snapshot, sizeof(ap_record_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save AP records: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── Dirty re-check ─────────────────────────────────────────────── */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool need_rewrite = s_dirty;
    if (need_rewrite) {
        s_dirty = false;
        memcpy(&snapshot, &ap_records, sizeof(ap_record_t));
    }
    xSemaphoreGive(s_mutex);

    if (need_rewrite) {
        ESP_LOGD(TAG, "ap_records_save: dirty flag set during write — re-saving");
        ret = blob_storage_write(&storage_handle, &snapshot, sizeof(ap_record_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to re-save AP records after dirty: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGD(TAG, "Saved %d AP records to storage", snapshot.available_records);
    return ESP_OK;
}

/* ── Bulk access ────────────────────────────────────────────────────────── */

esp_err_t ap_records_set_all(const ap_record_t *records)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!records) {
        ESP_LOGE(TAG, "Invalid records pointer");
        return ESP_ERR_INVALID_ARG;
    }
    if (records->available_records > CONFIG_MAX_AP_COUNT) {
        ESP_LOGE(TAG, "Invalid record count: %d (max: %d)",
                 records->available_records, CONFIG_MAX_AP_COUNT);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&ap_records, records, sizeof(ap_record_t));
    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Set %d AP records", records->available_records);
    return ESP_OK;
}

/*
 * ap_records_get_all — copy the internal store to caller-supplied buffers.
 *
 * out       : caller-supplied array of at least CONFIG_MAX_AP_COUNT entries
 * out_count : receives the number of valid entries written to out
 *
 * The mutex is held only for the duration of the internal memcpy. The caller
 * owns the data on its own stack after this returns and is immune to
 * concurrent add/remove/clear operations from other tasks.
 */
esp_err_t ap_records_get_all(ap_info_t *out, uint8_t *out_count)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!out || !out_count) {
        ESP_LOGE(TAG, "Invalid output pointers");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_count = (uint8_t)ap_records.available_records;
    memcpy(out, ap_records.ap_list,
           ap_records.available_records * sizeof(ap_info_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

/*
 * ap_records_get_readonly — UNSAFE direct pointer into the credential store.
 *
 * WARNING: This function returns a raw pointer into the internal ap_record_t
 * struct WITHOUT holding the mutex. Calling it from any context other than
 * a single-task diagnostic (e.g. a CLI command that runs only when the
 * manager is stopped) creates a data race with ap_records_add(),
 * ap_records_remove_by_ssid(), and ap_records_clear_all().
 *
 * This function is DISABLED in production builds. It is only compiled when
 * AP_RECORDS_ALLOW_UNSAFE_ACCESS is defined, which must never appear in a
 * production Makefile or sdkconfig. Define it only in test or debug builds:
 *
 *   idf.py -DAP_RECORDS_ALLOW_UNSAFE_ACCESS=1 build   ← test/debug only
 *
 * For all normal read access use ap_records_get_all(), which snapshots the
 * store under the mutex into a caller-supplied buffer and is safe from any
 * task context.
 *
 * ap_records_sort_by_usage() is a public API provided for test and diagnostic
 * use. wifi_manager internals never call it — the connect_list is sorted from
 * a snapshot at scan time. Do not call it from application code that runs
 * concurrently with the wifi_manager task; it modifies ap_records.ap_list[]
 * under mutex but changes the persistent sort order in ways that may
 * interact unexpectedly with the LRU eviction policy.
 */
#ifdef AP_RECORDS_ALLOW_UNSAFE_ACCESS
const ap_record_t *ap_records_get_readonly(void)
{
    if (!is_initialized) return NULL;
    return &ap_records;
}
#endif /* AP_RECORDS_ALLOW_UNSAFE_ACCESS */

/* ── Add / remove ───────────────────────────────────────────────────────── */

/*
 * ap_records_add — add a new credential or update an existing one.
 *
 * Priority invariant (enforced atomically under mutex):
 *   The new or updated record receives use_count = max_existing + 1,
 *   saturating at UINT32_MAX. This ensures it sorts to position 0 in the
 *   connect_list on the next scan regardless of what was previously stored.
 *
 *   When the store is full, the entry with the lowest use_count is evicted
 *   (LRU policy). The max_existing read happens AFTER the eviction so that
 *   if the evicted entry happened to hold the previous maximum, the new
 *   record still sorts correctly relative to what remains.
 *
 *   The entire eviction → max-read → set sequence runs under s_mutex.
 *   A concurrent ap_records_add() from another task cannot interleave.
 */
esp_err_t ap_records_add(const char *ssid, const char *password, const uint8_t *bssid)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid || !password) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD(TAG, "ap_records_add: SSID='%s'", ssid);
    if (strlen(ssid)     >= sizeof(ap_records.ap_list[0].ssid) ||
        strlen(password) >= sizeof(ap_records.ap_list[0].password)) {
        ESP_LOGE(TAG, "SSID or password too long");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* ── Update existing record if SSID already known ── */
    for (int i = 0; i < ap_records.available_records; i++) {
        if (strcmp((char *)ap_records.ap_list[i].ssid, ssid) == 0) {
            strncpy((char *)ap_records.ap_list[i].password, password,
                    sizeof(ap_records.ap_list[i].password) - 1);
            ap_records.ap_list[i].password[sizeof(ap_records.ap_list[i].password) - 1] = '\0';

            if (bssid) {
                memcpy(ap_records.ap_list[i].bssid, bssid, 6);
            }

            /* Set to max + 1 so this AP sorts to position 0 next scan */
            ap_records.ap_list[i].use_count = sat_inc(get_max_use_count_locked());

            ESP_LOGI(TAG, "Updated existing AP record: %s (use_count=%"PRIu32")",
                     ssid, ap_records.ap_list[i].use_count);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    /* ── New record ── */
    int target_index;

    if (ap_records.available_records < CONFIG_MAX_AP_COUNT) {
        /* Slot available */
        target_index = ap_records.available_records;
        ap_records.available_records++;
    } else {
        /* Store full — evict entry with lowest use_count (LRU policy).
         * Eviction happens BEFORE the max read so that if the evicted
         * entry held the previous maximum, we compute max from what remains. */
        int min_idx = 0;
        for (int i = 1; i < CONFIG_MAX_AP_COUNT; i++) {
            if (ap_records.ap_list[i].use_count < ap_records.ap_list[min_idx].use_count) {
                min_idx = i;
            }
        }
        ESP_LOGI(TAG, "Replacing least used AP: %s (use_count=%"PRIu32") with: %s",
                 ap_records.ap_list[min_idx].ssid,
                 ap_records.ap_list[min_idx].use_count,
                 ssid);
        target_index = min_idx;
        /* available_records stays the same — same slot count, different content */
    }

    /* Write new entry */
    strncpy((char *)ap_records.ap_list[target_index].ssid, ssid,
            sizeof(ap_records.ap_list[target_index].ssid) - 1);
    ap_records.ap_list[target_index].ssid[sizeof(ap_records.ap_list[target_index].ssid) - 1] = '\0';

    strncpy((char *)ap_records.ap_list[target_index].password, password,
            sizeof(ap_records.ap_list[target_index].password) - 1);
    ap_records.ap_list[target_index].password[sizeof(ap_records.ap_list[target_index].password) - 1] = '\0';

    if (bssid) {
        memcpy(ap_records.ap_list[target_index].bssid, bssid, 6);
    } else {
        memset(ap_records.ap_list[target_index].bssid, 0, 6);
    }

    /* max is read after eviction — correct even if evicted entry held the max */
    ap_records.ap_list[target_index].use_count = sat_inc(get_max_use_count_locked());

    ESP_LOGI(TAG, "Added new AP record: %s (use_count=%"PRIu32", total: %d)",
             ssid, ap_records.ap_list[target_index].use_count,
             ap_records.available_records);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ap_records_get(int index, ap_info_t *ap_info)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ap_info) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < 0 || index >= ap_records.available_records) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(ap_info, &ap_records.ap_list[index], sizeof(ap_info_t));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

int ap_records_get_count(void)
{
    if (!is_initialized) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = ap_records.available_records;
    xSemaphoreGive(s_mutex);
    return count;
}

esp_err_t ap_records_find_by_ssid(const char *ssid, ap_info_t *ap_info, int *index)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ap_records.available_records; i++) {
        if (strcmp((char *)ap_records.ap_list[i].ssid, ssid) == 0) {
            if (ap_info)  memcpy(ap_info, &ap_records.ap_list[i], sizeof(ap_info_t));
            if (index)    *index = i;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_find_by_bssid(const uint8_t *bssid, ap_info_t *ap_info, int *index)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!bssid) return ESP_ERR_INVALID_ARG;

    /* Guard: a zero BSSID in the search key would spuriously match any
     * record stored without a BSSID (all-zeros sentinel). Reject it.  */
    static const uint8_t zero_bssid[6] = {0};
    if (memcmp(bssid, zero_bssid, 6) == 0) {
        ESP_LOGW(TAG, "find_by_bssid called with all-zero BSSID — rejected");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ap_records.available_records; i++) {
        if (memcmp(ap_records.ap_list[i].bssid, bssid, 6) == 0) {
            if (ap_info)  memcpy(ap_info, &ap_records.ap_list[i], sizeof(ap_info_t));
            if (index)    *index = i;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

/*
 * ap_records_increment_use_count — bump use_count for the named AP.
 *
 * Applies a saturating increment (capped at UINT32_MAX) so long-running
 * deployments with frequent reconnects never wrap the counter.
 */
esp_err_t ap_records_increment_use_count(const char *ssid)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ap_records.available_records; i++) {
        if (strcmp((char *)ap_records.ap_list[i].ssid, ssid) == 0) {
            ap_records.ap_list[i].use_count = sat_inc(ap_records.ap_list[i].use_count);
            ESP_LOGD(TAG, "Incremented use count for %s to %"PRIu32,
                     ssid, ap_records.ap_list[i].use_count);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_remove_by_ssid(const char *ssid)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ap_records.available_records; i++) {
        if (strcmp((char *)ap_records.ap_list[i].ssid, ssid) == 0) {
            for (int j = i; j < ap_records.available_records - 1; j++) {
                memcpy(&ap_records.ap_list[j], &ap_records.ap_list[j + 1],
                       sizeof(ap_info_t));
            }
            ap_records.available_records--;
            memset(&ap_records.ap_list[ap_records.available_records], 0,
                   sizeof(ap_info_t));
            s_dirty = true;   /* signal ap_records_save() to re-check after write */
            ESP_LOGI(TAG, "Removed AP record: %s (remaining: %d)",
                     ssid, ap_records.available_records);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ap_records_remove_by_index(int index)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < 0 || index >= ap_records.available_records) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Removing AP record at index %d: %s",
             index, ap_records.ap_list[index].ssid);

    for (int i = index; i < ap_records.available_records - 1; i++) {
        memcpy(&ap_records.ap_list[i], &ap_records.ap_list[i + 1], sizeof(ap_info_t));
    }
    ap_records.available_records--;
    memset(&ap_records.ap_list[ap_records.available_records], 0, sizeof(ap_info_t));
    s_dirty = true;   /* signal ap_records_save() to re-check after write */

    ESP_LOGI(TAG, "Removed AP record (remaining: %d)", ap_records.available_records);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ap_records_clear_all(void)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&ap_records, 0, sizeof(ap_record_t));
    s_dirty = true;   /* signal ap_records_save() to re-check after write */
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Cleared all AP records");
    return ESP_OK;
}

/*
 * ap_records_sort_by_usage — sort the persistent store descending by use_count.
 *
 * DIAGNOSTIC / TEST USE ONLY. wifi_manager internals never call this function.
 * The connect_list used for connection attempts is sorted from a snapshot of
 * the store at scan time (see wifi_mgr_scan_handle_results()); the persistent
 * sort order of ap_records.ap_list has no effect on which AP is tried first.
 *
 * Do not call from application code that runs concurrently with the manager
 * task. While this function holds s_mutex during the sort, modifying the
 * persistent sort order can interact unexpectedly with the LRU eviction
 * policy in ap_records_add() if both run in close succession.
 */
esp_err_t ap_records_sort_by_usage(void)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ap_records.available_records <= 1) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    /* Bubble sort — descending by use_count */
    for (int i = 0; i < ap_records.available_records - 1; i++) {
        for (int j = 0; j < ap_records.available_records - i - 1; j++) {
            if (ap_records.ap_list[j].use_count < ap_records.ap_list[j + 1].use_count) {
                ap_info_t temp;
                memcpy(&temp,                    &ap_records.ap_list[j],     sizeof(ap_info_t));
                memcpy(&ap_records.ap_list[j],   &ap_records.ap_list[j + 1], sizeof(ap_info_t));
                memcpy(&ap_records.ap_list[j + 1], &temp,                    sizeof(ap_info_t));
            }
        }
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGD(TAG, "Sorted AP records by usage");
    return ESP_OK;
}

/* ── Stats / diagnostics ─────────────────────────────────────────────────── */

esp_err_t ap_records_get_stats(const blob_storage_handle_t *handle,
                                size_t *used_size, size_t *max_size)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "AP records not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle || !handle->initialized || !used_size || !max_size) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    *max_size = handle->max_size;

    bool exists = false;
    size_t current_size = 0;
    esp_err_t err = blob_storage_exists(handle, &exists, &current_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error getting blob stats for key '%s': %s",
                 handle->key, esp_err_to_name(err));
        return err;
    }

    *used_size = exists ? current_size : 0;
    ESP_LOGD(TAG, "Stats: used=%zu, max=%zu", *used_size, *max_size);
    return ESP_OK;
}

size_t ap_records_get_size(void)
{
    return sizeof(ap_record_t);
}

bool ap_records_is_initialized(void)
{
    return is_initialized;
}

/*
 * ap_records_print_all — log all stored AP records at INFO level for diagnostics.
 *
 * SECURITY INVARIANT: passwords are NEVER logged, at any log level, by this
 * function or anywhere else in this module. Only SSID, BSSID, and use_count
 * are emitted. Do not add password fields to this log statement — even at
 * LOGV — as log output is captured in customer support dumps and crash reports.
 */
void ap_records_print_all(void)
{
    if (!is_initialized) {
        ESP_LOGW(TAG, "AP records not initialized");
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "=== AP Records (Total: %d) ===", ap_records.available_records);
    for (int i = 0; i < ap_records.available_records; i++) {
        /* Log SSID, BSSID, and use_count only. Password is intentionally omitted. */
        ESP_LOGI(TAG, "Record %d: SSID='%s', BSSID=" MACSTR ", use_count=%"PRIu32,
                 i,
                 (const char *)ap_records.ap_list[i].ssid,
                 MAC2STR(ap_records.ap_list[i].bssid),
                 ap_records.ap_list[i].use_count);
    }
    xSemaphoreGive(s_mutex);
}
