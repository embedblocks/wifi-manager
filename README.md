# wifi-manager

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

Full WiFi lifecycle management for ESP-IDF — one component, one task, one
event interface. Your application calls `wifi_mgr_start()` and observes
`WIFI_MGR_EVENT_*` events. It never calls `esp_wifi` directly.

The component handles everything in between: continuous scanning until a
live AP appears, a configurable time-window deadline after which BLE
provisioning starts automatically, credential-based reconnection with
LRU priority across up to `CONFIG_MAX_AP_COUNT` stored networks,
NVS persistence, disconnect recovery, and BLE provisioning that retries
in place on timeout rather than requiring a reboot.

---

## What it handles

Getting a device onto WiFi for the first time is the easy part. The harder
part is everything that happens afterward: the disconnect at 3 AM, the AP
that reboots and changes channel, the office site with dozens of visible
networks, the device that needs to re-provision without a reboot. This
component is designed around those cases.

- **Continuous scanning** until at least one live AP is visible
- **Time-window deadline** — if a known AP doesn't appear within the
  configured window, BLE provisioning starts automatically
- **Credential store** with LRU priority — up to `CONFIG_MAX_AP_COUNT`
  networks, ranked by connection history, persisted to NVS
- **Disconnect recovery** — on any disconnect the full scan-and-reconnect
  cycle restarts from scratch
- **BLE provisioning via ESP Unified Provisioning** — retry-in-place on
  timeout or failure, no BT controller reinit required, configurable
  BT memory lifecycle after success
- **Event interface** — `WIFI_MGR_EVENT_*` events on the default loop;
  `wifi_mgr_wait_for_connection()` for tasks that block until connected
- **No heap allocation after `wifi_mgr_init()`** — all buffers are
  statically sized at build time
- **Thread-safe** — all state access is mutex-protected; safe to call from
  any task

---

## Chip support

| Chip     | Status              |
|----------|---------------------|
| ESP32    | Tested              |
| ESP32-S3 | Expected to work    |
| ESP32-C3 | Expected to work    |
| ESP32-C6 | Expected to work    |

---

## Installation

```bash
idf.py add-dependency "embedblocks/wifi-manager^0.1.0"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  embedblocks/wifi-manager: "^0.1.0"
```

---

## Usage

```c
#include "wifi_manager.h"

void app_main(void)
{
    /* 1. NVS, netif layer, and event loop — application's responsibility */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Do NOT call esp_netif_create_default_wifi_sta() —
     * wifi_mgr_start() creates and owns the STA netif. */

    /* 2. Register for events (optional but typical) */
    esp_event_handler_register(WIFI_MGR_EVENT, ESP_EVENT_ANY_ID,
                               your_event_handler, NULL);

    /* 3. Init and start — zero-init config uses all Kconfig defaults */
    wifi_mgr_config_t config = {0};
    ESP_ERROR_CHECK(wifi_mgr_init(&config));
    ESP_ERROR_CHECK(wifi_mgr_start());

    /* 4. Block until connected (or loop on reconnect) */
    wifi_mgr_wait_for_connection(0);   /* 0 = block forever */
    /* ... do application work ... */
}
```

On first boot with no stored credentials the device scans, waits for the
time-window deadline, then opens a BLE provisioning session. Use the
**ESP BLE Prov** app (Android / iOS) to send WiFi credentials. On all
subsequent boots it connects automatically using stored credentials.

---

## State machine

```
boot
 │
 ▼
SCANNING ◄────────────────────────────────────────┐
 │                                                 │
 │ live AP visible                      disconnect │
 ▼                                                 │
TW_ACTIVE ──── tw expires, no known AP ──► PROVISIONING
 │                                        (retry in place
 │ known AP visible                        on timeout)
 ▼
CONNECTING
 │
 │ connected ──── tw expires ───────────────────────►(PROVISIONING)
 ▼
CONNECTED
```

The time window (`CONFIG_WIFI_KNOWN_AP_WAIT_MS`, default 120 s) is a hard
deadline. If it expires before a known AP is found and connected,
provisioning starts regardless of connection state. Provisioning retries
in place on failure or timeout — no reboot required, no BT controller
reinit, indefinitely.

---

## Configuration

All timing and behaviour is controlled via Kconfig
(`idf.py menuconfig → wifi_manager`). Key values:

| Kconfig symbol | Default | Description |
|---|---|---|
| `CONFIG_MAX_AP_COUNT` | 5 | Max stored credentials |
| `CONFIG_WIFI_KNOWN_AP_WAIT_MS` | 120 000 | Time-window deadline (ms) |
| `CONFIG_WIFI_INTER_SCAN_DELAY_MS` | 3 000 | Delay between scan cycles |
| `CONFIG_WIFI_CONNECT_TIMEOUT_MS` | 8 000 | Per-AP connection timeout |
| `CONFIG_WIFI_MAX_CONNECT_ATTEMPTS` | 3 | Retries per AP per TW cycle |
| `CONFIG_WIFI_PROV_TIMEOUT_MS` | 300 000 | BLE session timeout before retry |
| `CONFIG_WIFI_PROV_BT_LIFECYCLE` | `KEEP_ALIVE` | BT memory after provisioning success |
| `CONFIG_WIFI_PROV_BLE_RETRY_BACKOFF_MS` | 2 000 | Backoff on sync provisioning start failure |

See `docs/03_configuration.md` for the full reference and tuning guidance.

---

## Events

```c
case WIFI_MGR_EVENT_CONNECTED: {
    wifi_mgr_connected_info_t *info = data;
    ESP_LOGI(TAG, "connected: %s  " IPSTR, info->ssid,
             IP2STR((ip4_addr_t *)&info->ip));
    break;
}
case WIFI_MGR_EVENT_PROVISIONING_STARTED:
    ESP_LOGI(TAG, "open ESP BLE Prov and follow the wizard");
    break;
case WIFI_MGR_EVENT_DISCONNECTED:
    /* manager handles reconnect automatically — no action needed */
    break;
```

Full event reference in `docs/09_event_system.md`.

---

## Partition table

WiFi + BLE (NimBLE) + `wifi_provisioning` + mbedTLS together exceed the
default 1 MB single-factory partition on ESP32. The included example uses
a custom 1.5 MB partition table (`examples/basic_provisioning/partitions.csv`).
Use it as a starting point for your own project.

---

## Threading model

One FreeRTOS task (`wifi_mgr`, configurable priority and stack via Kconfig)
owns all WiFi state. Your application interacts with it through:

- **Events** posted to the default event loop — observe state changes
- **`wifi_mgr_wait_for_connection(timeout_ms)`** — block until connected
- **`wifi_mgr_is_connected()`** — non-blocking point-in-time check
- **`wifi_mgr_stop()`** — clean shutdown within a bounded time

No direct `esp_wifi_*` calls are needed or expected from application code.

---

## Known limitations

A full list with mitigations is in `docs/01_overview.md`. The most
operationally relevant:

- **Hidden SSIDs** require `CONFIG_WIFI_SCAN_HIDDEN=y`
- **Duplicate SSIDs** (two networks, same name, different passwords) —
  only one credential can be stored; SSID is the primary key
- **OTA struct changes** to `ap_record_t` wipe stored credentials on
  first boot after update — plan a migration strategy before shipping
- **BT memory release** after provisioning
  (`CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS`) is a one-way
  operation per boot — BLE re-provisioning without reboot requires
  `KEEP_ALIVE` (the default)

---

## Documentation

| Document | Contents |
|---|---|
| `docs/01_overview.md` | Requirements, limitations, dependency graph |
| `docs/02_state_machine.md` | Full state machine with transition table |
| `docs/03_configuration.md` | All Kconfig symbols, tuning guidance |
| `docs/04_public_api.md` | Public API reference with pseudocode |
| `docs/05_internal_architecture.md` | Task, event loop, notification design |
| `docs/06_scan_flow.md` | Scan cycle internals |
| `docs/07_connect_flow.md` | Connection and retry logic |
| `docs/08_provisioning_flow.md` | BLE provisioning, retry policy, race condition fixes |
| `docs/09_event_system.md` | Event reference and usage patterns |

---

## License

MIT — see LICENSE file.