---
name: rock-esp32-bringup
description: Bring up, build, flash, monitor, and troubleshoot the Rock ESP32 firmware on the JC4880P443C_I_W ESP32-P4 board. Use for ESP-IDF work, board detection, ESP-Hosted Wi-Fi through the companion ESP32-C6, or early DC-016 device firmware in this repository.
---

# Rock ESP32-P4 bring-up

Read `docs/bring-up.md` before changing hardware configuration or flashing firmware.

## Environment

- Activate ESP-IDF with `$env:PYTHONUTF8='1'; . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'`.
- Use target `esp32p4` and ESP-IDF 6.1 unless the user explicitly chooses the vendor 5.5.4 baseline.
- Re-detect the serial port before flashing. COM6 was observed during initial bring-up, but is not a permanent assumption.
- Never overlap builds, compilation-based tests, or dependency installations. Wait for each command to finish and inspect its result.

## Hardware safety

- Use the board's USB2 connector for native USB Serial/JTAG and provide stable 5 V power with at least 600 mA available.
- Do not erase flash, restore the vendor image, or flash the companion ESP32-C6 unless the user explicitly requests that exact operation.
- Preserve the vendor recovery binaries under `D:\work\JC4880P443C_I_W\8-Burn operation\Burn files`.
- The P4 has no native Wi-Fi. Network work must use the on-board ESP32-C6 through ESP-Hosted over SDIO.

## Workflow

1. Keep the first milestone limited to the C Hello World and serial output.
2. Build before flashing; do not combine first-time diagnosis into a single opaque command.
3. Flash only the P4 application during normal bring-up.
4. For Wi-Fi, verify the C6 slave firmware first, then add the official ESP-Hosted component and a single HTTPS request.
5. Keep credentials out of source control. Supply them through local configuration that is ignored by Git.
6. Treat Rust as the intended application direction, but keep the verified C bootstrap as a hardware diagnostic until the Rust/ESP-IDF/ESP-Hosted path is proven.
