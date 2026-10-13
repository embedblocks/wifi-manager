# Changelog

All notable changes to espnow_mqtt will be documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

