# wifi-manager

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5%20%7C%20v6.0-blue)
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

Requires **ESP-IDF v5.5 or later** — tested on v5.5.2 and v6.0.3. Users of
0.1.x on ESP-IDF v5.5 can move to this version directly.

```bash
idf.py add-dependency "embedblocks/wifi-manager^0.4.1"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  embedblocks/wifi-manager: "^0.4.1"
```

`espressif/network_provisioning` (the registry replacement for the
`wifi_provisioning` component that ESP-IDF v6.0 removed) is pulled in
automatically as a transitive dependency, on v5.5 as well as v6.0.

On ESP-IDF v5.x, if your application *also* uses the built-in
`wifi_provisioning` component directly, add
`set(EXCLUDE_COMPONENTS wifi_provisioning)` to your project's
`CMakeLists.txt` or move that code to `network_provisioning` too
(Espressif's migration guide recommends this when both are present).

---

## Integration checklist

1. Call `nvs_flash_init()`, `esp_netif_init()` and
   `esp_event_loop_create_default()` before `wifi_mgr_init()`. Do **not**
   create the STA netif yourself.
2. **Provide a Wi-Fi setup trigger.** A device provisions on its own only
   while it holds no credentials. To add a network later (device moved,
   router replaced, password changed), your application must call
   `wifi_mgr_reboot_into_setup()` — from a button, an app command, a
   power-cycle gesture, whatever your product has. The component contains
   no GPIO code. `examples/basic_provisioning` shows a setup button built
   on `espressif/button` (hold 3 s = setup, hold 10 s = forget all
   networks, decided on release).
   Without a trigger, a provisioned device can never get a new network
   without reflashing; the component logs a hint once per boot when it
   notices.
3. Choose `CONFIG_WIFI_PS_MODE` (or `wifi_mgr_config_t.ps_mode`). Use
   `NONE` for audio or other latency-sensitive traffic. Don't call
   `esp_wifi_set_ps()` yourself.
4. Choose a PoP mode (`CONFIG_WIFI_PROV_AUTH_MODE_*`).

### Setup mode in short

- `wifi_mgr_reboot_into_setup()` reboots (it does not return) and the
  device comes up in BLE provisioning. Stored networks are kept. Save
  application state first with `esp_register_shutdown_handler()` if needed.
- `wifi_mgr_forget_all_and_reboot_into_setup()` forgets every network
  first; if that can't be committed to NVS it returns the error and does
  not reboot.
- With credentials stored, setup mode ends after
  `CONFIG_WIFI_SETUP_DEADLINE_MS` (5 min), extended while a phone is
  connected or credentials are being tried, capped at
  `CONFIG_WIFI_SETUP_MAX_MS` (15 min). The device then returns to its
  stored networks. A wrong password just restarts the attempt.
- `wifi_mgr_start_ble_provisioning()` enters setup mode without a reboot,
  for applications that must keep running.

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

## Example

The `examples/basic_provisioning` directory contains a fully working example
covering first-boot BLE provisioning and automatic reconnection on subsequent
boots. It is the reference integration for this component.

```bash
cd examples/basic_provisioning
idf.py set-target esp32        # or esp32s3, esp32c3, esp32c6
idf.py build flash monitor
```

**First boot** (no stored credentials): the device scans, waits for the
time-window deadline, then opens a BLE session advertised as
`PROV_<last3MAC>`. Open the **ESP BLE Prov** app (Android/iOS), select the
device, enter your WiFi credentials. The device connects, stores the
credential, and transitions to `CONNECTED`.

**Subsequent boots**: connects automatically to the stored AP, no user
interaction.

See `examples/basic_provisioning/README.md` for the full guide, including
auth mode selection (open / static PoP / MAC-derived PoP), headless device
setup, partition table notes, and credential management at runtime.

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
| `CONFIG_WIFI_SETUP_DEADLINE_MS` | 300 000 | Setup mode length when credentials are stored |
| `CONFIG_WIFI_SETUP_GRACE_MS` | 60 000 | Extension while a phone is connected / credentials are tried |
| `CONFIG_WIFI_SETUP_MAX_MS` | 900 000 | Hard cap on setup mode when credentials are stored |
| `CONFIG_WIFI_PS_MODE` | `MIN_MODEM` | Wi-Fi power save (also `wifi_mgr_config_t.ps_mode`) |
| `CONFIG_WIFI_AUTO_PROVISION` | `WHEN_UNPROVISIONED` | When tw expiry may start BLE provisioning on its own (also `wifi_mgr_config_t.auto_provision`) |
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
case WIFI_MGR_EVENT_KNOWN_AP_UNREACHABLE: {
    /* stored network in range, but every attempt fails (password changed?).
     * Never auto-re-provisions — prompt the user, then call
     * wifi_mgr_start_ble_provisioning() if they confirm. */
    wifi_mgr_known_ap_unreachable_t *u = data;
    ESP_LOGW(TAG, "%s unreachable, reason %u", u->ssid, u->last_reason);
    break;
}
```

Full event reference in `docs/09_event_system.md`.

---

## Partition table

WiFi + BLE (NimBLE) + `network_provisioning` + mbedTLS together exceed the
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

- **Adding a network to a provisioned device needs a setup trigger** (see
  the Integration checklist). Under the default `auto_provision` policy it
  never re-enters setup mode by itself — not after a move, a router
  replacement or a password change; it posts `NO_KNOWN_AP_IN_TW` /
  `KNOWN_AP_UNREACHABLE` and keeps trying.
- **Hidden SSIDs** require `CONFIG_WIFI_SCAN_HIDDEN=y`
- **Duplicate SSIDs** (two networks, same name, different passwords) —
  only one credential can be stored; SSID is the primary key
- **OTA struct changes** to `ap_record_t` wipe stored credentials on
  first boot after update — plan a migration strategy before shipping
- **Bluetooth memory release** after provisioning
  (`CONFIG_WIFI_PROV_BT_LIFECYCLE_FREE_ON_SUCCESS`) is one-way per boot:
  entering setup mode again reboots the device into setup mode first.
  `KEEP_ALIVE` (the default) avoids the reboot at the cost of the reserved
  memory. Don't combine `FREE_ON_SUCCESS` with `auto_provision = ALWAYS`.

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
| `docs/10_integration.md` | Boot sequences and integration patterns |
| `docs/11_test_plan_0.3.0.md` | Hardware test plan (0.3.x – 0.4.0) |
| `docs/12_setup_mode.md` | **Current reference:** setup mode, provisioning policy, driver NVS handling, Bluetooth lifecycle |

---

## License

MIT — see LICENSE file.