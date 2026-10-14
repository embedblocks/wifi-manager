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

## Choosing a provisioning auth mode (including headless devices)

`CONFIG_WIFI_PROV_AUTH_MODE` (under **WiFi Manager** → *Provisioning security
mode* in menuconfig) is a three-way choice:

- **Open** — no PoP, no encryption. Bench testing only.
- **Static PoP** — a fixed string baked into the firmware via
  `CONFIG_WIFI_PROV_POP`. **This is the right choice for headless devices**
  (no UART/console exposed to the operator): the PoP is known ahead of time
  by whoever builds the firmware or operates the provisioning tooling, so
  there's nothing that needs to be read off the device at provisioning time.
- **MAC-derived** (default) — a per-device PoP derived from the device's own
  MAC address, printed to the serial console when provisioning starts. Unique
  per unit with zero extra configuration, but requires console access to read
  the value, so it doesn't work for headless devices.

To switch to static PoP, in `sdkconfig.defaults`:

```
CONFIG_WIFI_PROV_AUTH_MODE_STATIC_POP=y
CONFIG_WIFI_PROV_POP="CHANGEME-1234"
```

**Replace `CHANGEME-1234` with your own value before flashing.** If you
forget and the BLE Prov app reports a failure (handshake/auth error) even
though you're typing in what you think is the right PoP, check that the
value you're entering in the app actually matches what's compiled into
`CONFIG_WIFI_PROV_POP` — a mismatch here is the single most common cause
of static-mode provisioning failures, and it fails silently from the
device's side (BLE auth just won't complete; there's no specific error
message pointing at "wrong PoP").

The string must be under 64 characters; an oversized value fails the
*build* with a clear error (rather than being silently truncated, which
used to be a real bug here) — see `WIFI_MGR_PROV_POP_BUF_SIZE` in
`wifi_mgr_ctx.h` if you ever need to raise that limit.

or via `idf.py menuconfig`. Treat the static PoP as a shared secret across
every unit built from that firmware image, not a per-device credential —
anyone with the value can provision any unit running it. If you need a true
per-device secret without console access, that requires either a factory
provisioning step that injects a unique value into NVS before deployment, or
a custom `wifi_mgr_config_t`-level override (not currently exposed by this
component — see the PoP resolution logic in `wifi_mgr_prov.c` if you need to
extend it).

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

## Wi-Fi setup button

Once the device holds credentials it never enters setup mode (BLE
provisioning) by itself, so adding a network later — device moved, router
replaced, password changed — needs a trigger. This example uses the board's
BOOT button (GPIO0 on ESP32/ESP32-S3, GPIO9 on ESP32-C3/C6) via
[`espressif/button`](https://components.espressif.com/components/espressif/button),
declared in `main/idf_component.yml`. That dependency belongs to the example
only; `wifi_manager` itself has no GPIO code.

| Hold, then release | Action |
|---|---|
| ≥ 3 s | `wifi_mgr_reboot_into_setup()` — reboot into setup mode, stored networks kept |
| ≥ 10 s | `wifi_mgr_forget_all_and_reboot_into_setup()` — forget all networks, then setup |

The action is decided on release, so holding for 10 s never triggers the
3 s action; the console says "release now: …" as each threshold is reached.
Pin, polarity and hold times are under `idf.py menuconfig` →
"basic_provisioning example". The button callbacks run in the `esp_timer`
task, so they only hand the action to a small task (forget-all writes NVS).

Holding BOOT while powering up or resetting enters the ROM download mode
instead of the application; that is fine after boot.

With credentials stored, setup mode ends after 5 min (extended while a phone
is connected, at most 15 min) and the device goes back to its stored networks.

## Bluetooth memory after provisioning

Controlled by `CONFIG_WIFI_PROV_BT_LIFECYCLE`:

- `KEEP_ALIVE` (default): Bluetooth memory stays reserved, so setup mode can
  be entered again at any time without a reboot.
- `FREE_ON_SUCCESS`: after a successful session the memory is released
  (`esp_bt_mem_release()`, one-way per boot). Entering setup mode again in the
  same boot reboots the device into setup mode first.

The BLE controller itself is torn down after every session under both
options; only the memory release is irreversible. Measure free internal heap
on your target before choosing `FREE_ON_SUCCESS` (the commonly quoted saving
of ~20–30 KB has not been measured for this component). See
`docs/12_setup_mode.md`.

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
