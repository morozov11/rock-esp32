#pragma once

// Private plumbing of the GUI module after the phase-4 split. rock_gui.c is
// the facade (shell widgets, UI state, event queue, view registry, public
// API); view/widget files own their static widgets. NOT a public header:
// the Rust-bound ABI is rock_gui.h and stays unchanged.
//
// Locking contract: the gui_* functions here are RAW — callers (the public
// rock_gui_* wrappers in rock_gui.c) hold the LVGL port lock. Exception:
// LVGL event callbacks run inside the LVGL task which already holds the
// (recursive) lock and may call the public wrappers like the old code did.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "rock_gui.h"

typedef enum {
    VIEW_NOW_PLAYING = 0,
    VIEW_CATALOG
} rock_gui_view_mode_t;

// ---- shell & state (rock_gui.c) ----

void gui_enqueue_event(rock_ui_event_type_t type, const char *text, int16_t val);
void gui_switch_view(rock_gui_view_mode_t view);
rock_gui_view_mode_t gui_active_view(void);
bool gui_catalog_stale(void);
rock_gui_playback_t gui_playback_status(void);
void gui_set_station_id(const char *station_id);
const char *gui_station_id(void);
lv_obj_t *gui_screen(void);            // shell root; the transport bar lives on it
lv_obj_t *gui_ticker_label(void);

// ---- shared widgets (rock_gui_widgets.c) ----

// Button + centered label boilerplate (former ~20 hand-rolled repeats):
// PANEL_2 background, ROCK_RADIUS_BUTTON radius, ROCK_COLOR_FG label.
// Deviating styles are applied by the caller on the returned button
// (label handle: lv_obj_get_child(btn, 0)).
lv_obj_t *gui_make_button(lv_obj_t *parent, int32_t w, int32_t h, const char *label,
                          const lv_font_t *font, lv_event_cb_t cb, void *user_data);

// Shared "back to Now Playing" header button callback (catalog view).
void gui_on_back_to_np_click(lv_event_t *e);

// Station monogram: deterministic fill from rock_ui_station_color() plus the
// first 1-2 alphanumeric initials of the name (rock_ui_tokens.h palette).
void gui_monogram_apply(lv_obj_t *box, lv_obj_t *lbl, const char *name);

// One station list row (shared by Browse and Search views).
typedef struct {
    lv_obj_t *row;
    lv_obj_t *icon_box;
    lv_obj_t *icon_lbl;
    lv_obj_t *name_lbl;
    lv_obj_t *desc_lbl;
    lv_obj_t *codec_lbl;
    lv_obj_t *play_lbl;
    lv_obj_t *eq_box;      // mini equalizer shown in the ACCENT "playing" row
    char station_id[129];
} station_row_ui_t;

// Build one hidden row (alternating background, whole-row click → play).
void gui_station_row_setup(station_row_ui_t *row_ui, lv_obj_t *parent, int index);

// Fill rows[0..ROCK_GUI_MAX_STATIONS) from items: first `limit` visible,
// the rest hidden. log_rows keeps the search-results debug logging.
void gui_station_rows_fill(station_row_ui_t *rows, const rock_gui_station_item_t *items,
                           size_t limit, bool log_rows);

// Modal state overlay: the former show_loading/empty/offline/error quartet
// unified into one entry point. title/desc fall back to per-kind defaults
// when NULL (error keeps its fixed title — message goes to desc).
typedef enum {
    GUI_OVERLAY_LOADING,
    GUI_OVERLAY_EMPTY,
    GUI_OVERLAY_OFFLINE,
    GUI_OVERLAY_ERROR,
} gui_overlay_kind_t;

void gui_overlay_build(lv_obj_t *parent);
void gui_overlay_set(gui_overlay_kind_t kind, const char *title, const char *desc,
                     bool show_buttons);
void gui_overlay_hide(void);
bool gui_overlay_trigger(const char *action);   // RETRY / BACK

// ---- now playing view + transport (rock_gui_views_now_playing.c) ----

void gui_now_playing_build(lv_obj_t *parent);
void gui_now_playing_show(void);
void gui_now_playing_hide(void);
// Row-click path: unconditionally mirror the picked row (station/desc/codec
// labels + empty track line).
void gui_now_playing_set_station(const char *name, const char *desc, const char *codec);
// rock_gui_update_playback path: conditional label updates keyed on the
// shell's station_id.
void gui_now_playing_update_meta(const char *station_id, const char *station_name,
                                const char *title, const char *subtitle);
void gui_now_playing_update_transport(rock_gui_playback_t status, uint8_t volume, bool muted);
void gui_now_playing_apply_cast_state(bool is_casting, const char *receiver_name);
void gui_now_playing_spectrum_update(const float *bands, bool playing);
void gui_now_playing_present(const char *station_id, const char *title, const char *subtitle);
const char *gui_now_playing_station_text(void);
bool gui_now_playing_trigger(const char *action);   // CATALOG, PLAYPAUSE, STOP, VOL+, VOL-

// ---- catalog view (rock_gui_views_catalog.c) ----
#define ROCK_GUI_CATALOG_ROW_STRIDE_Y     (ROCK_LIST_ROW_HEIGHT + 4) // 56px row + 4px flex gap
#define ROCK_GUI_CATALOG_SCROLL_THRESHOLD 20                         // px from edge to trigger window slide / fetch

void gui_catalog_build(lv_obj_t *parent);
void gui_catalog_show(void);
void gui_catalog_hide(void);
void gui_catalog_begin(const rock_gui_station_item_t *items, size_t count, const char *next_cursor);
void gui_catalog_append(const rock_gui_station_item_t *items, size_t count, const char *next_cursor);
void gui_catalog_clear(void);
void gui_catalog_reset_fetching(void);
// Fill the rows: catalog pages and search results share the list.
// search_results=false — catalog page (counter update, "Каталог пуст" on 0);
// search_results=true — search results (keyboard down, list up, row logging).
void gui_catalog_set_rows(const rock_gui_station_item_t *items, size_t count, bool search_results);
void gui_catalog_hide_rows(void);
void gui_catalog_begin_edit(void);                  // show keyboard, hide list
station_row_ui_t *gui_catalog_rows(void);
size_t gui_catalog_rows_count(void);
bool gui_catalog_trigger(const char *action);       // SEARCH, SEARCH:<query>, LOADMORE

// ---- virtual keyboard (rock_gui_keyboard.c) ----

// Install the RU/EN/123 maps and the language-switch input callback; selects
// the RU map initially. Call once after creating the lv_keyboard object.
void gui_keyboard_install(lv_obj_t *kb);

// ---- cast menu overlay (rock_gui_views_cast.c) ----

void gui_cast_build(lv_obj_t *parent);
void gui_cast_hide(void);
void gui_cast_menu_show(bool scanning);
void gui_cast_set_devices(const rock_gui_cast_item_t *items, size_t count);
void gui_cast_menu_dismiss(void);
void gui_cast_apply_active_output(bool is_casting);   // local-speaker badge
bool gui_cast_trigger(const char *action);         // CAST:SELECT:<row>, CAST:LOCAL
