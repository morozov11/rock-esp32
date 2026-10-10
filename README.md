# rock-esp32

Firmware workspace for [RockServer](https://github.com/morozov11/rockserver) ([rockplatform.win](https://rockplatform.win)) devices based on the JC4880P443C_I_W board (ESP32-P4 with an ESP32-C6 Wi-Fi companion). The initial milestone is deliberately small: a Rust application hosted by ESP-IDF that prints a heartbeat once per second.

## Ecosystem

rock-esp32 is the embedded hardware component in the RockCast internet radio suite:

- **[rock-esp32](https://github.com/morozov11/rock-esp32)** *(this repository)* — Standalone hardware radio player and display firmware based on ESP32-P4.
- **[RockCast](https://github.com/morozov11/rockcast)** — Desktop internet radio player for Windows & Linux with Google Cast (CASTV2).
- **[RockMobile](https://github.com/morozov11/rockmobile)** — Android companion app and radio player.
- **[RockServer](https://github.com/morozov11/rockserver)** — Rust backend for radio discovery, AI semantic search, and device control ([rockplatform.win](https://rockplatform.win)).
- **[RockCast Station Catalog](https://github.com/morozov11/rockcast-station-catalog)** — Curated baseline station catalog, JSON schema, and offline validation tools.

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
core** are complete (2026-09-09).  On the target board this is verified for the
ESP-Hosted 3.0.7 C6 companion over SDIO, Wi-Fi scanning, and generic
manifest/state/command dispatch; pairing identity, device-session renewal, and
the bounded WSS transport are implemented and covered by host contract tests,
while a live paired session on the board is still pending (task RE-11 in
[docs/plan-control.md](docs/plan-control.md)).  See
[docs/dc-017-progress.md](docs/dc-017-progress.md) for the verification record.

DC-018 now provides the LVGL v9 display surface and GT911 touch seam. The owner
decided on 2026-09-09 that this board will become a standalone RockCast-class
radio: local station playback, touch browse/search, Rockmobile control, and an
always-listening `ESP` wake word followed by the existing RockServer voice and
typed-command path. The contract-first delivery plan is maintained in
[docs/rockcast-device-plan.md](docs/rockcast-device-plan.md); execution is
coordinated from [docs/plan-control.md](docs/plan-control.md), which also
tracks the cross-repository tasks for RockServer, Rockmobile, and RockCast.

DC-019 and DC-020 remain deferred because sensor hardware is not currently
available; they do not block the radio, GUI, playback, or radio-only voice
milestones.

The DC-018 implementation record is maintained in
[docs/dc-018-progress.md](docs/dc-018-progress.md).  Physical golden-view and
touch verification remains a separate board-session record; no flash operation
is implied by a firmware build.

Display bring-up (2026-09-08, completed ahead of the original checkpoint order): the 4.3-inch
ST7701 panel runs through LVGL 9.5.0 + `esp_lvgl_port` 2.9.0. See
[docs/bring-up.md](docs/bring-up.md) for the verified pin/timing map and the
ESP-IDF 6.x API migrations the vendor ST7701 component needed.

## Firmware layout and refactoring status

After the phase-3 layering refactor (2026-10-10, see
[docs/refactoring-audit.md](docs/refactoring-audit.md) for the full record and
phase status), the C modules under `main/` follow a strict dependency
direction: lower layers never call the UI directly — everything upward goes
through callbacks registered in `main/main.c` (the composition root):

- `display_bsp.c` — display hardware only (DSI/ST7701 panel, LVGL port task,
  GT911 touch, backlight); no screens, no GUI knowledge.
- `rock_ui_screens.c` — non-GUI LVGL screens: splash, legacy bounded
  presentations, DC-017 pairing, Wi-Fi scan list, onboarding, OTA progress.
- `rock_gui.c` — the native station GUI (views, touch, events); exposes narrow
  hooks (`rock_gui_trigger`, `rock_gui_log_debug_state`) for diagnostics and
  does not include `player_bsp.h`.
- `rock_serial.c` — the CMD: serial diagnostics console over USB-JTAG (the
  command set is a contract of `tools/*.py`); `rock_screenshot.c` — the
  RLE/base64 screenshot transport.
- `player_bsp.c`, `control_platform.c`, `rock_ota.c` — audio, Wi-Fi/WSS/OTA
  platform and update workers; report state upward via
  `rock_player_set_ui_cbs` / `rock_platform_set_ui_cbs` /
  `rock_ota_set_progress_cb` instead of calling the UI.
- `rock_ffi_exports.h` — the C-visible surface of the pure-Rust exports
  (`rock_validate_stream_uri`, `rock_qr_encode`).

WSS frame kinds are logged from the Rust session loop (parsed envelope kind
only — frame content is never logged, per the phase-1 confidentiality
contract); the C transport logs only lengths and results.

Refactoring phases 1–3 (critical bugs, dead code, layer inversion) are
complete and verified on the board; phase 4 (splitting the remaining god
objects) is planned in the audit document.

See [docs/bring-up.md](docs/bring-up.md) for hardware notes and [docs/rust-direction.md](docs/rust-direction.md) for the language strategy.

## Codex project tooling

The repository includes a project skill at `.agents/skills/rock-esp32-bringup` and project-scoped Espressif MCP servers in `.codex/config.toml`. Trust the project and refresh Codex after cloning so the MCP servers become available.
