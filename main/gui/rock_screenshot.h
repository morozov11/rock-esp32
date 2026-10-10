#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Takes an LVGL snapshot of the active screen and dumps it over serial
 * (stdout) in base64 format wrapped in ===SCREENSHOT_START=== and
 * ===SCREENSHOT_END=== markers (RLE-compressed RGB565). Consumed by the
 * serial console (rock_serial.c) and tools/capture_screen.py.
 */
int rock_gui_dump_screenshot(void);

#ifdef __cplusplus
}
#endif
