# rock-esp32

Firmware workspace for RockServer devices based on the JC4880P443C_I_W board (ESP32-P4 with an ESP32-C6 Wi-Fi companion). The initial milestone is deliberately small: a Rust application hosted by ESP-IDF that prints a heartbeat once per second.

## Current target

- Board: 4.3-inch JC4880P443C_I_W/Y
- Main MCU: ESP32-P4, revision v1.3 observed during initial detection
- Flash: 16 MB
- Development port observed during initial detection: COM6 via USB Serial/JTAG
- ESP-IDF: 6.1

The project explicitly selects ESP32-P4 revisions below v3.0 and a minimum revision of v1.0. ESP32-P4 v1.x and v3.x require mutually exclusive ESP-IDF builds.

## Build commands

Install the upstream Rust target once:

The Rust toolchain is pinned by `rust-toolchain.toml` (nightly with `rust-src`); rustup provisions it automatically on the first build. The `riscv32imafc-esp-espidf` target is built from source via `-Zbuild-std`, so no manual target installation is needed.

Then build, flash, and monitor through ESP-IDF:

```powershell
$env:PYTHONUTF8='1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py set-target esp32p4
idf.py --no-ccache build
idf.py --no-ccache -p COM6 flash
idf.py -p COM6 monitor
```

Re-check the port before flashing; Windows can assign a different COM number after reconnecting the board. Exit the serial monitor with `Ctrl+]`.

The Rust firmware uses the `esp-idf-sys` crate (vendored under `third_party/esp-idf-sys`; see its `PROVENANCE.md` for the pinned revision and local patches). It was built with ESP-IDF 6.1, flashed on COM6, and verified through USB Serial/JTAG on an ESP32-P4 v1.3. The monitor prints `Rust + esp-idf-sys; free heap: ...` once per second.

The application loop lives in `src/lib.rs`. `main/main.c` is the ESP-IDF entry point (`app_main`) and a small FFI bridge for logging and FreeRTOS delays; the Rust library exports `rust_main` instead of defining its own `app_main`.

## Roadmap

The Rust bootstrap, display pipeline, and **DC-017 — provisioning and transport
core** are complete and verified on the target board (2026-09-09).  This includes
the ESP-Hosted 3.0.7 C6 companion over SDIO, Wi-Fi scanning, persistent pairing
identity, device-session renewal, bounded WSS transport, reconnect/heartbeat,
and generic manifest/state/command dispatch.  See
[docs/dc-017-progress.md](docs/dc-017-progress.md) for the verification record.

The next executable milestone is **DC-018 — ESP32 display surface**:

1. Initialize and validate the GT911 touch controller on the existing LVGL v9
   display stack.
2. Register `display.main`, map the protocol presentations `text`, `now_playing`,
   and `sensor_grid` to native views, and report the displayed view state.
3. Make `display.show_view` handling bounded and idempotent; verify golden
   presentations and offline/reconnect transitions on the physical board.

DC-019 then adds sensor modules and telemetry, and DC-020 verifies the full
`show_sensors` → `sensor_grid` path through RockServer.  Interactive station
browsing and local audio output remain explicitly out of protocol-v1 scope
(DC-039 and DC-040, respectively) until a separate product decision.

The DC-018 implementation record is maintained in
[docs/dc-018-progress.md](docs/dc-018-progress.md).  Physical golden-view and
touch verification remains a separate board-session record; no flash operation
is implied by a firmware build.

Display bring-up (2026-09-08, completed ahead of the original checkpoint order): the 4.3-inch
ST7701 panel runs through LVGL 9.5.0 + `esp_lvgl_port` 2.9.0 with an uptime clock
(`HH:MM:SS`) rendered on the physical display; the Rust side drives the label through
the `main/display_bsp.c` facade. See [docs/bring-up.md](docs/bring-up.md) for the
verified pin/timing map and the ESP-IDF 6.x API migrations the vendor ST7701
component needed.

See [docs/bring-up.md](docs/bring-up.md) for hardware notes and [docs/rust-direction.md](docs/rust-direction.md) for the language strategy.

## Codex project tooling

The repository includes a project skill at `.agents/skills/rock-esp32-bringup` and project-scoped Espressif MCP servers in `.codex/config.toml`. Trust the project and refresh Codex after cloning so the MCP servers become available.
