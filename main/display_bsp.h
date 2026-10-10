#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the JC4880P443C_I_W display: MIPI-DSI PHY power, DSI bus, ST7701
 * panel, LVGL port task, GT911 touch and backlight. Must be called once
 * before any rock_ui_* call (screens live in rock_ui_screens.h) or
 * rock_gui_start(). Values follow the working vendor LVGL v9 demo
 * (native 480x800 portrait panel, LVGL rotates to 800x480).
 */
int rock_display_init(void);

#ifdef __cplusplus
}
#endif
