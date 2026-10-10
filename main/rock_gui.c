#include "rock_gui.h"
#include "esp_wifi.h"
#include "rockcast_tokens.h"
#include "rock_fonts.h"
#include "audio_spectrum.h"
#include "display_bsp.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "draw/snapshot/lv_snapshot.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

static const char *TAG = "rock-gui";
static bool s_gui_active = false;

typedef enum {
    VIEW_NOW_PLAYING = 0,
    VIEW_BROWSE,
    VIEW_SEARCH
} rock_gui_view_mode_t;

// FreeRTOS queue for UI events
static QueueHandle_t s_ui_event_queue = NULL;

// Active state
static rock_gui_view_mode_t s_active_view = VIEW_NOW_PLAYING;
static rock_player_status_t s_playback_status = ROCK_PLAYER_STATUS_IDLE;
static char s_station_id[129] = "";
static uint8_t s_volume_level = 65;
static bool s_volume_muted = false;
static bool s_catalog_stale = false;

// Metrics
static uint32_t s_fps = 0;
static uint32_t s_frame_counter = 0;
static int64_t s_last_fps_time_us = 0;

// UI Handles - Shell
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

// View 1: Now Playing
static lv_obj_t *s_view_np = NULL;
static lv_obj_t *s_lbl_np_badge = NULL;
static lv_obj_t *s_lbl_np_station = NULL;
static lv_obj_t *s_lbl_np_subtitle = NULL;
static lv_obj_t *s_lbl_np_track = NULL;
static lv_obj_t *s_cont_np_codec = NULL;
static lv_obj_t *s_lbl_np_codec = NULL;

static lv_obj_t *s_cont_spectrum = NULL;
static lv_obj_t *s_spectrum_bars[SPECTRUM_BANDS_COUNT];

// Transport controls
static lv_obj_t *s_cont_transport = NULL;
static lv_obj_t *s_btn_browse = NULL;
static lv_obj_t *s_btn_search = NULL;
static lv_obj_t *s_btn_play_pause = NULL;
static lv_obj_t *s_lbl_play_pause = NULL;
static lv_obj_t *s_btn_stop = NULL;
static lv_obj_t *s_btn_vol_down = NULL;
static lv_obj_t *s_lbl_vol_val = NULL;
static lv_obj_t *s_btn_vol_up = NULL;
static lv_obj_t *s_btn_cast = NULL;
static lv_obj_t *s_lbl_cast = NULL;

// Cast overlay handles
static lv_obj_t *s_cast_overlay = NULL;
static lv_obj_t *s_cast_status_lbl = NULL;
static lv_obj_t *s_cast_spinner = NULL;
static lv_obj_t *s_cast_list = NULL;
static lv_obj_t *s_btn_cast_refresh = NULL;
static lv_obj_t *s_cast_local_btn = NULL;
static lv_obj_t *s_cast_local_badge = NULL;

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_badge;
    char receiver_id[65];
} cast_device_row_t;

static cast_device_row_t s_cast_rows[ROCK_GUI_MAX_CAST_DEVICES];
static size_t s_cast_rows_count = 0;
static bool s_is_casting = false;
static char s_cast_receiver_name[129] = {0};

// View 2: Browse
static lv_obj_t *s_view_browse = NULL;
static lv_obj_t *s_browse_header = NULL;
static lv_obj_t *s_btn_browse_back = NULL;
static lv_obj_t *s_lbl_browse_title = NULL;
static lv_obj_t *s_list_browse = NULL;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *icon_box;
    lv_obj_t *icon_lbl;
    lv_obj_t *name_lbl;
    lv_obj_t *desc_lbl;
    lv_obj_t *codec_lbl;
    lv_obj_t *play_lbl;
    char station_id[129];
} station_row_ui_t;

static station_row_ui_t s_browse_rows[ROCK_GUI_MAX_STATIONS];
static size_t s_browse_rows_count = 0;

// View 3: Search
static lv_obj_t *s_view_search = NULL;
static lv_obj_t *s_search_header = NULL;
static lv_obj_t *s_btn_search_back = NULL;
static lv_obj_t *s_ta_search = NULL;
static lv_obj_t *s_btn_search_clear = NULL;
static lv_obj_t *s_cont_chips = NULL;
static lv_obj_t *s_kb_search = NULL;
static lv_obj_t *s_list_search = NULL;
static station_row_ui_t s_search_rows[ROCK_GUI_MAX_STATIONS];
static size_t s_search_rows_count = 0;

// Overlays
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_overlay_spinner = NULL;
static lv_obj_t *s_overlay_icon = NULL;
static lv_obj_t *s_overlay_title = NULL;
static lv_obj_t *s_overlay_desc = NULL;
static lv_obj_t *s_overlay_btn1 = NULL;
static lv_obj_t *s_overlay_lbl1 = NULL;
static lv_obj_t *s_overlay_btn2 = NULL;
static lv_obj_t *s_overlay_lbl2 = NULL;

// Four rows, explicit language keys; all layouts can return to Russian.
static const char * const kb_map_ru[] = {
    "ё", "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ъ", LV_SYMBOL_BACKSPACE, "\n",
    "EN", "ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "\n",
    "123", "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", ".", "-", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char * const kb_map_en[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "RU", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "123", "z", "x", "c", "v", "b", "n", "m", ".", "-", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char * const kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "RU", "EN", "!", "?", "@", "#", "$", "%", "&", "*", "\n",
    "+", "-", "=", "(", ")", "/", ":", ";", ".", ",", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static lv_buttonmatrix_ctrl_t kb_controls[3][48];
static void keyboard_input(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_buttonmatrix_get_button_text(kb, id);
    if (!strcmp(text, "RU")) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
    else if (!strcmp(text, "EN")) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_2);
    else if (!strcmp(text, "123")) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_3);
    else lv_keyboard_def_event_cb(e);
}

static void switch_view(rock_gui_view_mode_t view);
static void enqueue_event(rock_ui_event_type_t type, const char *text, int16_t val);
static void serial_cmd_task(void *arg);

// ---------------------------------------------------------------------------
// Touch Event Callbacks (Runs in LVGL task context - MUST NEVER BLOCK)
// ---------------------------------------------------------------------------

static void on_nav_browse_click(lv_event_t *e)
{
    (void)e;
    switch_view(VIEW_BROWSE);
    enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, "", 0);
}

static void on_nav_search_click(lv_event_t *e)
{
    (void)e;
    switch_view(VIEW_SEARCH);
}

static void on_back_to_np_click(lv_event_t *e)
{
    (void)e;
    switch_view(VIEW_NOW_PLAYING);
}

static void on_play_pause_click(lv_event_t *e)
{
    (void)e;
    if (s_playback_status == ROCK_PLAYER_STATUS_PLAYING) {
        enqueue_event(ROCK_UI_EVT_PLAYBACK_PAUSE, NULL, 0);
    } else {
        enqueue_event(ROCK_UI_EVT_PLAYBACK_PLAY, NULL, 0);
    }
}

static void on_stop_click(lv_event_t *e)
{
    (void)e;
    enqueue_event(ROCK_UI_EVT_PLAYBACK_STOP, NULL, 0);
}

static void on_vol_down_click(lv_event_t *e)
{
    (void)e;
    enqueue_event(ROCK_UI_EVT_VOLUME_CHANGE, NULL, -5);
}

static void on_vol_up_click(lv_event_t *e)
{
    (void)e;
    enqueue_event(ROCK_UI_EVT_VOLUME_CHANGE, NULL, 5);
}

static void on_station_row_click(lv_event_t *e)
{
    station_row_ui_t *row_data = (station_row_ui_t *)lv_event_get_user_data(e);
    if (!row_data || row_data->station_id[0] == '\0') return;

    if (s_lbl_ticker) {
        lv_label_set_text_fmt(s_lbl_ticker, "Starting: %s...", lv_label_get_text(row_data->name_lbl));
    }
    snprintf(s_station_id, sizeof(s_station_id), "%s", row_data->station_id);
    lv_label_set_text(s_lbl_np_station, lv_label_get_text(row_data->name_lbl));
    lv_label_set_text(s_lbl_np_subtitle, lv_label_get_text(row_data->desc_lbl));
    lv_label_set_text(s_lbl_np_codec, lv_label_get_text(row_data->codec_lbl));
    lv_label_set_text(s_lbl_np_track, "");
    enqueue_event(ROCK_UI_EVT_PLAY_STATION, row_data->station_id, 0);
    switch_view(VIEW_NOW_PLAYING);
}

static void on_genre_chip_click(lv_event_t *e)
{
    const char *genre = (const char *)lv_event_get_user_data(e);
    if (!genre) return;
    if (s_ta_search) {
        lv_textarea_set_text(s_ta_search, genre);
    }
    enqueue_event(ROCK_UI_EVT_CATALOG_SEARCH, genre, 0);
    rock_gui_show_loading("Поиск по жанру...");
}

static void on_search_edit(lv_event_t *e)
{
    (void)e;
    if (s_kb_search) lv_obj_remove_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
    if (s_list_search) lv_obj_add_flag(s_list_search, LV_OBJ_FLAG_HIDDEN);
}

static void on_search_clear_click(lv_event_t *e)
{
    on_search_edit(e);
    if (s_ta_search) {
        lv_textarea_set_text(s_ta_search, "");
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *txt = lv_textarea_get_text(s_ta_search);
        if (txt && txt[0] != '\0') {
            enqueue_event(ROCK_UI_EVT_CATALOG_SEARCH, txt, 0);
            rock_gui_show_loading("Поиск станций...");
        }
    }
}

static void on_overlay_retry_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_state();
    enqueue_event(ROCK_UI_EVT_RETRY, NULL, 0);
}

static void on_overlay_back_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_state();
    if (s_active_view == VIEW_SEARCH) on_search_edit(NULL);
    else switch_view(VIEW_NOW_PLAYING);
}

static void on_cast_btn_click(lv_event_t *e)
{
    (void)e;
    rock_gui_show_cast_menu(true);
    enqueue_event(ROCK_UI_EVT_CAST_BROWSE, NULL, 0);
}

static void on_cast_close_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_cast_menu();
}

static void on_cast_local_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_cast_menu();
    enqueue_event(ROCK_UI_EVT_CAST_DISCONNECT, NULL, 0);
}

static void on_cast_refresh_click(lv_event_t *e)
{
    (void)e;
    if (s_cast_spinner) lv_obj_remove_flag(s_cast_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_cast_status_lbl) {
        lv_label_set_text(s_cast_status_lbl, "Поиск устройств Chromecast...");
        lv_obj_remove_flag(s_cast_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    enqueue_event(ROCK_UI_EVT_CAST_BROWSE, NULL, 0);
}

static void on_cast_device_row_click(lv_event_t *e)
{
    cast_device_row_t *row = (cast_device_row_t *)lv_event_get_user_data(e);
    if (!row || row->receiver_id[0] == '\0') return;
    rock_gui_dismiss_cast_menu();
    enqueue_event(ROCK_UI_EVT_CAST_SELECT, row->receiver_id, 0);
}

// ---------------------------------------------------------------------------
// Helpers & Navigation
// ---------------------------------------------------------------------------

static void enqueue_event(rock_ui_event_type_t type, const char *text, int16_t val)
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

static void switch_view(rock_gui_view_mode_t view)
{
    s_active_view = view;
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_cast_overlay) lv_obj_add_flag(s_cast_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_cont_transport) {
        if (view == VIEW_NOW_PLAYING) lv_obj_remove_flag(s_cont_transport, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_cont_transport, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_view_np) lv_obj_add_flag(s_view_np, LV_OBJ_FLAG_HIDDEN);
    if (s_view_browse) lv_obj_add_flag(s_view_browse, LV_OBJ_FLAG_HIDDEN);
    if (s_view_search) lv_obj_add_flag(s_view_search, LV_OBJ_FLAG_HIDDEN);

    switch (view) {
        case VIEW_NOW_PLAYING:
            if (s_view_np) lv_obj_remove_flag(s_view_np, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_section) lv_label_set_text(s_lbl_section, "• Сейчас играет");
            break;
        case VIEW_BROWSE:
            if (s_view_browse) lv_obj_remove_flag(s_view_browse, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_section) lv_label_set_text(s_lbl_section, "• Каталог станций");
            break;
        case VIEW_SEARCH:
            if (s_view_search) lv_obj_remove_flag(s_view_search, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_section) lv_label_set_text(s_lbl_section, "• Поиск станций");
            break;
    }
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

    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        if (!s_spectrum_bars[i]) continue;
        float val = bands[i];
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;

        int h = 4 + (int)(val * 64.0f);
        lv_obj_set_height(s_spectrum_bars[i], h);
        lv_obj_set_y(s_spectrum_bars[i], 74 - h);

        if (val > 0.08f && s_playback_status == ROCK_PLAYER_STATUS_PLAYING) {
            lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_ACCENT, 0);
        } else {
            lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_BAR_DIM, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Construction of GUI Views
// ---------------------------------------------------------------------------

static void build_header(lv_obj_t *parent)
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

static void build_ticker(lv_obj_t *parent)
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

static void build_now_playing_view(lv_obj_t *parent)
{
    s_view_np = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_np);
    lv_obj_set_size(s_view_np, 800, 404);
    lv_obj_set_pos(s_view_np, 0, 0);

    // Left Metadata Area (x=24, y=16, w=480, h=300)
    lv_obj_t *meta_box = lv_obj_create(s_view_np);
    lv_obj_remove_style_all(meta_box);
    lv_obj_set_size(meta_box, 480, 300);
    lv_obj_set_pos(meta_box, 24, 16);
    lv_obj_set_flex_flow(meta_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(meta_box, 6, 0);

    s_lbl_np_badge = lv_label_create(meta_box);
    lv_label_set_text(s_lbl_np_badge, "СЕЙЧАС ИГРАЕТ");
    lv_obj_set_style_text_color(s_lbl_np_badge, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_lbl_np_badge, &font_inter_14, 0);

    s_lbl_np_station = lv_label_create(meta_box);
    lv_label_set_text(s_lbl_np_station, "Выберите станцию");
    lv_label_set_long_mode(s_lbl_np_station, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_np_station, 470);
    lv_obj_set_style_text_color(s_lbl_np_station, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_lbl_np_station, &font_inter_24, 0);

    s_lbl_np_subtitle = lv_label_create(meta_box);
    lv_label_set_text(s_lbl_np_subtitle, "");
    lv_obj_set_style_text_color(s_lbl_np_subtitle, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_np_subtitle, &font_inter_14, 0);

    s_lbl_np_track = lv_label_create(meta_box);
    lv_label_set_text(s_lbl_np_track, "");
    lv_label_set_long_mode(s_lbl_np_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_np_track, 470);
    lv_obj_set_style_text_color(s_lbl_np_track, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_lbl_np_track, &font_inter_18, 0);
    lv_obj_set_style_pad_top(s_lbl_np_track, 8, 0);

    s_cont_np_codec = lv_obj_create(meta_box);
    lv_obj_remove_style_all(s_cont_np_codec);
    lv_obj_set_size(s_cont_np_codec, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_bg_color(s_cont_np_codec, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_cont_np_codec, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_cont_np_codec, 4, 0);
    lv_obj_set_style_pad_left(s_cont_np_codec, 10, 0);
    lv_obj_set_style_pad_right(s_cont_np_codec, 10, 0);
    lv_obj_set_style_pad_top(s_cont_np_codec, 4, 0);

    s_lbl_np_codec = lv_label_create(s_cont_np_codec);
    lv_label_set_text(s_lbl_np_codec, "");
    lv_obj_set_style_text_color(s_lbl_np_codec, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_np_codec, &font_inter_14, 0);

    // Right Visual Area: Art + Spectrum (x=520, y=16, w=256, h=300)
    lv_obj_t *art_box = lv_obj_create(s_view_np);
    lv_obj_remove_style_all(art_box);
    lv_obj_set_size(art_box, 80, 80);
    lv_obj_set_pos(art_box, 608, 16);
    lv_obj_set_style_bg_color(art_box, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(art_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(art_box, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(art_box, 1, 0);
    lv_obj_set_style_radius(art_box, 8, 0);

    lv_obj_t *art_icon = lv_label_create(art_box);
    lv_label_set_text(art_icon, ROCK_ICON_PLAY);
    lv_obj_set_style_text_color(art_icon, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(art_icon, &font_inter_24, 0);
    lv_obj_center(art_icon);

    // 16-band Spectrum Container (x=520, y=110, w=256, h=80)
    s_cont_spectrum = lv_obj_create(s_view_np);
    lv_obj_remove_style_all(s_cont_spectrum);
    lv_obj_set_size(s_cont_spectrum, 256, 80);
    lv_obj_set_pos(s_cont_spectrum, 520, 110);
    lv_obj_set_style_bg_color(s_cont_spectrum, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_cont_spectrum, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cont_spectrum, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(s_cont_spectrum, 1, 0);
    lv_obj_set_style_radius(s_cont_spectrum, 6, 0);

    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        s_spectrum_bars[i] = lv_obj_create(s_cont_spectrum);
        lv_obj_remove_style_all(s_spectrum_bars[i]);
        lv_obj_set_size(s_spectrum_bars[i], 11, 4);
        lv_obj_set_pos(s_spectrum_bars[i], 10 + i * 15, 74 - 4);
        lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_BAR_DIM, 0);
        lv_obj_set_style_bg_opa(s_spectrum_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_spectrum_bars[i], ROCK_RADIUS_BAR, 0);
    }

    // Transport Bar (x=16, y=330, w=768, h=64)
    s_cont_transport = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_cont_transport);
    lv_obj_set_size(s_cont_transport, 768, ROCK_TRANSPORT_HEIGHT);
    lv_obj_set_pos(s_cont_transport, 16, 370);
    lv_obj_set_style_bg_color(s_cont_transport, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_cont_transport, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cont_transport, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(s_cont_transport, 1, 0);
    lv_obj_set_style_radius(s_cont_transport, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_style_pad_left(s_cont_transport, 12, 0);
    lv_obj_set_style_pad_right(s_cont_transport, 12, 0);
    lv_obj_set_flex_flow(s_cont_transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cont_transport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Nav: Browse
    s_btn_browse = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_browse, 100, 48);
    lv_obj_set_style_bg_color(s_btn_browse, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_browse, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_browse, on_nav_browse_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_b = lv_label_create(s_btn_browse);
    lv_label_set_text(lbl_b, "Каталог");
    lv_obj_set_style_text_font(lbl_b, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_b, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_b);

    // Nav: Search
    s_btn_search = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_search, 90, 48);
    lv_obj_set_style_bg_color(s_btn_search, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_search, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_pad_left(s_btn_search, 8, 0);
    lv_obj_add_event_cb(s_btn_search, on_nav_search_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_s = lv_label_create(s_btn_search);
    lv_label_set_text(lbl_s, "Поиск");
    lv_obj_set_style_text_font(lbl_s, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_s, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_s);

    // Spacer
    lv_obj_t *sp1 = lv_obj_create(s_cont_transport);
    lv_obj_remove_style_all(sp1);
    lv_obj_set_flex_grow(sp1, 1);

    // Play/Pause Dominant Button (64x52)
    s_btn_play_pause = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_play_pause, 64, 52);
    lv_obj_set_style_bg_color(s_btn_play_pause, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_btn_play_pause, 6, 0);
    lv_obj_add_event_cb(s_btn_play_pause, on_play_pause_click, LV_EVENT_CLICKED, NULL);
    s_lbl_play_pause = lv_label_create(s_btn_play_pause);
    lv_label_set_text(s_lbl_play_pause, ROCK_ICON_PLAY);
    lv_obj_set_style_text_font(s_lbl_play_pause, &font_inter_24, 0);
    lv_obj_set_style_text_color(s_lbl_play_pause, ROCK_COLOR_WHITE, 0);
    lv_obj_center(s_lbl_play_pause);

    // Stop Button (54x52)
    s_btn_stop = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_stop, 54, 52);
    lv_obj_set_style_bg_color(s_btn_stop, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_stop, 6, 0);
    lv_obj_add_event_cb(s_btn_stop, on_stop_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_stop = lv_label_create(s_btn_stop);
    lv_label_set_text(lbl_stop, ROCK_ICON_STOP);
    lv_obj_set_style_text_font(lbl_stop, &font_inter_18, 0);
    lv_obj_set_style_text_color(lbl_stop, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_stop);

    // Spacer
    lv_obj_t *sp2 = lv_obj_create(s_cont_transport);
    lv_obj_remove_style_all(sp2);
    lv_obj_set_flex_grow(sp2, 1);

    // Volume Controls: [-] 65% [+]
    s_btn_vol_down = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_vol_down, 48, 48);
    lv_obj_set_style_bg_color(s_btn_vol_down, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_vol_down, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_vol_down, on_vol_down_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_vd = lv_label_create(s_btn_vol_down);
    lv_label_set_text(lbl_vd, "-");
    lv_obj_set_style_text_font(lbl_vd, &font_inter_18, 0);
    lv_obj_set_style_text_color(lbl_vd, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_vd);

    s_lbl_vol_val = lv_label_create(s_cont_transport);
    lv_label_set_text(s_lbl_vol_val, " 65% ");
    lv_obj_set_style_text_font(s_lbl_vol_val, &font_hack_16, 0);
    lv_obj_set_style_text_color(s_lbl_vol_val, ROCK_COLOR_FG, 0);

    s_btn_vol_up = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_vol_up, 48, 48);
    lv_obj_set_style_bg_color(s_btn_vol_up, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_vol_up, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_vol_up, on_vol_up_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_vu = lv_label_create(s_btn_vol_up);
    lv_label_set_text(lbl_vu, "+");
    lv_obj_set_style_text_font(lbl_vu, &font_inter_18, 0);
    lv_obj_set_style_text_color(lbl_vu, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_vu);

    // Cast dominant action button
    s_btn_cast = lv_button_create(s_cont_transport);
    lv_obj_set_size(s_btn_cast, 68, 48);
    lv_obj_set_style_bg_color(s_btn_cast, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_cast, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_cast, on_cast_btn_click, LV_EVENT_CLICKED, NULL);
    s_lbl_cast = lv_label_create(s_btn_cast);
    lv_label_set_text(s_lbl_cast, "Cast");
    lv_obj_set_style_text_font(s_lbl_cast, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_lbl_cast, ROCK_COLOR_FG, 0);
    lv_obj_center(s_lbl_cast);
}

static void setup_station_row(station_row_ui_t *row_ui, lv_obj_t *parent, int index)
{
    row_ui->row = lv_obj_create(parent);
    lv_obj_remove_style_all(row_ui->row);
    lv_obj_set_size(row_ui->row, 768, ROCK_LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(row_ui->row, (index % 2 == 0) ? ROCK_COLOR_PANEL : ROCK_COLOR_ROW_ALT, 0);
    lv_obj_set_style_bg_opa(row_ui->row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row_ui->row, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_pad_left(row_ui->row, 12, 0);
    lv_obj_set_style_pad_right(row_ui->row, 16, 0);
    lv_obj_set_flex_flow(row_ui->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_ui->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row_ui->row, on_station_row_click, LV_EVENT_CLICKED, row_ui);

    // Left Icon box 40x40
    row_ui->icon_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(row_ui->icon_box);
    lv_obj_set_size(row_ui->icon_box, 40, 40);
    lv_obj_set_style_bg_color(row_ui->icon_box, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(row_ui->icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row_ui->icon_box, 4, 0);

    row_ui->icon_lbl = lv_label_create(row_ui->icon_box);
    lv_label_set_text(row_ui->icon_lbl, "R");
    lv_obj_set_style_text_font(row_ui->icon_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->icon_lbl, ROCK_COLOR_MUTED, 0);
    lv_obj_center(row_ui->icon_lbl);

    // Text box (name + desc)
    lv_obj_t *text_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(text_box);
    lv_obj_set_size(text_box, 500, 48);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(text_box, 12, 0);

    row_ui->name_lbl = lv_label_create(text_box);
    lv_label_set_text(row_ui->name_lbl, "Station Name");
    lv_label_set_long_mode(row_ui->name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row_ui->name_lbl, 490);
    lv_obj_set_style_text_font(row_ui->name_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->name_lbl, ROCK_COLOR_FG, 0);

    row_ui->desc_lbl = lv_label_create(text_box);
    lv_label_set_text(row_ui->desc_lbl, "Genre / Tags");
    lv_label_set_long_mode(row_ui->desc_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row_ui->desc_lbl, 490);
    lv_obj_set_style_text_font(row_ui->desc_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(row_ui->desc_lbl, ROCK_COLOR_MUTED, 0);

    // Codec capsule
    row_ui->codec_lbl = lv_label_create(row_ui->row);
    lv_label_set_text(row_ui->codec_lbl, "128k");
    lv_obj_set_style_text_font(row_ui->codec_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(row_ui->codec_lbl, ROCK_COLOR_MUTED, 0);
    lv_obj_set_width(row_ui->codec_lbl, 70);

    // Play action indicator
    row_ui->play_lbl = lv_label_create(row_ui->row);
    lv_label_set_text(row_ui->play_lbl, ROCK_ICON_PLAY);
    lv_obj_set_style_text_font(row_ui->play_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->play_lbl, ROCK_COLOR_ACCENT, 0);

    lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
}

static void build_browse_view(lv_obj_t *parent)
{
    s_view_browse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_browse);
    lv_obj_set_size(s_view_browse, 800, 404);
    lv_obj_set_pos(s_view_browse, 0, 0);
    lv_obj_add_flag(s_view_browse, LV_OBJ_FLAG_HIDDEN);

    // Header (h=48)
    s_browse_header = lv_obj_create(s_view_browse);
    lv_obj_remove_style_all(s_browse_header);
    lv_obj_set_size(s_browse_header, 800, 48);
    lv_obj_set_pos(s_browse_header, 0, 0);
    lv_obj_set_style_pad_left(s_browse_header, 16, 0);
    lv_obj_set_style_pad_right(s_browse_header, 16, 0);
    lv_obj_set_flex_flow(s_browse_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_browse_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_btn_browse_back = lv_button_create(s_browse_header);
    lv_obj_set_size(s_btn_browse_back, 110, 40);
    lv_obj_set_style_bg_color(s_btn_browse_back, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_browse_back, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_browse_back, on_back_to_np_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(s_btn_browse_back);
    lv_label_set_text(lbl_back, ROCK_ICON_ARROW_LEFT " Плеер");
    lv_obj_set_style_text_font(lbl_back, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_back, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_back);

    s_lbl_browse_title = lv_label_create(s_browse_header);
    lv_label_set_text(s_lbl_browse_title, "КАТАЛОГ РАДИОСТАНЦИЙ");
    lv_obj_set_style_text_font(s_lbl_browse_title, &font_inter_18, 0);
    lv_obj_set_style_text_color(s_lbl_browse_title, ROCK_COLOR_FG, 0);
    lv_obj_set_style_pad_left(s_lbl_browse_title, 16, 0);

    // Scrollable List container (x=16, y=52, w=768, h=344)
    s_list_browse = lv_obj_create(s_view_browse);
    lv_obj_remove_style_all(s_list_browse);
    lv_obj_set_size(s_list_browse, 768, 344);
    lv_obj_set_pos(s_list_browse, 16, 52);
    lv_obj_set_flex_flow(s_list_browse, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_browse, 4, 0);
    lv_obj_set_scrollbar_mode(s_list_browse, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        setup_station_row(&s_browse_rows[i], s_list_browse, i);
    }
}

static void build_search_view(lv_obj_t *parent)
{
    s_view_search = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_search);
    lv_obj_set_size(s_view_search, 800, 404);
    lv_obj_set_pos(s_view_search, 0, 0);
    lv_obj_add_flag(s_view_search, LV_OBJ_FLAG_HIDDEN);

    // Header bar with Back + Input + Clear
    s_search_header = lv_obj_create(s_view_search);
    lv_obj_remove_style_all(s_search_header);
    lv_obj_set_size(s_search_header, 800, 48);
    lv_obj_set_pos(s_search_header, 0, 0);
    lv_obj_set_style_pad_left(s_search_header, 16, 0);
    lv_obj_set_style_pad_right(s_search_header, 16, 0);
    lv_obj_set_flex_flow(s_search_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_search_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_btn_search_back = lv_button_create(s_search_header);
    lv_obj_set_size(s_btn_search_back, 110, 40);
    lv_obj_set_style_bg_color(s_btn_search_back, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_search_back, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_search_back, on_back_to_np_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(s_btn_search_back);
    lv_label_set_text(lbl_back, ROCK_ICON_ARROW_LEFT " Назад");
    lv_obj_set_style_text_font(lbl_back, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_back, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_back);

    s_ta_search = lv_textarea_create(s_search_header);
    lv_obj_set_size(s_ta_search, 520, 40);
    lv_obj_set_style_bg_color(s_ta_search, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_text_color(s_ta_search, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_ta_search, &font_inter_18, 0);
    lv_obj_set_style_radius(s_ta_search, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_border_color(s_ta_search, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(s_ta_search, 1, 0);
    lv_textarea_set_one_line(s_ta_search, true);
    lv_textarea_set_max_length(s_ta_search, 128);
    lv_obj_add_event_cb(s_ta_search, on_search_edit, LV_EVENT_CLICKED, NULL);
    lv_textarea_set_placeholder_text(s_ta_search, "Поиск станций...");

    s_btn_search_clear = lv_button_create(s_search_header);
    lv_obj_set_size(s_btn_search_clear, 90, 40);
    lv_obj_set_style_bg_color(s_btn_search_clear, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_search_clear, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_pad_left(s_btn_search_clear, 8, 0);
    lv_obj_add_event_cb(s_btn_search_clear, on_search_clear_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_clr = lv_label_create(s_btn_search_clear);
    lv_label_set_text(lbl_clr, "Очистить");
    lv_obj_set_style_text_font(lbl_clr, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_clr, ROCK_COLOR_MUTED, 0);
    lv_obj_center(lbl_clr);

    // Genre Chips Container (x=16, y=50, w=768, h=36)
    s_cont_chips = lv_obj_create(s_view_search);
    lv_obj_remove_style_all(s_cont_chips);
    lv_obj_set_size(s_cont_chips, 768, 36);
    lv_obj_set_pos(s_cont_chips, 16, 50);
    lv_obj_set_flex_flow(s_cont_chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_cont_chips, 8, 0);

    static const char *genres[] = {"Rock", "Pop", "Jazz", "Metal", "Classical", "News", "Electronic"};
    for (size_t i = 0; i < sizeof(genres) / sizeof(genres[0]); i++) {
        lv_obj_t *chip = lv_button_create(s_cont_chips);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 32);
        lv_obj_set_style_bg_color(chip, ROCK_COLOR_PANEL_2, 0);
        lv_obj_set_style_radius(chip, 16, 0);
        lv_obj_set_style_pad_left(chip, 12, 0);
        lv_obj_set_style_pad_right(chip, 12, 0);
        lv_obj_add_event_cb(chip, on_genre_chip_click, LV_EVENT_CLICKED, (void *)genres[i]);

        lv_obj_t *chip_lbl = lv_label_create(chip);
        lv_label_set_text(chip_lbl, genres[i]);
        lv_obj_set_style_text_font(chip_lbl, &font_inter_14, 0);
        lv_obj_set_style_text_color(chip_lbl, ROCK_COLOR_FG, 0);
        lv_obj_center(chip_lbl);
    }

    // Results container (scrollable list, initially hidden or under keyboard)
    s_list_search = lv_obj_create(s_view_search);
    lv_obj_remove_style_all(s_list_search);
    lv_obj_set_size(s_list_search, 768, 308);
    lv_obj_set_pos(s_list_search, 16, 90);
    lv_obj_set_flex_flow(s_list_search, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_search, 4, 0);
    lv_obj_add_flag(s_list_search, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        setup_station_row(&s_search_rows[i], s_list_search, i);
    }

    // On-Screen Virtual Keyboard (x=16, y=92, w=768, h=220)
    // Four rows with 55 px each before gaps (satisfies the 44 px target).
    s_kb_search = lv_keyboard_create(s_view_search);
    lv_obj_set_size(s_kb_search, 768, 220);
    lv_obj_set_align(s_kb_search, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_kb_search, 16, 92);
    lv_keyboard_set_textarea(s_kb_search, s_ta_search);
    const char * const *maps[] = {kb_map_ru, kb_map_en, kb_map_num};
    for (int m = 0; m < 3; m++) {
        int button = 0;
        for (int i = 0; maps[m][i][0]; i++) {
            if (!strcmp(maps[m][i], "\n")) continue;
            kb_controls[m][button++] = LV_KEYBOARD_CTRL_BUTTON_FLAGS | (!strcmp(maps[m][i], " ") ? 5 : 1);
        }
        lv_keyboard_set_map(s_kb_search, LV_KEYBOARD_MODE_USER_1 + m, maps[m], kb_controls[m]);
    }
    lv_obj_remove_event_cb(s_kb_search, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_kb_search, keyboard_input, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_pad_all(s_kb_search, 4, 0);
    lv_obj_set_style_pad_row(s_kb_search, 4, 0);
    lv_obj_set_style_pad_column(s_kb_search, 4, 0);
    lv_keyboard_set_mode(s_kb_search, LV_KEYBOARD_MODE_USER_1);
    lv_obj_set_style_text_font(s_kb_search, &font_inter_18, 0);
    lv_obj_set_style_bg_color(s_kb_search, ROCK_COLOR_PANEL, 0);
    lv_obj_add_event_cb(s_kb_search, on_kb_event, LV_EVENT_READY, NULL);
}

static void build_overlay(lv_obj_t *parent)
{
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, 800, 326);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, ROCK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 520, 260);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 12, 0);

    s_overlay_spinner = lv_spinner_create(card);
    lv_obj_set_size(s_overlay_spinner, 48, 48);
    lv_obj_set_style_arc_color(s_overlay_spinner, ROCK_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_overlay_spinner, ROCK_COLOR_PANEL_2, LV_PART_MAIN);

    s_overlay_icon = lv_label_create(card);
    lv_label_set_text(s_overlay_icon, ROCK_ICON_BOLT);
    lv_obj_set_style_text_font(s_overlay_icon, &font_inter_24, 0);
    lv_obj_set_style_text_color(s_overlay_icon, ROCK_COLOR_WARN, 0);
    lv_obj_add_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);

    s_overlay_title = lv_label_create(card);
    lv_label_set_text(s_overlay_title, "Загрузка...");
    lv_obj_set_style_text_font(s_overlay_title, &font_inter_18, 0);
    lv_obj_set_style_text_color(s_overlay_title, ROCK_COLOR_FG, 0);

    s_overlay_desc = lv_label_create(card);
    lv_label_set_text(s_overlay_desc, "Пожалуйста, подождите");
    lv_obj_set_style_text_font(s_overlay_desc, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_overlay_desc, ROCK_COLOR_MUTED, 0);

    lv_obj_t *btn_box = lv_obj_create(card);
    lv_obj_remove_style_all(btn_box);
    lv_obj_set_size(btn_box, LV_SIZE_CONTENT, 48);
    lv_obj_set_flex_flow(btn_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_box, 16, 0);

    s_overlay_btn1 = lv_button_create(btn_box);
    lv_obj_set_size(s_overlay_btn1, 160, 44);
    lv_obj_set_style_bg_color(s_overlay_btn1, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_overlay_btn1, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_overlay_btn1, on_overlay_retry_click, LV_EVENT_CLICKED, NULL);

    s_overlay_lbl1 = lv_label_create(s_overlay_btn1);
    lv_label_set_text(s_overlay_lbl1, "Повторить");
    lv_obj_set_style_text_font(s_overlay_lbl1, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_overlay_lbl1, ROCK_COLOR_FG, 0);
    lv_obj_center(s_overlay_lbl1);

    s_overlay_btn2 = lv_button_create(btn_box);
    lv_obj_set_size(s_overlay_btn2, 160, 44);
    lv_obj_set_style_bg_color(s_overlay_btn2, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_overlay_btn2, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_overlay_btn2, on_overlay_back_click, LV_EVENT_CLICKED, NULL);

    s_overlay_lbl2 = lv_label_create(s_overlay_btn2);
    lv_label_set_text(s_overlay_lbl2, "Назад");
    lv_obj_set_style_text_font(s_overlay_lbl2, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_overlay_lbl2, ROCK_COLOR_WHITE, 0);
    lv_obj_center(s_overlay_lbl2);

    lv_obj_add_flag(btn_box, LV_OBJ_FLAG_HIDDEN);
}

static void build_cast_overlay(lv_obj_t *parent)
{
    s_cast_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_cast_overlay);
    lv_obj_set_size(s_cast_overlay, 800, 326);
    lv_obj_set_pos(s_cast_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_cast_overlay, ROCK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_cast_overlay, LV_OPA_90, 0);
    lv_obj_add_flag(s_cast_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_cast_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 580, 290);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);

    // Top Header: Title, Refresh, Close
    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 550, 36);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Выход звука");
    lv_obj_set_style_text_font(title, &font_inter_18, 0);
    lv_obj_set_style_text_color(title, ROCK_COLOR_FG, 0);

    lv_obj_t *sp = lv_obj_create(hdr);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    s_btn_cast_refresh = lv_button_create(hdr);
    lv_obj_set_size(s_btn_cast_refresh, 100, 32);
    lv_obj_set_style_bg_color(s_btn_cast_refresh, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_btn_cast_refresh, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_cast_refresh, on_cast_refresh_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ref = lv_label_create(s_btn_cast_refresh);
    lv_label_set_text(lbl_ref, "Обновить");
    lv_obj_set_style_text_font(lbl_ref, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_ref, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl_ref);

    lv_obj_t *btn_close = lv_button_create(hdr);
    lv_obj_set_size(btn_close, 36, 32);
    lv_obj_set_style_bg_color(btn_close, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(btn_close, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(btn_close, on_cast_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cls = lv_label_create(btn_close);
    lv_label_set_text(lbl_cls, "X");
    lv_obj_set_style_text_font(lbl_cls, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_cls, ROCK_COLOR_MUTED, 0);
    lv_obj_center(lbl_cls);

    // Scrollable List container
    s_cast_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_cast_list);
    lv_obj_set_size(s_cast_list, 550, 210);
    lv_obj_set_flex_flow(s_cast_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cast_list, 6, 0);

    // Row 0: Local speaker
    s_cast_local_btn = lv_button_create(s_cast_list);
    lv_obj_set_size(s_cast_local_btn, 540, 42);
    lv_obj_set_style_bg_color(s_cast_local_btn, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(s_cast_local_btn, 6, 0);
    lv_obj_set_style_pad_left(s_cast_local_btn, 12, 0);
    lv_obj_set_style_pad_right(s_cast_local_btn, 12, 0);
    lv_obj_set_flex_flow(s_cast_local_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cast_local_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_cast_local_btn, on_cast_local_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_loc = lv_label_create(s_cast_local_btn);
    lv_label_set_text(lbl_loc, "Динамик радио (локальный)");
    lv_obj_set_style_text_font(lbl_loc, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_loc, ROCK_COLOR_FG, 0);

    lv_obj_t *loc_sp = lv_obj_create(s_cast_local_btn);
    lv_obj_remove_style_all(loc_sp);
    lv_obj_set_flex_grow(loc_sp, 1);

    s_cast_local_badge = lv_label_create(s_cast_local_btn);
    lv_label_set_text(s_cast_local_badge, "[Активен]");
    lv_obj_set_style_text_font(s_cast_local_badge, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_cast_local_badge, ROCK_COLOR_ACCENT, 0);

    // Status box (spinner + label)
    lv_obj_t *status_box = lv_obj_create(s_cast_list);
    lv_obj_remove_style_all(status_box);
    lv_obj_set_size(status_box, 540, 36);
    lv_obj_set_flex_flow(status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(status_box, 12, 0);
    lv_obj_set_style_pad_column(status_box, 10, 0);

    s_cast_spinner = lv_spinner_create(status_box);
    lv_obj_set_size(s_cast_spinner, 24, 24);
    lv_obj_set_style_arc_color(s_cast_spinner, ROCK_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_cast_spinner, ROCK_COLOR_PANEL_2, LV_PART_MAIN);

    s_cast_status_lbl = lv_label_create(status_box);
    lv_label_set_text(s_cast_status_lbl, "Поиск устройств Chromecast...");
    lv_obj_set_style_text_font(s_cast_status_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_cast_status_lbl, ROCK_COLOR_MUTED, 0);

    // Pre-create 16 device rows
    for (size_t i = 0; i < ROCK_GUI_MAX_CAST_DEVICES; i++) {
        lv_obj_t *btn = lv_button_create(s_cast_list);
        lv_obj_set_size(btn, 540, 42);
        lv_obj_set_style_bg_color(btn, ROCK_COLOR_PANEL_2, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_left(btn, 12, 0);
        lv_obj_set_style_pad_right(btn, 12, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(btn, on_cast_device_row_click, LV_EVENT_CLICKED, &s_cast_rows[i]);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *lbl_name = lv_label_create(btn);
        lv_label_set_text(lbl_name, "");
        lv_obj_set_style_text_font(lbl_name, &font_inter_14, 0);
        lv_obj_set_style_text_color(lbl_name, ROCK_COLOR_FG, 0);

        lv_obj_t *dsp = lv_obj_create(btn);
        lv_obj_remove_style_all(dsp);
        lv_obj_set_flex_grow(dsp, 1);

        lv_obj_t *lbl_badge = lv_label_create(btn);
        lv_label_set_text(lbl_badge, "[Играет]");
        lv_obj_set_style_text_font(lbl_badge, &font_inter_14, 0);
        lv_obj_set_style_text_color(lbl_badge, ROCK_COLOR_ACCENT, 0);
        lv_obj_add_flag(lbl_badge, LV_OBJ_FLAG_HIDDEN);

        s_cast_rows[i].btn = btn;
        s_cast_rows[i].lbl_name = lbl_name;
        s_cast_rows[i].lbl_badge = lbl_badge;
        memset(s_cast_rows[i].receiver_id, 0, sizeof(s_cast_rows[i].receiver_id));
    }
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

    build_header(s_screen);
    build_ticker(s_screen);

    s_viewport = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_viewport);
    lv_obj_set_size(s_viewport, 800, 404);
    lv_obj_set_pos(s_viewport, 0, ROCK_HEADER_HEIGHT);

    build_now_playing_view(s_viewport);
    build_browse_view(s_viewport);
    build_search_view(s_viewport);
    build_overlay(s_viewport);
    build_cast_overlay(s_viewport);

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

    switch_view(VIEW_NOW_PLAYING);

    xTaskCreatePinnedToCore(serial_cmd_task, "gui_cmd", 32768, NULL, 3, NULL, 0);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "RockCast native station GUI started successfully");
    return 0;
}

bool rock_gui_poll_event(rock_ui_event_t *out_event, uint32_t timeout_ms)
{
    if (!s_ui_event_queue || !out_event) return false;
    return xQueueReceive(s_ui_event_queue, out_event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void rock_gui_update_playback(rock_player_status_t status, const char *station_id,
                              const char *title, const char *subtitle,
                              uint8_t volume, bool muted)
{
    if (!lvgl_port_lock(500)) return;

    s_playback_status = status;
    s_volume_level = volume;
    s_volume_muted = muted;

    if (station_id && station_id[0]) {
        if (strcmp(s_station_id, station_id) != 0) {
            const char *cur = lv_label_get_text(s_lbl_np_station);
            if (!cur || cur[0] == '\0') {
                lv_label_set_text(s_lbl_np_station, station_id);
            }
        }
        snprintf(s_station_id, sizeof(s_station_id), "%s", station_id);
    }
    if (title && title[0]) {
        lv_label_set_text(s_lbl_np_station, title);
    }
    if (subtitle && subtitle[0]) {
        lv_label_set_text(s_lbl_np_subtitle, subtitle);
    }

    if (s_lbl_play_pause) {
        if (status == ROCK_PLAYER_STATUS_PLAYING) {
            lv_label_set_text(s_lbl_play_pause, ROCK_ICON_PAUSE);
        } else {
            lv_label_set_text(s_lbl_play_pause, ROCK_ICON_PLAY);
        }
    }
    if (s_lbl_vol_val) {
        if (muted) {
            lv_label_set_text(s_lbl_vol_val, " MUTE ");
        } else {
            lv_label_set_text_fmt(s_lbl_vol_val, " %u%% ", volume);
        }
    }
    if (s_lbl_ticker) {
        switch (status) {
            case ROCK_PLAYER_STATUS_PLAYING:
                lv_label_set_text_fmt(s_lbl_ticker, "Воспроизведение: %s", lv_label_get_text(s_lbl_np_station));
                break;
            case ROCK_PLAYER_STATUS_BUFFERING:
                lv_label_set_text(s_lbl_ticker, "Буферизация потока станции...");
                break;
            case ROCK_PLAYER_STATUS_PAUSED:
                lv_label_set_text(s_lbl_ticker, "Пауза");
                break;
            case ROCK_PLAYER_STATUS_STOPPED:
                lv_label_set_text(s_lbl_ticker, "Воспроизведение остановлено");
                break;
            case ROCK_PLAYER_STATUS_ERROR:
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

    size_t limit = count > ROCK_GUI_MAX_STATIONS ? ROCK_GUI_MAX_STATIONS : count;
    s_browse_rows_count = limit;
    s_catalog_stale = false;

    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        station_row_ui_t *row_ui = &s_browse_rows[i];
        if (i < limit && items) {
            strncpy(row_ui->station_id, items[i].id, sizeof(row_ui->station_id) - 1);
            lv_label_set_text(row_ui->name_lbl, items[i].name[0] ? items[i].name : items[i].id);

            char initial[4] = "R";
            if (items[i].name[0]) {
                size_t n = ((uint8_t)items[i].name[0] < 0x80) ? 1 : (((uint8_t)items[i].name[0] < 0xE0) ? 2 : 3);
                memcpy(initial, items[i].name, n);
                initial[n] = '\0';
            }
            lv_label_set_text(row_ui->icon_lbl, initial);

            char sub[128];
            snprintf(sub, sizeof(sub), "%s", items[i].tags[0] ? items[i].tags : "Radio");
            lv_label_set_text(row_ui->desc_lbl, sub);

            char cdc[32];
            if (items[i].bitrate_kbps > 0) {
                snprintf(cdc, sizeof(cdc), "%luk %s", (unsigned long)items[i].bitrate_kbps,
                         items[i].codec[0] ? items[i].codec : "MP3");
            } else {
                snprintf(cdc, sizeof(cdc), "%s", items[i].codec[0] ? items[i].codec : "MP3");
            }
            lv_label_set_text(row_ui->codec_lbl, cdc);

            lv_obj_remove_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            row_ui->station_id[0] = '\0';
            lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    rock_gui_dismiss_state();
    switch_view(VIEW_BROWSE);
    if (count == 0) rock_gui_show_empty("Каталог пуст");
    lvgl_port_unlock();
}

void rock_gui_set_search_results(const rock_gui_station_item_t *items, size_t count)
{
    if (!lvgl_port_lock(1000)) return;

    size_t limit = count > ROCK_GUI_MAX_STATIONS ? ROCK_GUI_MAX_STATIONS : count;
    s_search_rows_count = limit;

    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        station_row_ui_t *row_ui = &s_search_rows[i];
        if (i < limit && items) {
            strncpy(row_ui->station_id, items[i].id, sizeof(row_ui->station_id) - 1);
            lv_label_set_text(row_ui->name_lbl, items[i].name[0] ? items[i].name : items[i].id);
            ESP_LOGI(TAG, "Search row [%d]: id='%s' name='%s'", (int)i, items[i].id, items[i].name);

            char initial[4] = "R";
            if (items[i].name[0]) {
                size_t n = ((uint8_t)items[i].name[0] < 0x80) ? 1 : (((uint8_t)items[i].name[0] < 0xE0) ? 2 : 3);
                memcpy(initial, items[i].name, n);
                initial[n] = '\0';
            }
            lv_label_set_text(row_ui->icon_lbl, initial);

            char sub[128];
            snprintf(sub, sizeof(sub), "%s", items[i].tags[0] ? items[i].tags : "Radio");
            lv_label_set_text(row_ui->desc_lbl, sub);

            char cdc[32];
            if (items[i].bitrate_kbps > 0) {
                snprintf(cdc, sizeof(cdc), "%luk %s", (unsigned long)items[i].bitrate_kbps,
                         items[i].codec[0] ? items[i].codec : "MP3");
            } else {
                snprintf(cdc, sizeof(cdc), "%s", items[i].codec[0] ? items[i].codec : "MP3");
            }
            lv_label_set_text(row_ui->codec_lbl, cdc);

            lv_obj_remove_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            row_ui->station_id[0] = '\0';
            lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_catalog_stale = false;
    if (s_kb_search) lv_obj_add_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
    if (s_list_search) lv_obj_remove_flag(s_list_search, LV_OBJ_FLAG_HIDDEN);

    rock_gui_dismiss_state();
    switch_view(VIEW_SEARCH);
    lvgl_port_unlock();
}

void rock_gui_show_loading(const char *message)
{
    if (!s_overlay || !lvgl_port_lock(500)) return;

    if (s_overlay_spinner) lv_obj_remove_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_icon) lv_obj_add_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_title) lv_label_set_text(s_overlay_title, message ? message : "Загрузка...");
    if (s_overlay_desc) lv_label_set_text(s_overlay_desc, "Пожалуйста, подождите");
    if (s_overlay_btn1) lv_obj_add_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_show_empty(const char *message)
{
    if (!s_overlay || !lvgl_port_lock(500)) return;

    if (s_overlay_spinner) lv_obj_add_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_icon) lv_obj_add_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_title) lv_label_set_text(s_overlay_title, message ? message : "Ничего не найдено");
    if (s_overlay_desc) lv_label_set_text(s_overlay_desc, "Попробуйте изменить поисковый запрос");
    if (s_overlay_btn1) lv_obj_remove_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_show_offline(const char *message)
{
    if (!s_overlay || !lvgl_port_lock(500)) return;

    if (s_overlay_spinner) lv_obj_add_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_icon) {
        lv_label_set_text(s_overlay_icon, ROCK_ICON_BOLT);
        lv_obj_set_style_text_color(s_overlay_icon, ROCK_COLOR_WARN, 0);
        lv_obj_remove_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_overlay_title) lv_label_set_text(s_overlay_title, message ? message : "Сервер недоступен");
    if (s_overlay_desc) lv_label_set_text(s_overlay_desc, "Проверьте сеть. Управление плеером доступно.");
    if (s_overlay_btn1) lv_obj_remove_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_show_error(const char *message)
{
    if (!s_overlay || !lvgl_port_lock(500)) return;

    if (s_overlay_spinner) lv_obj_add_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_icon) {
        lv_label_set_text(s_overlay_icon, ROCK_ICON_CROSS);
        lv_obj_set_style_text_color(s_overlay_icon, ROCK_COLOR_ERROR, 0);
        lv_obj_remove_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_overlay_title) lv_label_set_text(s_overlay_title, "Ошибка воспроизведения");
    if (s_overlay_desc) lv_label_set_text(s_overlay_desc, message ? message : "Поток станции временно недоступен");
    if (s_overlay_btn1) lv_obj_remove_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_dismiss_state(void)
{
    if (!s_overlay || !lvgl_port_lock(500)) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_show_cast_menu(bool scanning)
{
    if (!s_cast_overlay || !lvgl_port_lock(500)) return;

    if (scanning) {
        if (s_cast_spinner) lv_obj_remove_flag(s_cast_spinner, LV_OBJ_FLAG_HIDDEN);
        if (s_cast_status_lbl) {
            lv_label_set_text(s_cast_status_lbl, "Поиск устройств Chromecast...");
            lv_obj_remove_flag(s_cast_status_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_cast_spinner) lv_obj_add_flag(s_cast_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_remove_flag(s_cast_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_set_cast_devices(const rock_gui_cast_item_t *items, size_t count)
{
    if (!lvgl_port_lock(500)) return;

    if (s_cast_spinner) lv_obj_add_flag(s_cast_spinner, LV_OBJ_FLAG_HIDDEN);

    if (count == 0) {
        if (s_cast_status_lbl) {
            lv_label_set_text(s_cast_status_lbl, "Устройства Chromecast не найдены");
            lv_obj_remove_flag(s_cast_status_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_cast_status_lbl) {
            lv_obj_add_flag(s_cast_status_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_cast_rows_count = count > ROCK_GUI_MAX_CAST_DEVICES ? ROCK_GUI_MAX_CAST_DEVICES : count;
    for (size_t i = 0; i < ROCK_GUI_MAX_CAST_DEVICES; i++) {
        if (i < s_cast_rows_count && items) {
            strncpy(s_cast_rows[i].receiver_id, items[i].receiver_id, sizeof(s_cast_rows[i].receiver_id) - 1);
            s_cast_rows[i].receiver_id[sizeof(s_cast_rows[i].receiver_id) - 1] = '\0';
            if (s_cast_rows[i].lbl_name) {
                lv_label_set_text(s_cast_rows[i].lbl_name, items[i].display_name);
            }
            if (s_cast_rows[i].lbl_badge) {
                if (items[i].is_active) {
                    lv_obj_remove_flag(s_cast_rows[i].lbl_badge, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_cast_rows[i].lbl_badge, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_cast_rows[i].btn) {
                lv_obj_remove_flag(s_cast_rows[i].btn, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (s_cast_rows[i].btn) {
                lv_obj_add_flag(s_cast_rows[i].btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    lvgl_port_unlock();
}

void rock_gui_dismiss_cast_menu(void)
{
    if (!s_cast_overlay || !lvgl_port_lock(500)) return;
    lv_obj_add_flag(s_cast_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void rock_gui_update_cast_status(bool is_casting, const char *receiver_name)
{
    if (!lvgl_port_lock(500)) return;

    s_is_casting = is_casting;
    if (receiver_name && receiver_name[0]) {
        strncpy(s_cast_receiver_name, receiver_name, sizeof(s_cast_receiver_name) - 1);
    } else {
        s_cast_receiver_name[0] = '\0';
    }

    if (s_btn_cast) {
        if (is_casting) {
            lv_obj_set_style_bg_color(s_btn_cast, ROCK_COLOR_ACCENT, 0);
            if (s_lbl_cast) lv_obj_set_style_text_color(s_lbl_cast, ROCK_COLOR_WHITE, 0);
        } else {
            lv_obj_set_style_bg_color(s_btn_cast, ROCK_COLOR_PANEL_2, 0);
            if (s_lbl_cast) lv_obj_set_style_text_color(s_lbl_cast, ROCK_COLOR_FG, 0);
        }
    }

    if (s_lbl_np_badge) {
        if (is_casting && s_cast_receiver_name[0]) {
            lv_label_set_text_fmt(s_lbl_np_badge, ROCK_ICON_PLAY " %s", s_cast_receiver_name);
        } else {
            lv_label_set_text(s_lbl_np_badge, "СЕЙЧАС ИГРАЕТ");
        }
    }

    if (s_cast_local_badge) {
        if (is_casting) {
            lv_obj_add_flag(s_cast_local_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_cast_local_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lvgl_port_unlock();
}

void rock_gui_mark_catalog_stale(void)
{
    if (!lvgl_port_lock(500)) return;

    s_catalog_stale = true;
    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        lv_obj_add_flag(s_browse_rows[i].row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_search_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_active_view == VIEW_BROWSE || s_active_view == VIEW_SEARCH) {
        rock_gui_show_offline("Связь с сервером потеряна");
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
            snprintf(s_station_id, sizeof(s_station_id), "%s", station_id);
            lv_label_set_text(s_lbl_np_station, title ? title : station_id);
            lv_label_set_text(s_lbl_np_subtitle, subtitle ? subtitle : "");
        } else if (subtitle && *subtitle) {
            lv_label_set_text(s_overlay_title, title ? title : "");
            lv_label_set_text(s_overlay_desc, subtitle);
            lv_obj_add_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
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
    switch (view_index) {
        case 0:
            switch_view(VIEW_NOW_PLAYING);
            break;
        case 1:
            switch_view(VIEW_BROWSE);
            break;
        case 2:
            switch_view(VIEW_SEARCH);
            break;
        default:
            break;
    }
    lvgl_port_unlock();
}

typedef struct {
    uint8_t octets[3];
    size_t octet_count;
    char line_buf[128];
    size_t line_pos;
} b64_stream_t;

static void b64_emit_byte(b64_stream_t *s, uint8_t b)
{
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    s->octets[s->octet_count++] = b;
    if (s->octet_count == 3) {
        uint32_t triple = (s->octets[0] << 16) | (s->octets[1] << 8) | s->octets[2];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 18) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 12) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 6) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[triple & 0x3F];
        s->octet_count = 0;
        if (s->line_pos >= 120) {
            s->line_buf[s->line_pos++] = '\n';
            s->line_buf[s->line_pos] = '\0';
            fputs(s->line_buf, stdout);
            s->line_pos = 0;
            vTaskDelay(1);
        }
    }
}

static void b64_flush(b64_stream_t *s)
{
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (s->octet_count > 0) {
        uint32_t a = s->octets[0];
        uint32_t b = (s->octet_count >= 2) ? s->octets[1] : 0;
        uint32_t triple = (a << 16) | (b << 8);
        s->line_buf[s->line_pos++] = b64_table[(triple >> 18) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 12) & 0x3F];
        s->line_buf[s->line_pos++] = (s->octet_count >= 2) ? b64_table[(triple >> 6) & 0x3F] : '=';
        s->line_buf[s->line_pos++] = '=';
        s->octet_count = 0;
    }
    if (s->line_pos > 0) {
        s->line_buf[s->line_pos++] = '\n';
        s->line_buf[s->line_pos] = '\0';
        fputs(s->line_buf, stdout);
        s->line_pos = 0;
        vTaskDelay(1);
    }
    fflush(stdout);
}

int rock_gui_dump_screenshot(void)
{
    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL for screenshot");
        return -1;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "No active screen");
        return -1;
    }

    lv_obj_update_layout(screen);
    lv_draw_buf_t *draw_buf = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();

    if (!draw_buf) {
        ESP_LOGE(TAG, "lv_snapshot_take failed");
        return -1;
    }

    uint32_t w = draw_buf->header.w;
    uint32_t h = draw_buf->header.h;
    uint32_t stride = draw_buf->header.stride;
    uint32_t data_size = draw_buf->data_size;
    uint32_t total_pixels = w * h;

    esp_log_level_set("*", ESP_LOG_NONE);

    printf("\n===SCREENSHOT_START:w=%u,h=%u,stride=%u,cf=RGB565_RLE,len=%u===\n",
           (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)data_size);
    fflush(stdout);

    b64_stream_t stream = {0};
    uint32_t idx = 0;

    while (idx < total_pixels) {
        uint16_t cur = *(uint16_t *)(draw_buf->data + (idx / w) * stride + (idx % w) * 2);
        uint32_t run = 1;
        while (idx + run < total_pixels && *(uint16_t *)(draw_buf->data + ((idx + run) / w) * stride + ((idx + run) % w) * 2) == cur && run < 255) {
            run++;
        }
        b64_emit_byte(&stream, (uint8_t)run);
        b64_emit_byte(&stream, (uint8_t)(cur & 0xFF));
        b64_emit_byte(&stream, (uint8_t)((cur >> 8) & 0xFF));
        idx += run;
    }
    b64_flush(&stream);

    printf("===SCREENSHOT_END===\n");
    fflush(stdout);

    esp_log_level_set("*", ESP_LOG_INFO);

    lv_draw_buf_destroy(draw_buf);
    return 0;
}

static void serial_cmd_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t jtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&jtag_cfg);
    bool jtag_installed = (err == ESP_OK || err == ESP_ERR_INVALID_STATE);

    char cmd[544];
    size_t cmd_idx = 0;

    ESP_LOGI(TAG, "Serial command listener active (USB-JTAG: %d)", (int)jtag_installed);

    while (1) {

        uint8_t ch = 0;
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(50));
        if (n > 0) {
            if (ch == '\n' || ch == '\r') {
                if (cmd_idx > 0) {
                    cmd[cmd_idx] = '\0';
                    ESP_LOGI(TAG, "Serial command received: '%s'", cmd);
                    if (strcmp(cmd, "CMD:SELFTEST") == 0) {
#if CONFIG_ROCK_BRINGUP_TESTS
                        static bool ran;
                        char state[16];
                        rock_player_get_state(state, sizeof(state), NULL, 0, NULL, NULL);
                        if (!ran && (!strcmp(state, "idle") || !strcmp(state, "stopped"))) {
                            ran = rock_player_bringup_start() == 0;
                        }
#else
                        ESP_LOGI(TAG, "SELFTEST disabled (build with CONFIG_ROCK_BRINGUP_TESTS=y)");
#endif
                    } else if (!strncmp(cmd, "CMD:UI:", 7)) {
                        if (lvgl_port_lock(500)) {
                            const char *action = cmd + 7;
                            if (!strcmp(action, "CATALOG")) lv_obj_send_event(s_btn_browse, LV_EVENT_CLICKED, NULL);
                            else if (!strcmp(action, "SEARCH")) { on_search_edit(NULL); lv_obj_send_event(s_btn_search, LV_EVENT_CLICKED, NULL); }
                            else if (!strncmp(action, "SEARCH:", 7)) {
                                switch_view(VIEW_SEARCH);
                                lv_textarea_set_text(s_ta_search, action + 7);
                                lv_obj_send_event(s_kb_search, LV_EVENT_READY, NULL);
                            } else if (!strcmp(action, "PLAYPAUSE")) lv_obj_send_event(s_btn_play_pause, LV_EVENT_CLICKED, NULL);
                            else if (!strcmp(action, "STOP")) lv_obj_send_event(s_btn_stop, LV_EVENT_CLICKED, NULL);
                            else if (!strcmp(action, "VOL+")) lv_obj_send_event(s_btn_vol_up, LV_EVENT_CLICKED, NULL);
                            else if (!strcmp(action, "VOL-")) lv_obj_send_event(s_btn_vol_down, LV_EVENT_CLICKED, NULL);
                            else if (!strcmp(action, "RETRY")) lv_obj_send_event(s_overlay_btn1, LV_EVENT_CLICKED, NULL);
                            else if (!strcmp(action, "BACK")) lv_obj_send_event(s_overlay_btn2, LV_EVENT_CLICKED, NULL);
                            else if (!strncmp(action, "STATIONID:", 10)) {
                                const char *station_id = action + 10;
                                if (*station_id && strlen(station_id) <= 128 &&
                                    strspn(station_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~") == strlen(station_id)) {
                                    ESP_LOGI(TAG, "Serial trigger station id='%s'", station_id);
                                    enqueue_event(ROCK_UI_EVT_PLAY_STATION, station_id, 0);
                                }
                            }
                            else if (!strncmp(action, "STATION:", 8)) {
                                int row = atoi(action + 8);
                                station_row_ui_t *rows = s_active_view == VIEW_SEARCH ? s_search_rows : s_browse_rows;
                                if (row >= 0 && row < ROCK_GUI_MAX_STATIONS && !s_catalog_stale &&
                                    !lv_obj_has_flag(rows[row].row, LV_OBJ_FLAG_HIDDEN)) {
                                    ESP_LOGI(TAG, "Serial trigger station row=%d id='%s'", row, rows[row].station_id);
                                    lv_obj_send_event(rows[row].row, LV_EVENT_CLICKED, NULL);
                                } else {
                                    ESP_LOGW(TAG, "Serial trigger station row=%d invalid (stale=%d hidden=%d)",
                                             row, s_catalog_stale,
                                             (row >= 0 && row < ROCK_GUI_MAX_STATIONS) ? lv_obj_has_flag(rows[row].row, LV_OBJ_FLAG_HIDDEN) : -1);
                                }
                            } else if (!strcmp(action, "CAST")) {
                                if (s_btn_cast) lv_obj_send_event(s_btn_cast, LV_EVENT_CLICKED, NULL);
                            } else if (!strncmp(action, "CAST:SELECT:", 12)) {
                                int row = atoi(action + 12);
                                if (row >= 0 && row < ROCK_GUI_MAX_CAST_DEVICES && s_cast_rows[row].btn &&
                                    !lv_obj_has_flag(s_cast_rows[row].btn, LV_OBJ_FLAG_HIDDEN)) {
                                    lv_obj_send_event(s_cast_rows[row].btn, LV_EVENT_CLICKED, NULL);
                                }
                            } else if (!strcmp(action, "CAST:LOCAL")) {
                                if (s_cast_local_btn) lv_obj_send_event(s_cast_local_btn, LV_EVENT_CLICKED, NULL);
                            }
                            lvgl_port_unlock();
                        }
                    } else if (strcmp(cmd, "CMD:METRICS") == 0) {
                        uint32_t underruns, errors, reconnects;
                        rock_player_get_stats(&underruns, &errors, &reconnects);
                        extern int rock_wifi_rssi(void);
                        extern int rock_wifi_channel(void);
                        wifi_ps_type_t ps_now = WIFI_PS_NONE;
                        esp_err_t ps_err = esp_wifi_get_ps(&ps_now);
                        ESP_LOGI(TAG, "metrics heap=%u internal=%u underruns=%lu decode_errors=%lu reconnects=%lu tasks=%u timer_hz=%lu rssi=%d ch=%d ps=%d ps_err=%d",
                            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                            (unsigned long)underruns, (unsigned long)errors, (unsigned long)reconnects,
                            (unsigned)uxTaskGetNumberOfTasks(), (unsigned long)s_fps, rock_wifi_rssi(),
                            rock_wifi_channel(), (int)ps_now, (int)ps_err);
                        rock_player_log_stream_metrics();
                        rock_spectrum_log_metrics();
                        ESP_LOGI(TAG, "ui view=%d browse=%u search=%u stale=%d playback=%d volume=%u",
                                 s_active_view, (unsigned)s_browse_rows_count,
                                 (unsigned)s_search_rows_count, s_catalog_stale,
                                 s_playback_status, s_volume_level);
                    } else if (strcmp(cmd, "CMD:SCREENSHOT") == 0) {
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:NP") == 0) {
                        rock_gui_switch_view(0);
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:BROWSE") == 0) {
                        rock_gui_switch_view(1);
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:SEARCH") == 0) {
                        rock_gui_switch_view(2);
                        rock_gui_dump_screenshot();
                    }
                    cmd_idx = 0;
                }
            } else if (cmd_idx + 1 < sizeof(cmd)) {
                cmd[cmd_idx++] = (char)ch;
            }
        }
    }
}
