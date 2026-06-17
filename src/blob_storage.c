/* VENDORED — local copy, not upstream-tracked */
/* blob_storage.c */
#include "blob_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "BLOB_STORAGE";
static bool storage_system_initialized = false;

/*
 * NVS limits a single blob value to NVS_PAGE_SIZE - overhead.
 * In the default ESP-IDF NVS configuration this is approximately 4000 bytes.
 * Blobs larger than this will fail at nvs_set_blob() with
 * ESP_ERR_NVS_VALUE_TOO_LONG. Catch the misconfiguration at handle creation
 * time so the error is surfaced during init rather than at the first write.
 */
#define BLOB_STORAGE_MAX_SUPPORTED_SIZE  4000U

esp_err_t blob_storage_init(void)
{
    if (storage_system_initialized) {
        ESP_LOGD(TAG, "Blob storage already initialized");
        return ESP_OK;
    }

    /* nvs_flash_init() must have been called by the application before this.
     * This component does not manage NVS flash initialisation to avoid
     * conflicts when NVS is shared with other components.               */

    storage_system_initialized = true;
    ESP_LOGI(TAG, "Blob storage system initialized");
    return ESP_OK;
}

esp_err_t blob_storage_create_handle(blob_storage_handle_t *handle,
                                     const char *namespace,
                                     const char *key,
                                     size_t max_size)
{
    if (!storage_system_initialized) {
        ESP_LOGE(TAG, "Blob storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle || !namespace || !key) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(namespace) >= sizeof(handle->namespace) ||
        strlen(key)       >= sizeof(handle->key)) {
        ESP_LOGE(TAG, "Namespace or key too long");
        return ESP_ERR_INVALID_ARG;
    }
    if (max_size == 0) {
        ESP_LOGE(TAG, "Max size cannot be zero");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Catch max_size values that exceed what NVS can store in a single
     * blob entry. Without this check, blob_storage_write() would succeed
     * here and fail silently at runtime with ESP_ERR_NVS_VALUE_TOO_LONG.
     */
    if (max_size > BLOB_STORAGE_MAX_SUPPORTED_SIZE) {
        ESP_LOGE(TAG,
                 "max_size %zu exceeds NVS single-blob limit (%u bytes). "
                 "Reduce the data structure size or split across multiple keys.",
                 max_size, BLOB_STORAGE_MAX_SUPPORTED_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(handle->namespace, namespace, sizeof(handle->namespace) - 1);
    handle->namespace[sizeof(handle->namespace) - 1] = '\0';

    strncpy(handle->key, key, sizeof(handle->key) - 1);
    handle->key[sizeof(handle->key) - 1] = '\0';

    handle->max_size    = max_size;
    handle->initialized = true;

    ESP_LOGD(TAG, "Created storage handle: namespace='%s', key='%s', max_size=%zu",
             handle->namespace, handle->key, handle->max_size);
    return ESP_OK;
}

esp_err_t blob_storage_write(const blob_storage_handle_t *handle,
                              const void *data, size_t size)
{
    if (!storage_system_initialized) {
        ESP_LOGE(TAG, "Blob storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Invalid or uninitialized handle");
        return ESP_ERR_INVALID_ARG;
    }
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Invalid data or size");
        return ESP_ERR_INVALID_ARG;
    }
    if (size > handle->max_size) {
        ESP_LOGE(TAG, "Data size %zu exceeds maximum %zu", size, handle->max_size);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(handle->namespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for namespace '%s': %s",
                 handle->namespace, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, handle->key, data, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing blob to key '%s': %s",
                 handle->key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing blob to NVS: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Wrote %zu bytes to namespace='%s', key='%s'",
             size, handle->namespace, handle->key);
    return ESP_OK;
}

esp_err_t blob_storage_read(const blob_storage_handle_t *handle,
                             void *data, size_t *size)
{
    if (!storage_system_initialized) {
        ESP_LOGE(TAG, "Blob storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Invalid or uninitialized handle");
        return ESP_ERR_INVALID_ARG;
    }
    if (!data || !size || *size == 0) {
        ESP_LOGE(TAG, "Invalid data buffer or size");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(handle->namespace, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for namespace '%s': %s",
                 handle->namespace, esp_err_to_name(err));
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_blob(nvs_handle, handle->key, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "Blob not found for key '%s'", handle->key);
        nvs_close(nvs_handle);
        return ESP_ERR_NVS_NOT_FOUND;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error getting blob size for key '%s': %s",
                 handle->key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    if (*size < required_size) {
        ESP_LOGE(TAG, "Buffer too small: required %zu, provided %zu",
                 required_size, *size);
        nvs_close(nvs_handle);
        *size = required_size;
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_blob(nvs_handle, handle->key, data, size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading blob from key '%s': %s",
                 handle->key, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Read %zu bytes from namespace='%s', key='%s'",
             *size, handle->namespace, handle->key);
    return ESP_OK;
}

esp_err_t blob_storage_exists(const blob_storage_handle_t *handle,
                               bool *exists, size_t *size)
{
    if (!storage_system_initialized) {
        ESP_LOGE(TAG, "Blob storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle || !handle->initialized || !exists) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(handle->namespace, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for namespace '%s': %s",
                 handle->namespace, esp_err_to_name(err));
        return err;
    }

    size_t blob_size = 0;
    err = nvs_get_blob(nvs_handle, handle->key, NULL, &blob_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *exists = false;
        if (size) *size = 0;
        ESP_LOGD(TAG, "Blob does not exist for key '%s'", handle->key);
        return ESP_OK;
    } else if (err == ESP_OK) {
        *exists = true;
        if (size) *size = blob_size;
        ESP_LOGD(TAG, "Blob exists for key '%s', size=%zu", handle->key, blob_size);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Error checking blob existence for key '%s': %s",
                 handle->key, esp_err_to_name(err));
        return err;
    }
}

esp_err_t blob_storage_delete(const blob_storage_handle_t *handle)
{
    if (!storage_system_initialized) {
        ESP_LOGE(TAG, "Blob storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Invalid or uninitialized handle");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(handle->namespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for namespace '%s': %s",
                 handle->namespace, esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(nvs_handle, handle->key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error deleting blob for key '%s': %s",
                 handle->key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    esp_err_t commit_err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing deletion to NVS: %s",
                 esp_err_to_name(commit_err));
        return commit_err;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "Blob not found for deletion, key '%s'", handle->key);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    ESP_LOGD(TAG, "Deleted blob for namespace='%s', key='%s'",
             handle->namespace, handle->key);
    return ESP_OK;
}

esp_err_t blob_storage_get_stats(const blob_storage_handle_t *handle,
                                  size_t *used_size, size_t *max_size)
{
    if (!storage_system_initialized) {
        ESP_LOGE(TAG, "Blob storage not initialized");
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
    ESP_LOGD(TAG, "Stats for namespace='%s', key='%s': used=%zu, max=%zu",
             handle->namespace, handle->key, *used_size, *max_size);
    return ESP_OK;
}

bool blob_storage_is_initialized(void)
{
    return storage_system_initialized;
}
