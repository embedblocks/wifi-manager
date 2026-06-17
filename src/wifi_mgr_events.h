/**
 * @file wifi_mgr_events.h
 * @brief Internal event posting wrapper for wifi_manager.
 *
 * Thin wrapper around esp_event_post() targeting the WIFI_MGR_EVENT base.
 * The event loop copies event data before returning; the data pointer does
 * not need to remain valid after the call.
 *
 * Private — do not include outside wifi_manager.
 */

#pragma once

#include "wifi_manager.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Post a wifi_manager event to the default event loop.
 *
 * @param id        Event ID from wifi_mgr_event_id_t.
 * @param data      Pointer to event data (may be NULL if data_size == 0).
 * @param data_size Size of event data in bytes (0 if no data).
 */
void wifi_mgr_events_post(wifi_mgr_event_id_t id, const void *data, size_t data_size);

#ifdef __cplusplus
}
#endif
