# DC-018 display surface

## Implemented

- The firmware registers the sole v1 surface, `display.main`, with the
  `display.presentation` capability.  It advertises only `text`,
  `now_playing`, and `sensor_grid`, with eight cards and 128 display-text
  characters as its hardware limits.
- Rust validates and bounds incoming display commands before forwarding plain
  strings to the C/LVGL facade.  It rejects a different surface or unknown
  view as `capability_not_supported`, reports the visible view state, emits
  accepted/succeeded lifecycle messages, and does not redraw or advance state
  for an identical presentation.
- `main/display_bsp.c` renders the three views through LVGL v9 and registers a
  GT911 on I2C1 (GPIO7 SDA, GPIO8 SCL) with `esp_lvgl_port`.  Touch is solely a
  local LVGL pointer input: no browse, search, station, or audio protocol was
  added.

## Verification record

- Host contract tests cover the display manifest and command parsing,
  including idempotent replay and unsupported surface/view rejection.
- The ESP-IDF build validates the GT911 component and the C/Rust FFI boundary.
- On 2026-09-09 the P4 image was flashed to USB Serial/JTAG `COM6` without
  erasing flash. The boot log confirmed ESP32-P4 v1.3, ST7701/LVGL startup,
  and GT911 identity `0x39,0x31,0x31`; the C6 remained on matching
  ESP-Hosted 3.0.7 and Wi-Fi scan found access points.
- Golden presentations, power-cycle coordinate checks, and reconnect/offline
  presentation transitions remain the next physical acceptance checks.
