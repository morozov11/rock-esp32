# JC4880P443C_I_W bring-up

## Verified baseline

The connected board was queried without modifying flash:

- ESP32-P4 revision v1.3
- USB Serial/JTAG on COM6
- 16 MB flash
- 40 MHz crystal

ESP-IDF 6.1 defaults to ESP32-P4 v3.1 and newer. This board uses v1.3, so `sdkconfig.defaults` selects the mutually exclusive pre-v3 hardware family and minimum revision v1.0. Never bypass this compatibility check with esptool's force option.

The C Hello World was successfully built, flashed, and monitored on the connected v1.3 board. The boot log confirmed the supported range v1.0 through v1.99 and ESP-IDF 6.1. A non-fatal warning reported a Boya flash chip using the generic driver; enable the dedicated Boya driver before flash performance or reliability testing.

The vendor documentation and recovery files are stored locally at `D:\work\JC4880P443C_I_W`. Keep that directory outside this repository because it contains large vendor archives and binary images.

## Wi-Fi architecture

ESP32-P4 does not contain a Wi-Fi radio. This board uses an on-board ESP32-C6 as a network co-processor. The vendor sample configures ESP-Hosted over SDIO with this mapping:

| Signal | ESP32-P4 GPIO |
| --- | ---: |
| CLK | 18 |
| CMD | 19 |
| D0 | 14 |
| D1 | 15 |
| D2 | 16 |
| D3 | 17 |
| C6 reset | 54 |

The vendor recovery directory contains `JC-C6-slave_v2.3.2.bin` for the C6 and `P4-JC4880P443C_I_W_V2.2.bin` as a merged P4 recovery image. These are recovery assets, not normal application dependencies. Do not flash either image as part of the Hello World milestone.

## Bring-up checkpoints

### 1. Serial Hello World

1. Activate the Espressif Installation Manager PowerShell profile.
2. Re-detect the board and confirm that the target is ESP32-P4.
3. Set the project target and build.
4. Flash only the P4 application.
5. Confirm `Hello from Rock ESP32-P4` appears once per second in the monitor.

### 2. Wi-Fi and internet test

1. Confirm the ESP32-C6 responds as an ESP-Hosted slave before changing its firmware.
2. Add the official ESP-Hosted host component using the ESP Component Registry.
3. Configure the verified SDIO pins above.
4. Connect to a dedicated test access point using credentials kept outside Git.
5. Synchronize time if TLS validation requires it.
6. Perform one HTTPS GET to a stable test endpoint and log the status code and response length.

Keep display, touch, audio, RockServer authentication, and OTA outside this checkpoint so network failures remain easy to isolate.

## Recovery and power

- Prefer the USB2 connector for native USB Serial/JTAG.
- Use a stable 5 V supply with at least 600 mA available.
- Never run `erase-flash` during ordinary diagnosis.
- Record the exact command and offsets before any vendor-image recovery operation.
