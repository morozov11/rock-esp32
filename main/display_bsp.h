#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the JC4880P443C_I_W display: MIPI-DSI PHY power, DSI bus, ST7701
 * panel and the esp_lvgl_port task. Must be called once before
 * rock_ui_clock_init(). Values follow the working vendor LVGL v9 demo
 * (native 480x800 portrait panel, LVGL rotates to 800x480).
 */
int rock_display_init(void);

/**
 * Create the clock UI inside the LVGL port: dark RockCast-like background,
 * a large uptime label and a small caption. Must be called after
 * rock_display_init(); the LVGL port lock is taken internally.
 */
int rock_ui_clock_init(void);

/**
 * Replace the clock label text. Takes the LVGL port lock internally with a
 * short timeout; returns false when the lock was not acquired in time, in
 * which case the caller may simply retry on its next tick.
 */
bool rock_ui_clock_set_text(const char *text);

#ifdef __cplusplus
}
#endif
