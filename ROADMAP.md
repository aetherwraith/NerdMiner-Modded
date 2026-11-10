# NerdMiner-Modded Roadmap

This document tracks major architectural enhancements, optimizations, and feature milestones for **NerdMiner-Modded**.

---

## 🎯 Completed Roadmap Features

### 1. Over-the-Air (OTA) Updates (`v1.9.0` + `v1.11.0`)
- **Web-Based Manual OTA Upload (`/update`)**: Drag-and-drop gzipped `PROGMEM` upload page with pre-upload binary header, chip target, and partition size validation.
- **Automated GitHub Releases OTA**: One-click release check and streaming HTTPS upgrade with dual-partition rollback safety (`esp_ota_mark_app_valid_cancel_rollback`).
- **Safe Hardware SHA Serialization**: `pauseHwMinerForTls()` + `g_hwShaMutex` lock around TLS handshakes so active hardware SHA mining never collides with mbedTLS SHA-384/512 operations, plus `saveStat()` before every planned restart.
- **Fleet Rolling OTA**: Browser-based multi-miner OTA push on `/` (Fleet tab) and CLI `tools/flash_provision_fleet.py --ota`.

### 2. Configuration via REST API (`/api/config`, `/api/system/config`) (`v1.7.0` + `v1.11.0`)
- **Unified Config Schema**: Single `nvMemory::fillConfigJson()` & `nvMemory::parseJsonConfig()` shared across SPIFFS persistence, REST API, HTML form save, and USB Serial CLI.
- **Zero-Reboot Live Apply**: Live handoff of pool/backup-pool credentials, POSIX DST timezone, fiat currency, backlight/night-dimming, RGB LED modes, and MQTT broker settings.
- **LAN-Safe Security**: Blocks external drive-by CSRF origins while keeping zero-touch LAN compatibility for Wotcha and Fleet Broadcast (with HTTP Basic Auth enforced when a custom `WebPassword` is configured).

### 3. Out-of-Box Provisioning & Discovery (`v1.8.0` + `v1.11.0`)
- **USB Serial CLI (`115200` baud)**: Full interactive provisioning (`config json`, `set`, `status`, `identify`, `ota`) running during initial captive-portal boot and handing off to `loop()` during mining to save `10.2 KB` of DRAM.
- **Zero-Touch mDNS**: Advertises `<hostname>.local` (`http://nm1.local/`) and `_nerdminer._tcp` service discovery TXT records on WiFi connect.

### 4. Fleet Management, Rich Telemetry & Deep Memory/Speed Optimization (`v1.11.0`)
- **Built-In Fleet Dashboard & Broadcast (`/` Fleet Tab)**: Client-side LAN peer auto-discovery (`/24` `/probe` scan), combined fleet hashrate/shares/crown best-diff summary, sortable live table, 1-click settings broadcast (including distinct random LED colors), and rolling fleet OTA.
- **Physical Miner Locator (`POST /api/identify`)**: 10-second cyan/white RGB LED strobe + high-contrast TFT hostname banner (restoring dark screen state afterward).
- **Extended Mining Telemetry**: True `session_best_difficulty` vs all-time NVS `best_difficulty`, Stratum share RTT (`pool.ping_ms`), mining `luck_pct`, and a top-8 `notable_shares` session feed.
- **Two-Way MQTT & LAN Share Celebration**: Home Assistant `<topic>/cmd` control (`identify`, `screen`, `page`, `ledMode`, `reboot`) plus UDP port `33339` LAN celebration pulses across all fleet units when a 32-bit share, record difficulty, or block is found.
- **Flash, IRAM & Latency Wins**:
  - Selective per-file `-O3 -funroll-loops` on hot mining code + `-Os` on cold Web/JSON/UI libraries (~`100+ KB` Flash saved).
  - Moved cold `nerd_sha256d_baked` out of IRAM on `NERD_SW_ASM` targets (`20.7 KB` IRAM saved) and gzipped static web pages.
  - Lock-free/double-buffered `ActiveJob` dispatch, immediate multi-share submission, persistent Stratum TCP line framing, screen-off TFT/HTTPS skip across all backlight-controlled boards, and `mempool.space` TLS connection reuse + streamed JSON filter.

---

## 📅 Version Milestones

| Version | Status | Key Deliverables |
| :--- | :--- | :--- |
| **v1.5.0** | Released | POSIX DST timezones, unauthenticated `/status` API, accepted/rejected share metrics, Wotcha integration |
| **v1.6.0** | Released | Upstream v1.4.0 sync: live settings without reboot, night dimming, idle screen timeout, chunked web streaming |
| **v1.7.0** | Released | REST Configuration API (`/api/system/config`, `/probe`, `/api/system`) with JSON GET/POST/PATCH and Wotcha support |
| **v1.8.0** | Released | USB Serial CLI (`115200` baud), `config.local.h` overrides, first-boot credential provisioning & webflasher config |
| **v1.9.0** | Released | Web OTA (`/update`), GitHub Releases automated OTA with dual-partition safety, and REST update API |
| **v1.10.x** | Released | Upstream v1.6.2 sync: Pulse CYD/T-Display/M5Stick UI, live `/` web monitor, MQTT HA discovery, Xtensa ASM SW miner, ckpool stats |
| **v1.11.0** | Released | Fleet Dashboard & Broadcast, `POST /api/identify`, mDNS, two-way MQTT & UDP share celebration, `session_best_diff` / `ping_ms` / `luck_pct` / notable shares, OTA SHA lock fix, and >120 KB Flash + >20 KB IRAM + >15 KB DRAM reclaimed |
