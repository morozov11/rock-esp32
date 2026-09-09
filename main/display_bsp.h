#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/** Render one bounded protocol-v1 presentation on the local LVGL display. */
bool rock_ui_show_text(const char *text);
bool rock_ui_show_now_playing(const char *station_id, const char *title,
                              const char *subtitle);
bool rock_ui_show_sensor_grid(const char *title, const char *items);

/** Render the DC-017 short code, verification phrase and pure-Rust QR matrix. */
bool rock_ui_pairing_show(const char *short_code, const char *phrase,
                          const uint8_t *modules, uint16_t width);

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
} rock_wifi_scan_item_t;

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
