# rock-esp32

Firmware workspace for RockServer devices based on the JC4880P443C_I_W board (ESP32-P4 with an ESP32-C6 Wi-Fi companion). The initial milestone is deliberately small: an ESP-IDF/C application that prints a heartbeat once per second.

## Current target

- Board: 4.3-inch JC4880P443C_I_W/Y
- Main MCU: ESP32-P4, revision v1.3 observed during initial detection
- Flash: 16 MB
- Development port observed during initial detection: COM6 via USB Serial/JTAG
- ESP-IDF: 6.1

The project explicitly selects ESP32-P4 revisions below v3.0 and a minimum revision of v1.0. ESP32-P4 v1.x and v3.x require mutually exclusive ESP-IDF builds.

## Build commands

These commands were used for the first successful ESP32-P4 v1.3 bring-up:

```powershell
$env:PYTHONUTF8='1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py set-target esp32p4
idf.py build
idf.py -p COM6 flash monitor
```

Re-check the port before flashing; Windows can assign a different COM number after reconnecting the board. Exit the serial monitor with `Ctrl+]`.

The initial C firmware was built with ESP-IDF 6.1, flashed on COM6, and verified through USB Serial/JTAG. The monitor prints `Hello from Rock ESP32-P4` once per second.

## Roadmap

1. Build and flash the C Hello World; verify serial output and reset behavior.
2. Validate the ESP32-C6 slave image and bring up ESP-Hosted over SDIO.
3. Connect to a test access point and make one HTTPS request.
4. Integrate the device with the RockServer DC-016 control-plane milestone.
5. Evaluate and migrate application code to Rust while retaining a known-good C hardware diagnostic.

See [docs/bring-up.md](docs/bring-up.md) for hardware notes and [docs/rust-direction.md](docs/rust-direction.md) for the language strategy.

## Codex project tooling

The repository includes a project skill at `.agents/skills/rock-esp32-bringup` and project-scoped Espressif MCP servers in `.codex/config.toml`. Trust the project and refresh Codex after cloning so the MCP servers become available.
