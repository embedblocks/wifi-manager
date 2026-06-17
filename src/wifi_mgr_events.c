/**
 * @file wifi_mgr_events.c
 * @brief Event posting implementation for wifi_manager.
 */

#include "wifi_mgr_events.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "wifi_mgr";

/* Event base definition — declared extern in wifi_manager.h. */
ESP_EVENT_DEFINE_BASE(WIFI_MGR_EVENT);

void wifi_mgr_events_post(wifi_mgr_event_id_t id, const void *data, size_t data_size)
{
    esp_err_t ret = esp_event_post(WIFI_MGR_EVENT, (int32_t)id,
                                   data, data_size,
                                   0 /* timeout: 0 ticks, non-blocking */);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post event %d: %s", (int)id, esp_err_to_name(ret));
    }
}
