# Changelog

All notable changes to espnow_mqtt will be documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

