/**
 * @file wifi_mgr_state.c
 * @brief Thread-safe state management for wifi_manager.
 */

#include "wifi_mgr_state.h"
#include "wifi_mgr_ctx.h"
#include "esp_log.h"

static const char *TAG = "wifi_mgr";

wifi_mgr_state_t wifi_mgr_state_get(void)
{
    xSemaphoreTake(CTX->state_mutex, portMAX_DELAY);
    wifi_mgr_state_t s = CTX->state;
    xSemaphoreGive(CTX->state_mutex);
    return s;
}

void wifi_mgr_state_set(wifi_mgr_state_t new_state)
{
    xSemaphoreTake(CTX->state_mutex, portMAX_DELAY);
    wifi_mgr_state_t old_state = CTX->state;
    CTX->state = new_state;
    /* Clear per-state-entry flag so truncation warning re-fires in the new state. */
    CTX->scan_truncation_warned = false;
    xSemaphoreGive(CTX->state_mutex);

    if (old_state != new_state) {
        ESP_LOGD(TAG, "State: %s → %s",
                 wifi_mgr_state_str(old_state),
                 wifi_mgr_state_str(new_state));
    }
}

const char *wifi_mgr_state_str(wifi_mgr_state_t state)
{
    switch (state) {
    case WIFI_MGR_STATE_IDLE:         return "IDLE";
    case WIFI_MGR_STATE_SCANNING:     return "SCANNING";
    case WIFI_MGR_STATE_TW_ACTIVE:    return "TW_ACTIVE";
    case WIFI_MGR_STATE_CONNECTED:    return "CONNECTED";
    case WIFI_MGR_STATE_PROVISIONING: return "PROVISIONING";
    case WIFI_MGR_STATE_ERROR:        return "ERROR";
    case WIFI_MGR_STATE_RESCAN_ONCE:  return "RESCAN_ONCE";
    default:                          return "UNKNOWN";
    }
}
