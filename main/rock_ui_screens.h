#pragma once

#include <stdbool.h>
#include <stdint.h>

// Owner of the rock_wifi_scan_item_t Wi-Fi scan domain type.
#include "control_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show the RockCast splash screen / boot logo on the LVGL display.
 * Must be called after rock_display_init(); takes the LVGL port lock internally.
 */
int rock_ui_splash_show(void);

/**
 * Update the status text at the bottom of the RockCast splash screen.
 */
bool rock_ui_splash_status(const char *status_text);

/**
 * Dismiss the splash screen state and invalidate splash pointers.
 */
void rock_ui_splash_dismiss(void);

/** Render one bounded protocol-v1 presentation on the local LVGL display. */
bool rock_ui_show_text(const char *text);
bool rock_ui_show_now_playing(const char *station_id, const char *title,
                              const char *subtitle);
bool rock_ui_show_sensor_grid(const char *title, const char *items);

/** Render the DC-017 short code, verification phrase and pure-Rust QR matrix. */
bool rock_ui_pairing_show(const char *short_code, const char *phrase,
                          const uint8_t *modules, uint16_t width);

/** Render scanned Wi-Fi networks on the ST7701 display. */
bool rock_ui_wifi_scan_show(const rock_wifi_scan_item_t *items, uint16_t count);

/** Render Wi-Fi onboarding instructions, PIN, and QR code. */
bool rock_ui_onboarding_show(const char *ap_ssid, const char *pin, const char *url,
                             const uint8_t *modules, uint16_t width);

/** Update status line on the onboarding screen. */
bool rock_ui_onboarding_status(const char *status_text);

/** Render OTA download progress on the ST7701 display. */
bool rock_ui_ota_progress_show(const char *version, int percent);

#ifdef __cplusplus
}
#endif
