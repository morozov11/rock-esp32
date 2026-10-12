// LVGL screens that are not part of the native station GUI: splash boot
// screen, the legacy bounded-presentation screens, Wi-Fi scan list, DC-017
// pairing, Wi-Fi onboarding and the OTA progress bar. Hardware bring-up
// (DSI/panel/touch/backlight) stays in display_bsp.c; this file only draws.
//
// The legacy screens delegate to rock_gui_present/rock_gui_is_active when the
// native station GUI owns the display — delegation inside the UI layer.

#include "rock_ui_screens.h"
#include "rock_gui.h"

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <string.h>

static lv_obj_t *s_splash_status_lbl = NULL;
static lv_obj_t *s_onboarding_status_lbl = NULL;
static lv_obj_t *s_ota_bar = NULL;
static lv_obj_t *s_ota_pct_lbl = NULL;
static void *s_qr_buffer;

// QR canvas geometry, shared by the pairing and onboarding screens (the
// render loop below used to be duplicated ~30 lines in each).
#define QR_CANVAS_SIZE 320
#define QR_QUIET_MODULES 4
#define QR_MAX_MODULES 177

static bool qr_canvas_render(lv_obj_t *parent, const uint8_t *modules, uint16_t width,
                             int align_offset_x)
{
    const int scale = QR_CANVAS_SIZE / (width + QR_QUIET_MODULES * 2);
    const int used = scale * (width + QR_QUIET_MODULES * 2);
    if (scale < 1) return false;
    if (!s_qr_buffer) {
        s_qr_buffer = heap_caps_calloc((size_t)QR_CANVAS_SIZE * QR_CANVAS_SIZE,
                                       sizeof(lv_color16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_qr_buffer) return false;
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, s_qr_buffer, QR_CANVAS_SIZE, QR_CANVAS_SIZE, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    int origin = (QR_CANVAS_SIZE - used) / 2 + QR_QUIET_MODULES * scale;
    for (uint16_t y = 0; y < width; ++y) for (uint16_t x = 0; x < width; ++x) {
        if (!modules[(size_t)y * width + x]) continue;
        for (int py = 0; py < scale; ++py) for (int px = 0; px < scale; ++px)
            lv_canvas_set_px(canvas, origin + x * scale + px, origin + y * scale + py, lv_color_black(), LV_OPA_COVER);
    }
    lv_obj_align(canvas, LV_ALIGN_RIGHT_MID, align_offset_x, 0);
    return true;
}

int rock_ui_splash_show(void)
{
    if (!lvgl_port_lock(1000)) {
        return -1;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_onboarding_status_lbl = NULL;
    s_ota_bar = NULL;
    s_ota_pct_lbl = NULL;

    // RockCast warm dark background
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1A1410), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Centered card container for Brand + Logo
    lv_obj_t *center_box = lv_obj_create(screen);
    lv_obj_remove_style_all(center_box);
    lv_obj_set_size(center_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(center_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(center_box, LV_ALIGN_CENTER, 0, -20);

    // Acoustic Waveform Bars container (centered row)
    lv_obj_t *wave_box = lv_obj_create(center_box);
    lv_obj_remove_style_all(wave_box);
    lv_obj_set_size(wave_box, LV_SIZE_CONTENT, 48);
    lv_obj_set_flex_flow(wave_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wave_box, 6, 0);
    lv_obj_set_style_pad_bottom(wave_box, 10, 0);

    static const int bar_heights[] = { 14, 24, 38, 48, 38, 24, 14 };
    for (size_t i = 0; i < sizeof(bar_heights) / sizeof(bar_heights[0]); ++i) {
        lv_obj_t *bar = lv_obj_create(wave_box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 6, bar_heights[i]);
        lv_obj_set_style_radius(bar, 3, 0);
        uint32_t bar_color = (i == 3) ? 0xE8DCC8 : ((i == 2 || i == 4) ? 0xD96B30 : 0xC45C26);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bar_color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }

    // Brand Name: "RockCast" with Montserrat 48
    lv_obj_t *brand_box = lv_obj_create(center_box);
    lv_obj_remove_style_all(brand_box);
    lv_obj_set_size(brand_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brand_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_rock = lv_label_create(brand_box);
    lv_label_set_text(lbl_rock, "ROCK");
    lv_obj_set_style_text_color(lbl_rock, lv_color_hex(0xE8DCC8), 0);
    lv_obj_set_style_text_font(lbl_rock, &lv_font_montserrat_48, 0);

    lv_obj_t *lbl_cast = lv_label_create(brand_box);
    lv_label_set_text(lbl_cast, "CAST");
    lv_obj_set_style_text_color(lbl_cast, lv_color_hex(0xC45C26), 0);
    lv_obj_set_style_text_font(lbl_cast, &lv_font_montserrat_48, 0);

    // Subtitle: "SMART STREAMING RADIO"
    lv_obj_t *subtitle = lv_label_create(center_box);
    lv_label_set_text(subtitle, "SMART STREAMING RADIO");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9A8B78), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(subtitle, 6, 0);

    // Status label at the bottom of the screen
    s_splash_status_lbl = lv_label_create(screen);
    lv_label_set_text(s_splash_status_lbl, "Starting system...");
    lv_obj_set_style_text_color(s_splash_status_lbl, lv_color_hex(0x7C8598), 0);
    lv_obj_set_style_text_font(s_splash_status_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_splash_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -28);

    lvgl_port_unlock();
    return 0;
}

bool rock_ui_splash_status(const char *status_text)
{
    if (!status_text || !lvgl_port_lock(500)) {
        return false;
    }
    if (s_splash_status_lbl) {
        lv_label_set_text(s_splash_status_lbl, status_text);
    }
    lvgl_port_unlock();
    return true;
}

void rock_ui_splash_dismiss(void)
{
    // Symmetric with rock_ui_splash_status: the label pointer is touched from
    // the network task under the LVGL lock, so clearing it needs the same lock.
    if (!lvgl_port_lock(500)) return;
    s_splash_status_lbl = NULL;
    lvgl_port_unlock();
}

static lv_obj_t *legacy_screen(void)
{
    static lv_obj_t *screen;
    if (!screen) screen = lv_obj_create(NULL);
    if (lv_screen_active() != screen) lv_screen_load(screen);
    return screen;
}

static bool ui_lock(void)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *screen = legacy_screen();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    return true;
}

static void ui_label(lv_obj_t *parent, const char *text, uint32_t color,
                     const lv_font_t *font, lv_align_t align, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, align, x, y);
}

bool rock_ui_show_text(const char *text)
{
    if (rock_gui_is_active()) { rock_gui_present(NULL, text, NULL); return true; }
    if (!text || !ui_lock()) return false;
    ui_label(lv_screen_active(), "ROCK", 0x7C8598, &lv_font_montserrat_16,
             LV_ALIGN_TOP_MID, 0, 24);
    ui_label(lv_screen_active(), text, 0xF5F7FA, &lv_font_montserrat_48,
             LV_ALIGN_CENTER, 0, 0);
    lvgl_port_unlock();
    return true;
}

bool rock_ui_show_now_playing(const char *station_id, const char *title,
                              const char *subtitle)
{
    if (rock_gui_is_active()) { rock_gui_present(station_id, title, subtitle); return true; }
    if (!station_id || !title || !ui_lock()) return false;
    lv_obj_t *screen = legacy_screen();
    ui_label(screen, "NOW PLAYING", 0x7C8598, &lv_font_montserrat_16,
             LV_ALIGN_TOP_LEFT, 42, 42);
    ui_label(screen, title, 0xF5F7FA, &lv_font_montserrat_48,
             LV_ALIGN_LEFT_MID, 42, -42);
    ui_label(screen, subtitle, 0x9CA3AF, &lv_font_montserrat_16,
             LV_ALIGN_LEFT_MID, 42, 24);
    ui_label(screen, station_id, 0x60A5FA, &lv_font_montserrat_16,
             LV_ALIGN_BOTTOM_LEFT, 42, -42);
    lvgl_port_unlock();
    return true;
}

bool rock_ui_show_sensor_grid(const char *title, const char *items)
{
    if (rock_gui_is_active()) { rock_gui_present(NULL, title, items); return true; }
    if (!title || !items || !ui_lock()) return false;
    lv_obj_t *screen = legacy_screen();
    ui_label(screen, title, 0xF5F7FA, &lv_font_montserrat_16,
             LV_ALIGN_TOP_LEFT, 30, 25);
    lv_obj_t *grid = lv_label_create(screen);
    lv_label_set_text(grid, items);
    lv_label_set_long_mode(grid, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(grid, 740);
    lv_obj_set_style_text_color(grid, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_text_font(grid, &lv_font_montserrat_16, 0);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 30, 75);
    lvgl_port_unlock();
    return true;
}

bool rock_ui_pairing_show(const char *short_code, const char *phrase,
                          const uint8_t *modules, uint16_t width)
{
    if (!short_code || !phrase || !modules || width == 0 || width > QR_MAX_MODULES) return false;
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *screen = legacy_screen();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_fmt(title, "PAIR  %s", short_code);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, -45);
    lv_obj_t *verification = lv_label_create(screen);
    lv_label_set_text(verification, phrase);
    lv_obj_set_style_text_color(verification, lv_color_hex(0x7C8598), 0);
    lv_obj_set_style_text_font(verification, &lv_font_montserrat_16, 0);
    lv_obj_align(verification, LV_ALIGN_LEFT_MID, 45, 25);
    if (!qr_canvas_render(screen, modules, width, -35)) {
        lvgl_port_unlock();
        return false;
    }
    lvgl_port_unlock();
    return true;
}

static const char *get_auth_mode_str(uint8_t authmode)
{
    switch (authmode) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA-PSK";
        case 3: return "WPA2-PSK";
        case 4: return "WPA/WPA2";
        case 5: return "WPA2-ENT";
        case 6: return "WPA3-PSK";
        case 7: return "WPA2/WPA3";
        case 8: return "WAPI";
        case 9: return "OWE";
        default: return "SECURED";
    }
}

bool rock_ui_wifi_scan_show(const rock_wifi_scan_item_t *items, uint16_t count)
{
    if (!lvgl_port_lock(1000)) return false;

    lv_obj_t *screen = legacy_screen();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AVAILABLE WI-FI NETWORKS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 20);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text_fmt(subtitle, "Found %u access point(s)", count);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x7C8598), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 30, 45);

    if (count == 0 || !items) {
        lv_obj_t *empty = lv_label_create(screen);
        lv_label_set_text(empty, "No Wi-Fi networks found");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x7C8598), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_center(empty);
        lvgl_port_unlock();
        return true;
    }

    lv_obj_t *cont = lv_obj_create(screen);
    lv_obj_set_size(cont, 740, 390);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x14171F), 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x262C38), 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_radius(cont, 8, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    uint16_t show_count = count > 30 ? 30 : count;
    for (uint16_t i = 0; i < show_count; i++) {
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1B202B), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // RSSI badge
        lv_obj_t *rssi_lbl = lv_label_create(row);
        lv_label_set_text_fmt(rssi_lbl, "%3d dBm", items[i].rssi);
        uint32_t rssi_color = items[i].rssi >= -60 ? 0x4ADE80 : (items[i].rssi >= -75 ? 0xFACC15 : 0xF87171);
        lv_obj_set_style_text_color(rssi_lbl, lv_color_hex(rssi_color), 0);
        lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(rssi_lbl, 80);

        // SSID
        lv_obj_t *ssid_lbl = lv_label_create(row);
        lv_label_set_text(ssid_lbl, items[i].ssid[0] ? items[i].ssid : "<Hidden Network>");
        lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(0xF5F7FA), 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_flex_grow(ssid_lbl, 1);

        // Channel
        lv_obj_t *ch_lbl = lv_label_create(row);
        lv_label_set_text_fmt(ch_lbl, "CH %u", items[i].channel);
        lv_obj_set_style_text_color(ch_lbl, lv_color_hex(0x7C8598), 0);
        lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(ch_lbl, 60);

        // Auth
        lv_obj_t *auth_lbl = lv_label_create(row);
        lv_label_set_text(auth_lbl, get_auth_mode_str(items[i].authmode));
        lv_obj_set_style_text_color(auth_lbl, lv_color_hex(items[i].authmode == 0 ? 0x94A3B8 : 0x60A5FA), 0);
        lv_obj_set_style_text_font(auth_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(auth_lbl, 110);
    }

    lvgl_port_unlock();
    return true;
}

bool rock_ui_onboarding_show(const char *ap_ssid, const char *pin, const char *url,
                             const uint8_t *modules, uint16_t width)
{
    if (!ap_ssid || !url || !modules || width == 0 || width > QR_MAX_MODULES) return false;
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *screen = legacy_screen();
    lv_obj_clean(screen);
    s_ota_bar = NULL;
    s_ota_pct_lbl = NULL;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1A1410), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "WI-FI SETUP");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8DCC8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 45, 25);

    lv_obj_t *step1 = lv_label_create(screen);
    lv_label_set_text_fmt(step1, "1. Scan QR code to connect phone\n   Wi-Fi: %s", ap_ssid);
    lv_obj_set_style_text_color(step1, lv_color_hex(0xC45C26), 0);
    lv_obj_set_style_text_font(step1, &lv_font_montserrat_16, 0);
    lv_obj_align(step1, LV_ALIGN_TOP_LEFT, 45, 95);

    lv_obj_t *step2 = lv_label_create(screen);
    lv_label_set_text_fmt(step2, "2. Select your home Wi-Fi in portal\n   or open: %s", url);
    lv_obj_set_style_text_color(step2, lv_color_hex(0x9A8B78), 0);
    lv_obj_set_style_text_font(step2, &lv_font_montserrat_16, 0);
    lv_obj_align(step2, LV_ALIGN_TOP_LEFT, 45, 155);

    if (pin && pin[0] != '\0') {
        lv_obj_t *pin_card = lv_obj_create(screen);
        lv_obj_set_size(pin_card, 350, 50);
        lv_obj_align(pin_card, LV_ALIGN_TOP_LEFT, 45, 220);
        lv_obj_set_style_bg_color(pin_card, lv_color_hex(0x241C16), 0);
        lv_obj_set_style_border_color(pin_card, lv_color_hex(0xC45C26), 0);
        lv_obj_set_style_border_width(pin_card, 2, 0);
        lv_obj_set_style_radius(pin_card, 8, 0);
        lv_obj_set_style_pad_all(pin_card, 6, 0);

        lv_obj_t *pin_lbl = lv_label_create(pin_card);
        lv_label_set_text_fmt(pin_lbl, "SETUP PIN:  %.3s %.3s", pin, pin + (strlen(pin) >= 3 ? 3 : 0));
        lv_obj_set_style_text_color(pin_lbl, lv_color_hex(0xE8DCC8), 0);
        lv_obj_set_style_text_font(pin_lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(pin_lbl);
    }

    s_onboarding_status_lbl = lv_label_create(screen);
    lv_label_set_text(s_onboarding_status_lbl, "Waiting for phone connection...");
    lv_obj_set_style_text_color(s_onboarding_status_lbl, lv_color_hex(0x9A8B78), 0);
    lv_obj_set_style_text_font(s_onboarding_status_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_onboarding_status_lbl, LV_ALIGN_TOP_LEFT, 45, 290);

    if (!qr_canvas_render(screen, modules, width, -45)) {
        lvgl_port_unlock();
        return false;
    }
    lvgl_port_unlock();
    return true;
}

bool rock_ui_onboarding_status(const char *status_text)
{
    if (!status_text || !lvgl_port_lock(1000)) return false;
    if (s_onboarding_status_lbl) {
        lv_label_set_text(s_onboarding_status_lbl, status_text);
    }
    lvgl_port_unlock();
    return true;
}

bool rock_ui_ota_progress_show(const char *version, int percent)
{
    if (!lvgl_port_lock(1000)) return false;
    lv_obj_t *screen = legacy_screen();

    if (!s_ota_bar) {
        lv_obj_clean(screen);
        s_onboarding_status_lbl = NULL;
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(screen);
        lv_label_set_text(title, "FIRMWARE UPDATE");
        lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -80);

        lv_obj_t *ver_lbl = lv_label_create(screen);
        lv_label_set_text_fmt(ver_lbl, "Downloading version: %s", version ? version : "candidate");
        lv_obj_set_style_text_color(ver_lbl, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(ver_lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(ver_lbl, LV_ALIGN_CENTER, 0, -20);

        s_ota_bar = lv_bar_create(screen);
        lv_obj_set_size(s_ota_bar, 500, 24);
        lv_obj_align(s_ota_bar, LV_ALIGN_CENTER, 0, 30);
        lv_bar_set_range(s_ota_bar, 0, 100);
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_ota_bar, 6, 0);

        s_ota_pct_lbl = lv_label_create(screen);
        lv_obj_set_style_text_color(s_ota_pct_lbl, lv_color_hex(0xF5F7FA), 0);
        lv_obj_set_style_text_font(s_ota_pct_lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(s_ota_pct_lbl, LV_ALIGN_CENTER, 0, 75);
    }

    if (s_ota_bar) {
        lv_bar_set_value(s_ota_bar, percent, LV_ANIM_OFF);
    }
    if (s_ota_pct_lbl) {
        if (percent >= 100) {
            lv_label_set_text(s_ota_pct_lbl, "100% - Verifying and Rebooting...");
        } else {
            lv_label_set_text_fmt(s_ota_pct_lbl, "%d%% completed", percent);
        }
    }
    if (percent >= 100) {
        s_ota_bar = NULL;
        s_ota_pct_lbl = NULL;
    }
    lvgl_port_unlock();
    return true;
}
