// GUI facade after the phase-4 split: owns the shell (header, viewport,
// ticker), the UI state and event queue, the view registry and every public
// rock_gui_* entry point. Views and shared widgets live in the sibling
// gui/rock_gui_widgets*.c and gui/rock_gui_views_*.c files; rock_gui.h (the
// Rust-bound ABI) is unchanged.

#include "rock_gui.h"
#include "rock_gui_internal.h"
#include "rockcast_tokens.h"
#include "rock_fonts.h"
#include "audio_spectrum.h"
#include "rock_ui_screens.h"
#include "rock_serial.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

static const char *TAG = "rock-gui";
static bool s_gui_active = false;

// FreeRTOS queue for UI events
static QueueHandle_t s_ui_event_queue = NULL;

// Active state
static rock_gui_view_mode_t s_active_view = VIEW_NOW_PLAYING;
static rock_gui_playback_t s_playback_status = ROCK_GUI_PLAYBACK_IDLE;
static char s_station_id[129] = "";
static uint8_t s_volume_level = 65;
static bool s_volume_muted = false;
static bool s_catalog_stale = false;
static char s_cast_receiver_name[129] = {0};

// Metrics
static uint32_t s_fps = 0;
static uint32_t s_frame_counter = 0;
static int64_t s_last_fps_time_us = 0;

// Shell widgets
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_header = NULL;
static lv_obj_t *s_lbl_logo = NULL;
static lv_obj_t *s_lbl_section = NULL;
static lv_obj_t *s_lbl_wifi = NULL;
static lv_obj_t *s_lbl_server = NULL;
static lv_obj_t *s_lbl_clock = NULL;

static lv_obj_t *s_viewport = NULL;

// Bottom Ticker
static lv_obj_t *s_ticker_bar = NULL;
static lv_obj_t *s_lbl_ticker = NULL;

// View registry (phase 4): adding a screen = one view file + one row here;
// switch_view walks this table instead of an enum+switch.
typedef struct {
    rock_gui_view_mode_t mode;
    void (*build)(lv_obj_t *parent);
    void (*show)(void);
    void (*hide)(void);
    const char *section_label;
} rock_gui_view_reg_t;

static const rock_gui_view_reg_t s_view_registry[] = {
    { VIEW_NOW_PLAYING, gui_now_playing_build, gui_now_playing_show, gui_now_playing_hide, "• Сейчас играет" },
    { VIEW_BROWSE, gui_browse_build, gui_browse_show, gui_browse_hide, "• Каталог станций" },
    { VIEW_SEARCH, gui_search_build, gui_search_show, gui_search_hide, "• Поиск станций" },
};

// ------------------------------------------------- state & shell accessors ---

void gui_enqueue_event(rock_ui_event_type_t type, const char *text, int16_t val)
{
    if (!s_ui_event_queue) return;
    rock_ui_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.value = val;
    if (text) {
        strncpy(evt.text, text, sizeof(evt.text) - 1);
    }
    xQueueSend(s_ui_event_queue, &evt, 0);
}

static const rock_gui_view_reg_t *find_view_reg(rock_gui_view_mode_t view)
{
    for (size_t i = 0; i < sizeof(s_view_registry) / sizeof(s_view_registry[0]); i++) {
        if (s_view_registry[i].mode == view) return &s_view_registry[i];
    }
    return NULL;
}

void gui_switch_view(rock_gui_view_mode_t view)
{
    s_active_view = view;
    gui_overlay_hide();
    gui_cast_hide();
    for (size_t i = 0; i < sizeof(s_view_registry) / sizeof(s_view_registry[0]); i++) {
        s_view_registry[i].hide();
    }

    const rock_gui_view_reg_t *reg = find_view_reg(view);
    if (reg) {
        reg->show();
        if (s_lbl_section) lv_label_set_text(s_lbl_section, reg->section_label);
    }
}

rock_gui_view_mode_t gui_active_view(void)
{
    return s_active_view;
}

bool gui_catalog_stale(void)
{
    return s_catalog_stale;
}

rock_gui_playback_t gui_playback_status(void)
{
    return s_playback_status;
}

void gui_set_station_id(const char *station_id)
{
    snprintf(s_station_id, sizeof(s_station_id), "%s", station_id);
}

const char *gui_station_id(void)
{
    return s_station_id;
}

lv_obj_t *gui_screen(void)
{
    return s_screen;
}

lv_obj_t *gui_ticker_label(void)
{
    return s_lbl_ticker;
}

// ---------------------------------------------------------------------------
// Visualizer and Timer Update
// ---------------------------------------------------------------------------

static void spectrum_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_frame_counter++;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_fps_time_us >= 1000000) {
        s_fps = s_frame_counter;
        s_frame_counter = 0;
        s_last_fps_time_us = now_us;

        // Update clock every second
        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        char clock_str[16];
        snprintf(clock_str, sizeof(clock_str), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
        if (s_lbl_clock) {
            lv_label_set_text(s_lbl_clock, clock_str);
        }

#if CONFIG_ROCK_UI_DEBUG_OVERLAY
        if (s_lbl_ticker) {
            uint32_t fft_us = rock_spectrum_get_cpu_us();
            size_t free_heap = esp_get_free_heap_size();
            lv_label_set_text_fmt(s_lbl_ticker, "FPS: %lu | Heap: %u KB | FFT: %lu us",
                                  (unsigned long)s_fps, (unsigned)(free_heap / 1024), (unsigned long)fft_us);
        }
#endif
    }

    // Read real FFT spectrum bands
    static float bands[SPECTRUM_BANDS_COUNT];
    rock_spectrum_get_bands(bands);
    gui_now_playing_spectrum_update(bands, s_playback_status == ROCK_GUI_PLAYBACK_PLAYING);
}

// ---------------------------------------------------------------------------
// Construction of the shell
// ---------------------------------------------------------------------------

static void make_header(lv_obj_t *parent)
{
    s_header = lv_obj_create(parent);
    lv_obj_remove_style_all(s_header);
    lv_obj_set_size(s_header, 800, ROCK_HEADER_HEIGHT);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_header, 16, 0);
    lv_obj_set_style_pad_right(s_header, 16, 0);
    lv_obj_set_flex_flow(s_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lbl_logo = lv_label_create(s_header);
    lv_label_set_text(s_lbl_logo, "ROCKCAST");
    lv_obj_set_style_text_color(s_lbl_logo, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_lbl_logo, &font_inter_18, 0);

    s_lbl_section = lv_label_create(s_header);
    lv_label_set_text(s_lbl_section, "• Сейчас играет");
    lv_obj_set_style_text_color(s_lbl_section, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_section, &font_inter_14, 0);
    lv_obj_set_style_pad_left(s_lbl_section, 8, 0);
    lv_obj_set_flex_grow(s_lbl_section, 1);

    s_lbl_wifi = lv_label_create(s_header);
    lv_label_set_text(s_lbl_wifi, ROCK_ICON_WIFI " Wi-Fi");
    lv_obj_set_style_text_color(s_lbl_wifi, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_wifi, &font_inter_14, 0);
    lv_obj_set_style_pad_right(s_lbl_wifi, 16, 0);

    s_lbl_server = lv_label_create(s_header);
    lv_label_set_text(s_lbl_server, "Server: OK");
    lv_obj_set_style_text_color(s_lbl_server, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_lbl_server, &font_inter_14, 0);
    lv_obj_set_style_pad_right(s_lbl_server, 16, 0);

    s_lbl_clock = lv_label_create(s_header);
    lv_label_set_text(s_lbl_clock, "12:00");
    lv_obj_set_style_text_color(s_lbl_clock, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_lbl_clock, &font_hack_16, 0);
}

static void make_ticker(lv_obj_t *parent)
{
    s_ticker_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ticker_bar);
    lv_obj_set_size(s_ticker_bar, 800, ROCK_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(s_ticker_bar, 0, 480 - ROCK_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(s_ticker_bar, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_ticker_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_ticker_bar, 16, 0);
    lv_obj_set_style_pad_right(s_ticker_bar, 16, 0);
    lv_obj_set_flex_flow(s_ticker_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ticker_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lbl_ticker = lv_label_create(s_ticker_bar);
    lv_label_set_text(s_lbl_ticker, "Готов к воспроизведению. Выберите станцию из каталога.");
    lv_label_set_long_mode(s_lbl_ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_ticker, 768);
    lv_obj_set_style_text_color(s_lbl_ticker, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_ticker, &font_inter_14, 0);
}

// ---------------------------------------------------------------------------
// Public C API Implementation
// ---------------------------------------------------------------------------

int rock_gui_start(void)
{
    if (s_gui_active) {
        if (!lvgl_port_lock(1000)) return -1;
        lv_screen_load(s_screen);
        lvgl_port_unlock();
        return 0;
    }
    ESP_LOGI(TAG, "Starting RockCast native station GUI...");

    if (!s_ui_event_queue) {
        s_ui_event_queue = xQueueCreate(16, sizeof(rock_ui_event_t));
        if (!s_ui_event_queue) {
            ESP_LOGE(TAG, "Failed to create UI event queue");
            return -1;
        }
    }

    // Initialize real FFT spectrum analyzer on Core 0
    if (rock_spectrum_init() != 0) {
        ESP_LOGE(TAG, "Audio spectrum init failed");
    }

    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL port lock");
        return -1;
    }

    rock_ui_splash_dismiss();
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);
    lv_obj_set_style_bg_color(s_screen, ROCK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    make_header(s_screen);
    make_ticker(s_screen);

    s_viewport = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_viewport);
    lv_obj_set_size(s_viewport, 800, 404);
    lv_obj_set_pos(s_viewport, 0, ROCK_HEADER_HEIGHT);

    for (size_t i = 0; i < sizeof(s_view_registry) / sizeof(s_view_registry[0]); i++) {
        s_view_registry[i].build(s_viewport);
    }
    gui_overlay_build(s_viewport);
    gui_cast_build(s_viewport);

    // Register 30 FPS update timer for FFT visualizer and ticker clock
    lv_timer_create(spectrum_timer_cb, 34, NULL);

    // Seed Browse screen with initial radio stations
    static const rock_gui_station_item_t s_default_stations[] = {
        { .id = "st-01", .name = "Radio Maximum", .tags = "Alternative, Rock", .codec = "MP3", .bitrate_kbps = 128, .is_playing = false },
        { .id = "st-02", .name = "Наше Радио", .tags = "Русский рок", .codec = "MP3", .bitrate_kbps = 128, .is_playing = false },
        { .id = "st-03", .name = "Rock FM 95.2", .tags = "Classic Rock, Hard Rock", .codec = "AAC", .bitrate_kbps = 128, .is_playing = false },
        { .id = "st-04", .name = "Radio Ultra", .tags = "Nu-Metal, Post-Grunge", .codec = "MP3", .bitrate_kbps = 192, .is_playing = false },
        { .id = "st-05", .name = "Radio Record", .tags = "Electronic, Club", .codec = "AAC", .bitrate_kbps = 128, .is_playing = false },
    };
    rock_gui_set_browse_stations(s_default_stations, sizeof(s_default_stations) / sizeof(s_default_stations[0]), NULL);

    s_gui_active = true;

    gui_switch_view(VIEW_NOW_PLAYING);

    // Serial diagnostics console (rock_serial.c); started here because its
    // CMD:UI:* actions target widgets that exist only from this point on.
    rock_serial_start();

    lvgl_port_unlock();
    ESP_LOGI(TAG, "RockCast native station GUI started successfully");
    return 0;
}

bool rock_gui_poll_event(rock_ui_event_t *out_event, uint32_t timeout_ms)
{
    if (!s_ui_event_queue || !out_event) return false;
    return xQueueReceive(s_ui_event_queue, out_event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void rock_gui_update_playback(rock_gui_playback_t status, const char *station_id,
                              const char *title, const char *subtitle,
                              uint8_t volume, bool muted)
{
    if (!lvgl_port_lock(500)) return;

    s_playback_status = status;
    s_volume_level = volume;
    s_volume_muted = muted;

    gui_now_playing_update_meta(station_id, title, subtitle);
    gui_now_playing_update_transport(status, volume, muted);

    if (s_lbl_ticker) {
        switch (status) {
            case ROCK_GUI_PLAYBACK_PLAYING:
                lv_label_set_text_fmt(s_lbl_ticker, "Воспроизведение: %s", gui_now_playing_station_text());
                break;
            case ROCK_GUI_PLAYBACK_BUFFERING:
                lv_label_set_text(s_lbl_ticker, "Буферизация потока станции...");
                break;
            case ROCK_GUI_PLAYBACK_PAUSED:
                lv_label_set_text(s_lbl_ticker, "Пауза");
                break;
            case ROCK_GUI_PLAYBACK_STOPPED:
                lv_label_set_text(s_lbl_ticker, "Воспроизведение остановлено");
                break;
            case ROCK_GUI_PLAYBACK_ERROR:
                lv_label_set_text(s_lbl_ticker, "Ошибка воспроизведения аудио");
                break;
            default:
                break;
        }
    }

    lvgl_port_unlock();
}

void rock_gui_update_connectivity(int rssi, bool wifi_connected, bool server_connected)
{
    static int last_rssi = INT_MIN;
    static bool last_wifi;
    static bool last_server;
    if (rssi == last_rssi && wifi_connected == last_wifi && server_connected == last_server) return;
    if (!lvgl_port_lock(500)) return;

    last_rssi = rssi;
    last_wifi = wifi_connected;
    last_server = server_connected;

    if (s_lbl_wifi) {
        if (wifi_connected) {
            lv_label_set_text_fmt(s_lbl_wifi, ROCK_ICON_WIFI " %d dBm", rssi);
            lv_obj_set_style_text_color(s_lbl_wifi, ROCK_COLOR_FG, 0);
        } else {
            lv_label_set_text(s_lbl_wifi, ROCK_ICON_WIFI " Нет связи");
            lv_obj_set_style_text_color(s_lbl_wifi, ROCK_COLOR_WARN, 0);
        }
    }

    if (s_lbl_server) {
        if (server_connected) {
            lv_label_set_text(s_lbl_server, "RockServer: OK");
            lv_obj_set_style_text_color(s_lbl_server, ROCK_COLOR_FG, 0);
        } else {
            lv_label_set_text(s_lbl_server, "RockServer: OFF");
            lv_obj_set_style_text_color(s_lbl_server, ROCK_COLOR_ERROR, 0);
        }
    }

    lvgl_port_unlock();
}

void rock_gui_set_browse_stations(const rock_gui_station_item_t *items, size_t count,
                                  const char *next_cursor)
{
    (void)next_cursor;
    if (!lvgl_port_lock(1000)) return;
    s_catalog_stale = false;
    gui_browse_set_stations(items, count);
    lvgl_port_unlock();
}

void rock_gui_set_search_results(const rock_gui_station_item_t *items, size_t count)
{
    if (!lvgl_port_lock(1000)) return;
    s_catalog_stale = false;
    gui_search_set_results(items, count);
    lvgl_port_unlock();
}

void rock_gui_show_loading(const char *message)
{
    if (!lvgl_port_lock(500)) return;
    gui_overlay_set(GUI_OVERLAY_LOADING, message, NULL, false);
    lvgl_port_unlock();
}

void rock_gui_show_empty(const char *message)
{
    if (!lvgl_port_lock(500)) return;
    gui_overlay_set(GUI_OVERLAY_EMPTY, message, NULL, true);
    lvgl_port_unlock();
}

void rock_gui_show_offline(const char *message)
{
    if (!lvgl_port_lock(500)) return;
    gui_overlay_set(GUI_OVERLAY_OFFLINE, message, NULL, true);
    lvgl_port_unlock();
}

void rock_gui_show_error(const char *message)
{
    if (!lvgl_port_lock(500)) return;
    gui_overlay_set(GUI_OVERLAY_ERROR, NULL, message, true);
    lvgl_port_unlock();
}

void rock_gui_dismiss_state(void)
{
    if (!lvgl_port_lock(500)) return;
    gui_overlay_hide();
    lvgl_port_unlock();
}

void rock_gui_show_cast_menu(bool scanning)
{
    if (!lvgl_port_lock(500)) return;
    gui_cast_menu_show(scanning);
    lvgl_port_unlock();
}

void rock_gui_set_cast_devices(const rock_gui_cast_item_t *items, size_t count)
{
    if (!lvgl_port_lock(500)) return;
    gui_cast_set_devices(items, count);
    lvgl_port_unlock();
}

void rock_gui_dismiss_cast_menu(void)
{
    if (!lvgl_port_lock(500)) return;
    gui_cast_menu_dismiss();
    lvgl_port_unlock();
}

void rock_gui_update_cast_status(bool is_casting, const char *receiver_name)
{
    if (!lvgl_port_lock(500)) return;

    if (receiver_name && receiver_name[0]) {
        strncpy(s_cast_receiver_name, receiver_name, sizeof(s_cast_receiver_name) - 1);
    } else {
        s_cast_receiver_name[0] = '\0';
    }

    gui_now_playing_apply_cast_state(is_casting, s_cast_receiver_name);
    gui_cast_apply_active_output(is_casting);

    lvgl_port_unlock();
}

void rock_gui_mark_catalog_stale(void)
{
    if (!lvgl_port_lock(500)) return;

    s_catalog_stale = true;
    gui_browse_hide_rows();
    gui_search_hide_rows();
    if (gui_active_view() == VIEW_BROWSE || gui_active_view() == VIEW_SEARCH) {
        gui_overlay_set(GUI_OVERLAY_OFFLINE, "Связь с сервером потеряна", NULL, true);
    }

    lvgl_port_unlock();
}

void rock_gui_set_starting_station(const char *station_id)
{
    if (!lvgl_port_lock(500)) return;

    if (station_id) {
        strncpy(s_station_id, station_id, sizeof(s_station_id) - 1);
    }
    if (s_lbl_ticker) {
        lv_label_set_text(s_lbl_ticker, "Starting...");
    }


    lvgl_port_unlock();
}

void rock_gui_get_metrics(uint32_t *out_fps, size_t *out_free_heap)
{
    if (out_fps) *out_fps = s_fps;
    if (out_free_heap) *out_free_heap = esp_get_free_heap_size();
}

void rock_gui_present(const char *station_id, const char *title, const char *subtitle)
{
    if (!s_gui_active || !lvgl_port_lock(500)) return;
    bool status = title && (!strcmp(title, "PLAYING") || !strcmp(title, "PAUSED") ||
        !strcmp(title, "BUFFERING") || !strcmp(title, "STOPPED") ||
        !strcmp(title, "Starting...") || !strcmp(title, "PLAYBACK ERROR"));
    if (!status) {
        if (station_id && *station_id) {
            gui_now_playing_present(station_id, title, subtitle);
        } else if (subtitle && *subtitle) {
            gui_overlay_set(GUI_OVERLAY_EMPTY, title ? title : "", subtitle, true);
        } else if (s_lbl_ticker) lv_label_set_text(s_lbl_ticker, title ? title : "");
    }
    lvgl_port_unlock();
}

bool rock_gui_is_active(void)
{
    return s_gui_active;
}

void rock_gui_switch_view(int view_index)
{
    if (!lvgl_port_lock(500)) return;
    if (s_screen && lv_screen_active() != s_screen) lv_screen_load(s_screen);
    if (find_view_reg((rock_gui_view_mode_t)view_index)) {
        gui_switch_view((rock_gui_view_mode_t)view_index);
    }
    lvgl_port_unlock();
}

void rock_gui_trigger(const char *action)
{
    if (!action || !lvgl_port_lock(500)) return;
    if (gui_now_playing_trigger(action)) {
    } else if (gui_overlay_trigger(action)) {
    } else if (gui_search_trigger(action)) {
    } else if (gui_cast_trigger(action)) {
    } else if (!strncmp(action, "STATIONID:", 10)) {
        const char *station_id = action + 10;
        if (*station_id && strlen(station_id) <= 128 &&
            strspn(station_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~") == strlen(station_id)) {
            ESP_LOGI(TAG, "Serial trigger station id='%s'", station_id);
            gui_enqueue_event(ROCK_UI_EVT_PLAY_STATION, station_id, 0);
        }
    }
    else if (!strncmp(action, "STATION:", 8)) {
        int row = atoi(action + 8);
        station_row_ui_t *rows = gui_active_view() == VIEW_SEARCH ? gui_search_rows() : gui_browse_rows();
        if (row >= 0 && row < ROCK_GUI_MAX_STATIONS && !gui_catalog_stale() &&
            !lv_obj_has_flag(rows[row].row, LV_OBJ_FLAG_HIDDEN)) {
            ESP_LOGI(TAG, "Serial trigger station row=%d id='%s'", row, rows[row].station_id);
            lv_obj_send_event(rows[row].row, LV_EVENT_CLICKED, NULL);
        } else {
            ESP_LOGW(TAG, "Serial trigger station row=%d invalid (stale=%d hidden=%d)",
                     row, gui_catalog_stale(),
                     (row >= 0 && row < ROCK_GUI_MAX_STATIONS) ? lv_obj_has_flag(rows[row].row, LV_OBJ_FLAG_HIDDEN) : -1);
        }
    }
    lvgl_port_unlock();
}

void rock_gui_log_debug_state(void)
{
    ESP_LOGI(TAG, "ui view=%d browse=%u search=%u stale=%d playback=%d volume=%u",
             (int)s_active_view, (unsigned)gui_browse_rows_count(),
             (unsigned)gui_search_rows_count(), (int)s_catalog_stale,
             (int)s_playback_status, (unsigned)s_volume_level);
}
