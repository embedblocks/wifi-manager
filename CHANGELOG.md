# Changelog

All notable changes to espnow_mqtt will be documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.4.1] — ESP-IDF v5.5 support

### Changed
- **ESP-IDF v5.5 is supported again**, in addition to v6.0:
  `idf: ">=5.5.0"` (was `">=6.0.0"`). The 0.2.0 switch to
  `espressif/network_provisioning` was never v6-specific — that component
  supports IDF >= 5.1 — only the manifest constraint was. Tested on v5.5.2
  (built and run on hardware) and v6.0.3. No code changes; 0.1.x users on
  v5.5 can upgrade directly.
- README, Kconfig help and docs: version requirement updated to v5.5+; note
  on excluding the built-in `wifi_provisioning` on v5.x if an application
  also uses it.

---

## [0.4.0] — Wi-Fi setup trigger and power save

### Added
- `wifi_mgr_reboot_into_setup()`: reboot straight into BLE provisioning
  ("setup mode"), keeping stored networks. The call to wire to a product's
  setup trigger (button, app command, power-cycle gesture). Uses a magic
  value in RTC no-init memory, read and cleared by `wifi_mgr_init()` on
  every boot and honoured only after a software reset (not after power-on,
  a crash or deep-sleep wakeup). Does not return.
- `wifi_mgr_forget_all_and_reboot_into_setup()`: forget every network,
  commit the empty store to NVS, then reboot into setup mode. On a commit
  failure it restores RAM from NVS, returns the error and does not reboot.
- Setup-mode deadline when credentials are stored:
  `CONFIG_WIFI_SETUP_DEADLINE_MS` (300 s), extended by
  `CONFIG_WIFI_SETUP_GRACE_MS` (60 s) while a phone is connected over BLE
  or received credentials are pending, capped by `CONFIG_WIFI_SETUP_MAX_MS`
  (15 min). Failed attempts restart in place inside the deadline. At the
  end: session stopped, held credentials discarded, station confirmed
  disconnected, driver copy wiped, back to scanning. Nothing from a session
  ended this way is saved, even if it connects at that moment. With no
  credentials stored, setup mode lasts until provisioning succeeds, as
  before.
- `ps_mode` (`CONFIG_WIFI_PS_MODE`, `wifi_mgr_config_t.ps_mode`,
  `wifi_mgr_ps_mode_t`): applied after `esp_wifi_start()` and after every
  provisioning session.
- Once-per-boot log hint, when no stored network is reachable, that the
  application must provide a setup trigger. README "Integration checklist".
- Example: Wi-Fi setup button in `examples/basic_provisioning`, built on
  `espressif/button` (example-only dependency): BOOT button by default; hold
  3 s = setup, 10 s = forget all, decided on release; callbacks hand the
  action to a task because they run in the `esp_timer` task.
- Example README: the Bluetooth memory section described the release as
  automatic; it only happens under `FREE_ON_SUCCESS` (not the default).

### Fixed
- **`FREE_ON_SUCCESS` never rebooted when BLE was needed again.** The
  `bt_mem_freed` check ran only when `network_prov_mgr_init()` failed,
  which it never does; the failure surfaces in
  `network_prov_mgr_start_provisioning()`. The check now runs before either
  call, and the reboot goes through the setup flag — a plain
  `esp_restart()` would have come back connected (a provisioned device
  never provisions by itself since 0.3.0), silently dropping the request.
- **`wifi_mgr_start_ble_provisioning()` from CONNECTED broke setup mode.**
  The resulting disconnect made `on_disconnect()` overwrite PROVISIONING
  with SCANNING, so the manager scanned while BLE provisioning ran and
  ignored its result. `on_disconnect()` now leaves PROVISIONING alone.
- A setup-mode start that failed in the caller's task could leave the
  manager task blocked in a wait for the old state. It is now woken.
- In SCANNING and RESCAN_ONCE, an unrelated notification during a scan
  could be taken for the scan watchdog (aborting the scan, or overwriting
  a state set by another task). Both now keep waiting or re-dispatch.
- The manager task could briefly observe the IDLE state at start-up.

### Changed
- Bluetooth lifecycle help text and code comments corrected: the BLE
  controller is torn down at every session stop under both options; only
  `esp_bt_mem_release()` is irreversible. `KEEP_ALIVE` keeps memory
  reserved, not the controller running. Behaviour unchanged.
- `WIFI_PROV_TIMEOUT_MS` now limits each attempt; with credentials stored
  the setup deadline limits the whole setup mode (whichever ends first).
- `auto_provision = ALWAYS` with `FREE_ON_SUCCESS` logs a start-up warning
  (one reboot per automatic setup mode during an outage).

### Migration
- Recompile: `wifi_mgr_config_t` grew `ps_mode`.
- Wire `wifi_mgr_reboot_into_setup()` to a trigger (see the Integration
  checklist).
- If you called `esp_wifi_set_ps()` yourself, use `ps_mode` instead —
  the component re-applies its own setting after provisioning.
- A manually started setup on a provisioned device now ends after the
  deadline instead of retrying forever.

---

## [0.3.1] — Review fixes for 0.3.0

### Fixed
- **The driver-config wipe after a failed session could fail silently**,
  leaving an unverified password in the driver's NVS (`nvs.net80211`).
  `esp_wifi_disconnect()` is asynchronous, so `esp_wifi_set_config()` could
  return `ESP_ERR_WIFI_STATE`. In addition, while its state is ≥ CRED_RECV
  (including STOPPING), network_provisioning reconnects on unexpected
  disconnect reasons when `wifi_conn_attempts == 0` — including
  `WIFI_REASON_ASSOC_LEAVE` from our own disconnect — which matters if the
  `NETWORK_PROV_END` wait timed out. The wipe now retries for up to 1 s;
  if it still fails it is deferred and retried before the next connection
  attempt (normally seconds later, and the only early safe point on a
  device that then resumes normal operation for weeks), at the next
  session end and at start-up.
- `KNOWN_AP_UNREACHABLE` could be reported as `NO_KNOWN_AP_IN_TW`: after a
  re-armed window, attempts continuing from the existing connect list
  without a fresh scan did not count as "known AP seen". Attempts now count.
- When the window expired in the same wakeup as an attempt's failure, the
  failure was recorded in the new window. The failure is now recorded
  first, so the event reports it and the new window starts clean.

---

## [0.3.0] — Provisioning safety

### Fixed
- **A provisioned device could fall into BLE provisioning permanently.** A
  router that took longer than the tw window to come back, with any
  neighbour's AP in range, sent the device to PROVISIONING, where it
  retried forever and never scanned again — until power-cycled, and
  advertising to anyone in BLE range meanwhile. tw expiry is now governed
  by `auto_provision` (new Kconfig choice `CONFIG_WIFI_AUTO_PROVISION`,
  runtime field `wifi_mgr_config_t.auto_provision`). The default,
  `WHEN_UNPROVISIONED`, provisions only when no credentials are stored,
  checked at expiry time.
- **Unverified credentials were stored with top priority.** The CRED_RECV
  handler called `ap_records_add()` (use_count = max + 1, evicting the
  lowest entry when full) before the connection was tried. Credentials
  are now held in RAM and saved only after CRED_SUCCESS; the held
  password is wiped on success, failure, timeout and shutdown.
- **The driver's own NVS kept provisioned passwords.** network_provisioning
  leaves the driver in `WIFI_STORAGE_FLASH` after every
  `start_provisioning()` and writes received (unverified) credentials to
  `nvs.net80211`. As a result later reconnects wrote to flash whenever
  the config changed, and `wifi_mgr_reset_provisioning()` left the last
  password behind. RAM storage is now re-applied after every
  `start_provisioning()` and at the end of every session; the driver
  config is overwritten after a failed session and at every
  `wifi_mgr_start()` (before `esp_wifi_start()`, only if something is
  stored), which also cleans units provisioned with 0.1.x / 0.2.x.
- A stale tw expiry (timer fired just as the last live AP disappeared)
  could start provisioning with no APs in range, contrary to R7. It is
  now ignored outside TW_ACTIVE.
- If `esp_wifi_set_config()` failed, `wifi_mgr_connect_attempt()` never
  armed `connect_timer`, leaving the manager waiting forever. The timer is
  now always armed.
- In TW_ACTIVE, an unrelated notification during a connect attempt
  started a second attempt on top of the first. The task now keeps
  waiting for the attempt's outcome.
- `conn_info.channel` was 0 in the CONNECTED event after provisioning.

### Changed
- When tw expires and provisioning is not allowed, the component posts an
  event, re-arms the window and keeps scanning/connecting. It does not
  disconnect, does not abort a scan, and a connection completing at the
  same moment wins.
- `WIFI_MGR_EVENT_NO_KNOWN_AP_IN_TW` now means "no stored AP was visible
  during the window" and no longer implies provisioning is starting —
  watch `WIFI_MGR_EVENT_PROVISIONING_STARTED` for that.
- The stored BSSID is used as a first-attempt lock only when that BSSID
  appears in the current scan; otherwise the first attempt is SSID-only
  (avoids a wasted attempt in mesh / multi-AP homes). The BSSID of the
  AP joined during provisioning is now stored.
- `wifi_mgr_start()` order: init → STA mode → wipe stored driver config →
  RAM storage → start.

### Added
- `WIFI_MGR_EVENT_KNOWN_AP_UNREACHABLE` (id 11, appended) with
  `wifi_mgr_known_ap_unreachable_t {ssid, last_reason, attempts}`: a stored
  AP was visible during the window but every attempt failed. Never
  triggers provisioning by itself — a look-alike AP produces the same
  failures.
- `wifi_mgr_auto_prov_t` and `wifi_mgr_config_t.auto_provision` (appended
  to the struct; zero = Kconfig default).

### Migration
- Recompile: `wifi_mgr_config_t` grew a field. Designated initialisers
  and `NULL` config are unaffected.
- A provisioned device no longer re-provisions automatically after a
  router password change. Provide a manual trigger that calls
  `wifi_mgr_start_ble_provisioning()`, or set `auto_provision` to
  `ALWAYS` to keep the old behaviour (not recommended).
- Not yet changed (planned): `FREE_ON_SUCCESS` reboot path, time-limited
  manual sessions, `ps_mode`, runtime PoP, design-doc corrections.

---

## [0.2.0] — ESP-IDF v6.0 port

### Changed (breaking)
- Requires ESP-IDF v6.0 or later (`idf: ">=6.0.0"`). Stay on 0.1.x for v5.x.
- Provisioning now uses `espressif/network_provisioning` (declared in
  `idf_component.yml`) instead of the built-in `wifi_provisioning`
  component, which ESP-IDF v6.0 removed. Internal API is a 1:1 rename
  (`wifi_prov_*` → `network_prov_*`, `WIFI_PROV_CRED_*` →
  `NETWORK_PROV_WIFI_CRED_*`, `WIFI_PROV_END` → `NETWORK_PROV_END`).
  The public `wifi_manager.h` API is unchanged.
- `network_provisioning` and `bt` moved from `REQUIRES` to `PRIV_REQUIRES`;
  no public header exposes their types.

### Fixed
- ESP-IDF v6.0 disables protocomm security versions 0 and 1 by default,
  which made provisioning fail at runtime with `ESP_ERR_NOT_SUPPORTED` and
  no log line. Each `CONFIG_WIFI_PROV_AUTH_MODE_*` option now selects the
  matching `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_x`, and
  `wifi_mgr_prov.c` has an `#error` guard as a backstop.
- Provisioning console banner said "SRP6a" for Security 1. Security 1 is
  X25519 + AES-CTR (SRP6a is Security 2); label corrected.

### Removed
- IDF v4.x `esp_wifi_set_auto_connect()` compatibility branch.

---

## [0.1.0] — Initial release

1st Release

