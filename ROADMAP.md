# NerdMiner-Modded Roadmap

This document outlines upcoming architectural enhancements, features, and milestones for **NerdMiner-Modded**.

---

## 🎯 High-Priority Roadmap Features

### 1. Over-the-Air (OTA) Updates

Provide seamless firmware upgrades without needing physical USB connection or PlatformIO re-flashing.

- **Web-Based Manual OTA Upload (`/update`)**:
  - Implement a dedicated upload endpoint using ESP32 `Update.h` / multipart HTTP upload.
  - Web UI drag-and-drop interface for uploading board-specific `.bin` firmware files directly from browser.
  - Pre-upload validation: verify binary headers, magic bytes, target chip architecture (`ESP32` vs `ESP32-S3`), and size against flash partition limits.
- **Automated GitHub Releases OTA**:
  - Check GitHub API (`api.github.com/repos/aetherwraith/NerdMiner-Modded/releases/latest`) for new tagged versions.
  - One-click upgrade button in web UI to stream and flash the appropriate binary asset directly from GitHub.
  - Safe TLS streaming with `g_hwShaMutex` throttling so active mining does not conflict with mbedTLS SHA operations.
- **Remote API Trigger for Fleet Managers (Wotcha)**:
  - Add `POST /api/system/update` accepting `{ "url": "https://..." }` or `{ "version": "latest" }` to allow Wotcha to orchestrate zero-touch fleet updates.
- **Partition Table & Fail-Safe Boot**:
  - Transition partition scheme to dual OTA (`ota_0`, `ota_1`) with ESP32 rollback support (`esp_ota_mark_app_valid_cancel_rollback`) so corrupted updates fall back safely to previous working firmware.

---

### 2. Configuration via REST API (`/api/system/config`)

Extend the monitoring API to allow programmatic configuration management, eliminating the need to parse HTML forms or scrape web pages.

- **`GET /api/system/config`**:
  - Returns complete system configuration as clean JSON:
    ```json
    {
      "pool": "public-pool.io",
      "port": 21496,
      "wallet": "bc1q...",
      "poolPassword": "x",
      "currency": "usd",
      "timezone": "Europe/London",
      "brightness": 250,
      "autoBrightness": true,
      "screenTimeout": 5,
      "nightDim": true,
      "nightBrightness": 30,
      "nightFade": 60,
      "latitude": 51.5074,
      "longitude": -0.1278,
      "invertColors": false,
      "flipDisplay": false,
      "saveStats": true
    }
    ```
- **`POST` / `PATCH /api/system/config`**:
  - Accepts JSON payload with any subset of settings to update.
  - Validates values and leverages `nvMemory::parseJsonConfig()` for unified schema handling.
  - Persists changes to SPIFFS `/config.json`.
  - **Zero-Reboot Live Apply**: Immediately hands off pool credentials to the stratum task, updates timezone via POSIX `tzset()`, refreshes currency/price, and updates backlight controls. Returns `{ "success": true, "rebootRequired": false }`.
  - Only returns `rebootRequired: true` if display hardware re-init is necessary (invert / flip display).
- **Direct Wotcha Integration**:
  - Allows Wotcha dashboard to remotely switch pools (e.g., failover or solo block bounty hunting), change wallet addresses, and adjust screen settings directly from the unified monitoring panel.

---

### 3. Out-of-Box First-Flash Configuration (Zero SD Card Required)

Enable quick, user-friendly initial provisioning for new devices without requiring an SD card or hardcoded credentials.

- **Enhanced Captive AP Setup (WiFiManager 2.0)**:
  - If no valid WiFi credentials exist in NVS/SPIFFS on boot, launch `NerdMinerAP` captive portal.
  - Modern, mobile-responsive first-time wizard:
    - WiFi Network selection with auto-scan.
    - Stratum Pool URL, Port, and BTC Wallet Address.
    - Searchable Timezone selector (with automatic DST).
    - Display orientation (standard vs flipped USB-left).
  - Write directly to `/config.json` in SPIFFS and transition immediately to miner mode.
- **Serial CLI / USB Provisioning Interface**:
  - Provide a lightweight serial command interface over USB UART (115200 baud):
    - `config get`: Output current JSON settings.
    - `config set wifi <ssid> <password>`
    - `config set pool <host> <port>`
    - `config set wallet <btc_address>`
    - `config set tz <timezone_id>`
    - `config save` & `config reset`
  - Enables single-click setup via WebUSB / ESP Web Tools in Chrome or automated Python provisioning scripts during factory flash.
- **Custom Compile-Time Overrides (`config.local.h`)**:
  - Support an optional `config.local.h` (git-ignored) or PlatformIO build flags (`-D DEFAULT_SSID="..."`, etc.) to bake personal defaults directly into built binaries without modifying version-controlled files.

---

## 📅 Version Milestones

| Version | Status | Key Deliverables |
| :--- | :--- | :--- |
| **v1.5.0** | Released | POSIX DST timezones, unauthenticated `/status` API, accepted/rejected share metrics, Wotcha integration |
| **v1.6.0** | Current | Upstream v1.4.0 sync: live settings without reboot, night dimming, idle screen timeout, chunked web streaming |
| **v1.7.0** | Planned | Feature 2: REST Configuration API (`/api/system/config`) with JSON GET/POST and Wotcha support |
| **v1.8.0** | Planned | Feature 3: First-flash captive portal wizard & USB serial CLI provisioning |
| **v1.9.0** | Planned | Feature 1: Web OTA (`/update`) & GitHub Releases automated OTA with dual-partition safety |
