# JC4880P443C_I_W bring-up

## Verified baseline

The connected board was queried without modifying flash:

- ESP32-P4 revision v1.3
- USB Serial/JTAG on COM6
- 16 MB flash
- 40 MHz crystal

ESP-IDF 6.1 defaults to ESP32-P4 v3.1 and newer. This board uses v1.3, so `sdkconfig.defaults` selects the mutually exclusive pre-v3 hardware family and minimum revision v1.0. Never bypass this compatibility check with esptool's force option.

The original C Hello World, its `no_std` Rust replacement, and the current `esp-idf-sys`-based Rust application were each successfully built, flashed, and monitored on the connected v1.3 board. The Rust application is a static library behind a small C/ESP-IDF bridge; see `docs/rust-direction.md` for the current build chain and the vendored `esp-idf-sys` copy. As of 2026-09-08 the monitor prints `Rust + esp-idf-sys; free heap: ...` once per second. The boot log confirmed the supported range v1.0 through v1.99 and ESP-IDF 6.1. A non-fatal warning reported a Boya flash chip using the generic driver; enable the dedicated Boya driver before flash performance or reliability testing.

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

### 1. Serial Hello World (complete)

1. Activate the Espressif Installation Manager PowerShell profile.
2. Re-detect the board and confirm that the target is ESP32-P4.
3. Build with `idf.py build`; it drives cargo for the Rust static library (nightly toolchain, `riscv32imafc-esp-espidf` target).
4. Flash only the P4 application.
5. Confirm `Rust + esp-idf-sys; free heap: ...` appears once per second in the monitor.

### 2. Wi-Fi and internet test

1. Confirm the ESP32-C6 responds as an ESP-Hosted slave before changing its firmware.
2. Add the official ESP-Hosted host component using the ESP Component Registry.
3. Configure the verified SDIO pins above.
4. Connect to a dedicated test access point using credentials kept outside Git.
5. Synchronize time if TLS validation requires it.
6. Perform one HTTPS GET to a stable test endpoint and log the status code and response length.

Keep display, touch, audio, RockServer authentication, and OTA outside this checkpoint so network failures remain easy to isolate.

### 3. Display and LVGL clock (complete, 2026-09-08)

Brought up ahead of the Wi-Fi checkpoint at the owner's request. The 4.3-inch panel
(ST7701 over MIPI-DSI) renders a dark-themed uptime clock `HH:MM:SS` driven from
Rust; this proves the display pipeline chosen by the RockServer DC-018 decision
(LVGL v9 + `esp_lvgl_port`) on the real board.

Verified configuration (from the working vendor LVGL v9 demo, adapted):

- MIPI-DSI: 2 data lanes, 750 Mbps lane rate; DSI PHY powered by LDO channel 3
  at 2.5 V (`esp_ldo_acquire_channel`).
- Panel: vendor-preset timings 480x800 portrait @ 28 MHz DPI clock, RGB565,
  one frame buffer; LVGL software-rotates to landscape 800x480
  (`rotation.swap_xy = true`).
- Pins: RST on GPIO5, backlight PWM on GPIO23 (LEDC, 5 kHz, 10-bit, ~70% duty).
- Components: `lvgl/lvgl` 9.5.0 and `espressif/esp_lvgl_port` 2.9.0 from the
  ESP Component Registry; the vendor `esp_lcd_st7701` 1.1.3 component is copied
  to `components/esp_lcd_st7701` because its DPI preset carries this board's
  specific timings.
- Rust drives the clock label through the `main/display_bsp.c` facade
  (`rock_ui_clock_set_text`), taking the LVGL port lock inside C. The loop uses
  `esp_timer_get_time` and a fixed buffer: pulling `std::time`/`CString` dragged
  `std::fs` into the link and failed on missing `realpath` under
  `-Zbuild-std` for this target.

ESP-IDF 6.x API migrations applied to the vendor ST7701 component (the vendor
tree targets 5.5.4):

- `esp_lcd_panel_dev_config_t.color_space` → `rgb_ele_order` (enum values
  unchanged).
- `esp_lcd_dpi_panel_config_t.pixel_format` → `in_color_format` +
  `out_color_format` of type `lcd_color_format_t` (`LCD_COLOR_FMT_RGB565`).
- `flags.use_dma2d` removed → explicit `esp_lcd_dpi_panel_enable_dma2d(panel)`
  after panel init.
- `esp_driver_pmu` does not exist in 6.x; the LDO API lives in `esp_hw_support`
  (always linked).
- `CONFIG_LV_MEM_CUSTOM` is an LVGL v8 symbol and is silently ignored by v9.

First flash showed `00` instead of `:` separators: the clock buffer was
zero-initialized and the separator positions were never written. The unit test
covering this never ran because the crate only builds for the firmware target;
`cargo test` on the host does not execute it. The formatter now starts from the
literal `00:00:00` template so separators cannot be dropped.

Touch (GT911 over I2C) is not initialized yet; it stays in the DC-018 display
surface scope together with the real presentation views.

## Recovery and power

- Prefer the USB2 connector for native USB Serial/JTAG.
- Use a stable 5 V supply with at least 600 mA available.
- Never run `erase-flash` during ordinary diagnosis.
- Record the exact command and offsets before any vendor-image recovery operation.
