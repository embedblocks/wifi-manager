/**
 * @file blob_storage.h
 * @brief Private NVS blob persistence layer for wifi_manager.
 *
 * VENDORED — local copy, not upstream-tracked.
 *
 * This header is in PRIV_INCLUDE_DIRS. Do not include it outside wifi_manager.
 * The NVS flash must be initialized (nvs_flash_init()) by the application
 * before blob_storage_init() is called.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for a named NVS blob.
 *
 * Created by blob_storage_create_handle(). Must be initialized before use.
 * Passed by pointer to all read/write operations.
 */
typedef struct {
    char   namespace[16];  /**< NVS namespace (max 15 chars + NUL) */
    char   key[16];        /**< NVS key (max 15 chars + NUL) */
    size_t max_size;       /**< Maximum blob size in bytes */
    bool   initialized;    /**< True after blob_storage_create_handle() succeeds */
} blob_storage_handle_t;

/**
 * @brief Initialize the blob storage system.
 *
 * Must be called once before any other blob_storage_* function.
 * nvs_flash_init() must have been called by the application beforehand.
 * Idempotent: safe to call multiple times.
 */
esp_err_t blob_storage_init(void);

/**
 * @brief Create a handle for a named NVS blob.
 *
 * Validates that max_size fits within the NVS single-blob limit (~4000 bytes).
 * Returns ESP_ERR_INVALID_SIZE if max_size exceeds the limit.
 *
 * @param[out] handle     Destination handle.
 * @param[in]  namespace  NVS namespace (max 15 chars).
 * @param[in]  key        NVS key (max 15 chars).
 * @param[in]  max_size   Maximum blob size in bytes.
 */
esp_err_t blob_storage_create_handle(blob_storage_handle_t *handle,
                                     const char *namespace,
                                     const char *key,
                                     size_t max_size);

/**
 * @brief Write data to NVS under the handle's namespace/key.
 *
 * Blocks ~10-20 ms for the NVS write and commit. Must not be called from
 * a timer callback or ISR.
 *
 * @param handle  Initialized storage handle.
 * @param data    Data to write.
 * @param size    Size of data in bytes (must be <= handle->max_size).
 */
esp_err_t blob_storage_write(const blob_storage_handle_t *handle,
                              const void *data, size_t size);

/**
 * @brief Read data from NVS under the handle's namespace/key.
 *
 * @param handle       Initialized storage handle.
 * @param[out] data    Destination buffer.
 * @param[in,out] size Input: buffer capacity. Output: bytes actually read.
 *                     Returns ESP_ERR_INVALID_SIZE if buffer is too small
 *                     (required_size is written to *size in that case).
 * @return ESP_OK, ESP_ERR_NVS_NOT_FOUND (no value stored), or error.
 */
esp_err_t blob_storage_read(const blob_storage_handle_t *handle,
                             void *data, size_t *size);

/**
 * @brief Check whether a blob exists in NVS.
 *
 * @param[out] exists  Set to true if the key exists.
 * @param[out] size    Size of the stored blob (may be NULL).
 */
esp_err_t blob_storage_exists(const blob_storage_handle_t *handle,
                               bool *exists, size_t *size);

/**
 * @brief Delete the blob from NVS.
 *
 * Returns ESP_ERR_NVS_NOT_FOUND if the key does not exist.
 */
esp_err_t blob_storage_delete(const blob_storage_handle_t *handle);

/**
 * @brief Query storage utilization.
 *
 * @param[out] used_size  Current blob size (0 if not stored).
 * @param[out] max_size   Maximum allowed size (handle->max_size).
 */
esp_err_t blob_storage_get_stats(const blob_storage_handle_t *handle,
                                  size_t *used_size, size_t *max_size);

/**
 * @brief Return true if blob_storage_init() has been called.
 */
bool blob_storage_is_initialized(void);

#ifdef __cplusplus
}
#endif
