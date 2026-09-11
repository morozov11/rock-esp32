# OTA Updates and Wi-Fi Onboarding (RE-9)

This document describes the firmware Over-The-Air (OTA) update mechanism and the interactive Wi-Fi onboarding flow for the RockCast ESP32 radio (`JC4880P443C_I_W` with ESP32-P4 v1.3 and ESP32-C6 companion).

## 1. Architectural Overview

```
                      +---------------------------------------------+
                      |             rock-esp32 Firmware             |
                      +---------------------------------------------+
                                      |
                 +--------------------+--------------------+
                 |                                         |
       [OTA Update Subsystem]                  [Wi-Fi Onboarding Subsystem]
                 |                                         |
  +--------------+--------------+           +--------------+--------------+
  |                             |           |                             |
[esp_https_ota]         [Rollback &]     [SoftAP Mode]             [HTTP Server]
- HTTPS client          Validation       - SSID: RockCast-Setup    - Port 80
- Cert bundle / test CA - otadata        - IP: 192.168.4.1         - PIN verification
- Paced 4KB chunks      - PENDING_VERIFY - Captive DNS (UDP 53)    - GET /api/scan
- Core 0, Priority 3    - Mark valid /   - Wi-Fi Quick Connect QR  - POST /connect
                          rollback                                 - Auto reboot to STA
                                                                          |
                                                                   [NVS rock_wifi]
                                                                          |
                                                                   [LVGL ST7701 UI]
                                                                   - Landscape 800x480
                                                                   - RockCast Brand Logo
                                                                   - 6-digit PIN Card
                                                                   - Wi-Fi Quick Connect QR
```

---

## 2. Security Boundaries & Protection Guarantees

1. **NVS Partition Isolation**:
   - The device pairing identity (`device_id`, `secret`) is strictly confined to the `rock_auth` NVS namespace.
   - Wi-Fi credentials reside exclusively in the `rock_wifi` NVS namespace.
   - Onboarding routines only read, write, or erase keys within `rock_wifi`. The `rock_auth` partition is never erased or modified by Wi-Fi operations.
2. **Setup PIN Anti-Hijacking Protection**:
   - To prevent unauthorized Wi-Fi configuration over the open SoftAP, each onboarding session generates a cryptographically random 6-digit PIN (`esp_random() % 1000000`).
   - The PIN is prominently rendered on the 800x480 LVGL display in a highlighted card (`SETUP PIN: XXX XXX`).
   - The web form requires entering this PIN (`inputmode="numeric"` for mobile numpad). Submissions to `POST /connect` without the exact matching PIN are rejected with HTTP 403 Forbidden.
3. **Wi-Fi Quick Connect QR Format**:
   - The QR code on the ST7701 display encodes standard Wi-Fi configuration (`WIFI:S:RockCast-Setup;T:nopass;;`).
   - Pointing any iOS or Android camera at the screen displays a native 1-tap "Join Network" prompt without searching for the network manually.
4. **Interactive Network Scanner (`GET /api/scan`)**:
   - The captive portal page queries `/api/scan` to automatically list nearby Wi-Fi networks in a `<select>` dropdown with RSSI signal bars and encryption status (🔒/🔓). Users only need to pick their network and enter the password.
5. **No Insecure TLS**:
   - OTA updates strictly enforce TLS certificate verification.
   - Production builds use the ESP certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`).
   - For isolated lab testing, `CONFIG_ROCK_OTA_TEST_CERT_PEM` allows specifying an explicit test CA certificate. Disabling TLS verification is prohibited.
6. **Credential Privacy**:
   - Passwords, Wi-Fi keys, stream URIs, and authentication tokens are never output to serial logs or error messages.

---

## 3. Storage and Credential Hierarchy

At boot time, `rock_platform_init()` determines Wi-Fi configuration in the following strict order:

1. **`CONFIG_ROCK_WIFI_SSID` in `sdkconfig` (Development Priority)**:
   If populated, the device connects directly using compile-time credentials. Onboarding is bypassed.
2. **`rock_wifi` in NVS**:
   If `CONFIG_ROCK_WIFI_SSID` is empty, the device attempts to load credentials from NVS. If valid credentials exist, it connects to the configured station.
3. **Interactive SoftAP Onboarding**:
   If neither source contains valid credentials (or if NVS credentials permanently fail to authenticate), the device starts SoftAP mode (`RockCast-Setup`) and presents the onboarding screen.

---

## 4. OTA Firmware Updates

### Partition Layout (RE-2 Migration)

| Partition | Offset | Size | Purpose |
| --- | ---: | ---: | --- |
| `nvs` | `0x9000` | `0x6000` (24 KiB) | Pinned pairing identity (`rock_auth`) & Wi-Fi (`rock_wifi`) |
| `phy_init` | `0xf000` | `0x1000` (4 KiB) | RF calibration |
| `otadata` | `0x10000` | `0x2000` (8 KiB) | Active partition selection & rollback tracking |
| `ota_0` | `0x20000` | `0x7F0000` (8128 KiB) | Application slot A |
| `ota_1` | `0x810000` | `0x7F0000` (8128 KiB) | Application slot B |

### Update Flow

1. **Trigger**:
   - `CONFIG_ROCK_OTA_CHECK_ON_BOOT=y`: Runs check shortly after Wi-Fi and SNTP readiness.
   - `CONFIG_ROCK_OTA_CHECK_INTERVAL_SEC`: Optional periodic timer check.
   - Future command extension: Command-triggered OTA is reserved for future contract versions; v1 fixtures remain frozen.
2. **Image Header Inspection**:
   - `esp_https_ota_get_img_desc()` inspects `esp_app_desc_t` (version, project name, compile time) before downloading flash payload.
   - If incoming version matches running version (and `force` is false), the update is cleanly skipped to prevent unnecessary flash wear.
3. **Paced Download**:
   - Task runs on **Core 0** with priority **3**.
   - Chunks are fetched and written in 4 KiB increments, followed by `vTaskDelay(pdMS_TO_TICKS(15))`.
   - This ensures **Core 1** audio playback (`esp_audio_codec` / I2S streaming at priorities 5 & 6) is never starved of CPU cycles or network bandwidth.
4. **Validation and Rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`)**:
   - Upon reboot into a new slot, the bootloader flags the partition as `ESP_OTA_IMG_PENDING_VERIFY`.
   - If the new image connects to Wi-Fi and initializes successfully, it calls `rock_ota_mark_valid()`, confirming the image in `otadata`.
   - If boot fails, panics, triggers a watchdog timeout, or fails critical network initialization within 30 seconds, the firmware (or bootloader) calls `rock_ota_mark_invalid_and_rollback()`, rolling back immediately to the previous partition.

---

## 5. Rollback and Failure Scenarios (S1–S6)

| Scenario | Trigger / Condition | System Behavior & Action |
| --- | --- | --- |
| **S1: Corrupted or Non-Booting Binary** | Flash corruption, missing symbols, crash on boot. | Bootloader rollback triggers on reboot; `otadata` reverts to previous slot; active app restored. |
| **S2: Image Boots but Network Fails** | New version fails Wi-Fi connection or self-test. | Firmware calls `esp_ota_mark_app_invalid_rollback_and_reboot()`; reboot returns to good slot. |
| **S3: Download Interrupted** | Wi-Fi disconnect, socket timeout, server reset. | `esp_https_ota_perform()` reports error; `esp_https_ota_abort()` cleans up; `otadata` is unchanged. |
| **S4: Matching Firmware Version** | Incoming image version equals running version. | Header check detects match; aborts download without flash erase or reboot. |
| **S5: Invalid Wi-Fi Password in Onboarding** | User enters incorrect password in web portal. | STA connection fails; NVS `rock_wifi` is cleared; SoftAP onboarding restarts with error banner on screen. |
| **S6: Simultaneous Radio Playback & OTA** | Radio stream active on Core 1 during OTA check. | Core 0 worker with 15ms task delay yields CPU; 0 audio buffer underruns occur on Core 1. |

---

## 6. Configuration Reference (`main/Kconfig.projbuild`)

```kconfig
menu "Rock OTA Update"
    config ROCK_OTA_URL
        string "Firmware update URL"
        default ""

    config ROCK_OTA_CHECK_ON_BOOT
        bool "Check for OTA updates on boot"
        default y

    config ROCK_OTA_CHECK_INTERVAL_SEC
        int "Periodic OTA check interval in seconds"
        default 0

    config ROCK_OTA_TEST_CERT_PEM
        string "Custom test CA certificate (PEM format)"
        default ""
endmenu

menu "Rock Wi-Fi Onboarding"
    config ROCK_AP_SSID
        string "SoftAP SSID for onboarding"
        default "RockCast-Setup"

    config ROCK_AP_PASSWORD
        string "SoftAP Password (empty for open network)"
        default ""

    config ROCK_AP_CHANNEL
        int "SoftAP Channel"
        default 1

    config ROCK_AP_MAX_CONN
        int "SoftAP Max Connections"
        default 4
endmenu
```
