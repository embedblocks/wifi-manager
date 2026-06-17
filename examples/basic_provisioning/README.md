# basic_provisioning — wifi_manager Example

This example demonstrates the full `wifi_manager` boot sequence on an ESP32,
including first-boot BLE provisioning and automatic reconnection on subsequent boots.

## What it does

1. Initialises NVS, `esp_netif`, and the default event loop (application's responsibility).
2. Registers a `WIFI_MGR_EVENT` handler that logs all state transitions.
3. Calls `wifi_mgr_init()` + `wifi_mgr_start()`.
4. Spawns a network task that blocks on `wifi_mgr_wait_for_connection()` and runs a heartbeat loop while connected.

### First boot (no stored credentials)

```
Scanning → live APs found → tw timer starts → tw expires → BLE provisioning
```

The device advertises over BLE as `PROV_<last3MAC>` (e.g. `PROV_EF0001`).
Open the **ESP BLE Prov** app (Android/iOS), select the device, and enter your
WiFi credentials. The device connects, stores the credentials in NVS, and
transitions to `CONNECTED`.

### Subsequent boots (credentials stored)

```
Scanning → live APs found → tw timer starts → known AP matched → CONNECTED
```

The device reconnects automatically without user interaction.

## Hardware required

Any ESP32 variant with BLE support (ESP32, ESP32-S3, ESP32-C3, ESP32-C6).

## Build and flash

```bash
cd examples/basic_provisioning
idf.py set-target esp32        # or esp32s3, esp32c3, esp32c6
idf.py build flash monitor
```

## Configuration

All timing and capacity values are tunable via `idf.py menuconfig` under
**WiFi Manager Configuration**. The `sdkconfig.defaults` file in this directory
uses the library defaults. Key knobs:

| Kconfig symbol | Default | Purpose |
|---|---|---|
| `CONFIG_WIFI_KNOWN_AP_WAIT_MS` | 120 000 ms | Time window before provisioning triggers |
| `CONFIG_WIFI_INTER_SCAN_DELAY_MS` | 3 000 ms | Delay between scan cycles |
| `CONFIG_WIFI_CONNECT_TIMEOUT_MS` | 8 000 ms | Per-attempt connection timeout |
| `CONFIG_WIFI_MAX_CONNECT_ATTEMPTS` | 3 | Total attempts per AP (1 initial + retries) before trying the next |
| `CONFIG_MAX_AP_COUNT` | 5 | Max stored credentials |
| `CONFIG_WIFI_PROV_DEVICE_NAME_PREFIX` | `"PROV_"` | BLE advertisement name prefix |

See `03_configuration.md` for the full tuning guide, including the stack budget
formula for `CONFIG_WIFI_SCAN_MAX_RESULTS`.

## Troubleshooting: binary too large for the factory partition

If you see an error like:

```
Error: app partition is too small for binary basic_provisioning.bin size 0x...:
  - Part 'factory' 0/0 @ 0x10000 size 0x100000 (overflow 0x...)
```

this almost always means the project picked up **Bluedroid** (the heavier
classic+BLE host) instead of **NimBLE** (BLE-only, roughly half the flash
footprint). This component's `sdkconfig.defaults` explicitly select NimBLE
(`CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BT_BLUEDROID_ENABLED=n`) for exactly
this reason — but if you already have a generated `sdkconfig` from before
adding this component (sdkconfig.defaults only seeds a *new* config; see the
note above in this README), the old host selection sticks.

Fix: run `idf.py menuconfig` → Component config → Bluetooth → Bluetooth Host,
and switch it to NimBLE. Or delete `sdkconfig` and `build/` and run
`idf.py build` again to regenerate from `sdkconfig.defaults`.

If your application genuinely needs classic Bluetooth alongside BLE
provisioning, Bluedroid is required and you'll need to either enlarge the
factory partition via a custom partition table, or move to a two-partition
(OTA) scheme. See `idf.py partition-table` and the ESP-IDF partition tables
guide.

## BT memory release after provisioning

`wifi_manager` already releases BT memory automatically once provisioning
completes — no application code needed. `wifi_mgr_prov.c` configures
`wifi_prov_mgr_init()` with `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`,
which frees both classic Bluetooth and BLE memory (recovering roughly
20–30 KB of heap) as soon as the provisioning manager is torn down.

**This is a one-way operation for the lifetime of the running firmware.**
If the device later needs to re-enter BLE provisioning (e.g. a factory-reset
button), a reboot is required — BT cannot be re-initialized after this
release without one. See `L8` in `01_overview.md` for the full trade-off
discussion.

If your product needs classic Bluetooth to keep running after WiFi
provisioning finishes (uncommon), change the handler constant in
`wifi_mgr_prov.c` from `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM` to
`WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE`, which frees only the BLE
memory and leaves classic BT intact.

## Credential management at runtime

```c
/* Add a credential (e.g. from a secondary config mechanism) */
wifi_mgr_add_ap("MySSID", "MyPassword", NULL /* no BSSID */);

/* Remove a credential */
wifi_mgr_remove_ap("MySSID");

/* List stored APs (passwords are NOT returned) */
wifi_mgr_ap_info_t list[5];
uint8_t count = 0;
wifi_mgr_get_stored_aps(list, &count, 5);
for (int i = 0; i < count; i++) {
    ESP_LOGI(TAG, "Stored AP: %s (used %lu times)", list[i].ssid, list[i].use_count);
}
```

## Event-driven pattern reference

See `09_event_system.md` for all event IDs, their payload types, and the five
usage patterns (`A` through `E`) for `wifi_mgr_wait_for_connection()`.
