// Now-Playing view + transport bar, phase-4 split out of rock_gui.c. This
// file owns the now-playing static widgets; the facade (rock_gui.c) drives
// it through the rock_gui_internal.h entry points. Phase 3 restyled the view
// to the rockcast player deck (docs/design/phase3-mockup.html): monogram,
// borderless in-deck spectrum with peak ticks, deck footer and the circular
// transport. Handlers and the emitted events are unchanged.

#include "rock_gui_internal.h"
#include "rock_ui_tokens.h"
#include "rock_fonts.h"
#include "audio_spectrum.h"

#include <string.h>

static lv_obj_t *s_view_np = NULL;

// Player deck
static lv_obj_t *s_mono_box = NULL;
static lv_obj_t *s_mono_lbl = NULL;
static lv_obj_t *s_lbl_np_badge = NULL;
static lv_obj_t *s_lbl_np_station = NULL;
static lv_obj_t *s_lbl_np_subtitle = NULL;
static lv_obj_t *s_livedot = NULL;
static lv_obj_t *s_lbl_np_track = NULL;
static lv_obj_t *s_cont_np_codec = NULL;
static lv_obj_t *s_lbl_np_codec = NULL;
static lv_obj_t *s_foot_dot = NULL;
static lv_obj_t *s_lbl_np_foot = NULL;

// In-deck spectrum (296x168, no backdrop) + white peak ticks
#define NP_SPEC_W 296
#define NP_SPEC_H 168
#define NP_SPEC_BAR_W 15
#define NP_SPEC_BAR_STEP 18
#define NP_SPEC_BAR_X0 5
#define NP_SPEC_MIN_H 12
#define NP_SPEC_MAX_H 162
static lv_obj_t *s_cont_spectrum = NULL;
static lv_obj_t *s_spectrum_bars[SPECTRUM_BANDS_COUNT];
static lv_obj_t *s_spectrum_peaks[SPECTRUM_BANDS_COUNT];
static float s_peak_level[SPECTRUM_BANDS_COUNT];

// Transport controls (created on the shell screen, owned here)
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

static void on_nav_browse_click(lv_event_t *e)
{
    (void)e;
    gui_switch_view(VIEW_BROWSE);
    gui_enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, "", 0);
}

static void on_nav_search_click(lv_event_t *e)
{
    (void)e;
    gui_switch_view(VIEW_SEARCH);
}

static void on_play_pause_click(lv_event_t *e)
{
    (void)e;
    if (gui_playback_status() == ROCK_GUI_PLAYBACK_PLAYING) {
        gui_enqueue_event(ROCK_UI_EVT_PLAYBACK_PAUSE, NULL, 0);
    } else {
        gui_enqueue_event(ROCK_UI_EVT_PLAYBACK_PLAY, NULL, 0);
    }
}

static void on_stop_click(lv_event_t *e)
{
    (void)e;
    gui_enqueue_event(ROCK_UI_EVT_PLAYBACK_STOP, NULL, 0);
}

static void on_vol_down_click(lv_event_t *e)
{
    (void)e;
    gui_enqueue_event(ROCK_UI_EVT_VOLUME_CHANGE, NULL, -5);
}

static void on_vol_up_click(lv_event_t *e)
{
    (void)e;
    gui_enqueue_event(ROCK_UI_EVT_VOLUME_CHANGE, NULL, 5);
}

static void on_cast_btn_click(lv_event_t *e)
{
    (void)e;
    rock_gui_show_cast_menu(true);
    gui_enqueue_event(ROCK_UI_EVT_CAST_BROWSE, NULL, 0);
}

static void np_set_monogram(const char *name)
{
    if (s_mono_box && s_mono_lbl) {
        gui_monogram_apply(s_mono_box, s_mono_lbl, name);
    }
}

void gui_now_playing_build(lv_obj_t *parent)
{
    s_view_np = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_np);
    lv_obj_set_size(s_view_np, 800, 404);
    lv_obj_set_pos(s_view_np, 0, 0);

    // Player deck 768x300 @ (16,12): PANEL, r10, 1px BORDER_SUBTLE, pad 12/16
    lv_obj_t *deck = lv_obj_create(s_view_np);
    lv_obj_remove_style_all(deck);
    lv_obj_set_size(deck, 768, 300);
    lv_obj_set_pos(deck, 16, 12);
    lv_obj_set_style_bg_color(deck, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(deck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(deck, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(deck, 1, 0);
    lv_obj_set_style_radius(deck, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_style_pad_hor(deck, 16, 0);
    lv_obj_set_style_pad_ver(deck, 12, 0);
    lv_obj_set_flex_flow(deck, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *deck_top = lv_obj_create(deck);
    lv_obj_remove_style_all(deck_top);
    lv_obj_set_width(deck_top, lv_pct(100));
    lv_obj_set_flex_grow(deck_top, 1);
    lv_obj_set_flex_flow(deck_top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(deck_top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(deck_top, 18, 0);

    // Station monogram 96x96, r8 + 2px ACCENT ring
    s_mono_box = lv_obj_create(deck_top);
    lv_obj_remove_style_all(s_mono_box);
    lv_obj_set_size(s_mono_box, 96, 96);
    lv_obj_set_style_bg_color(s_mono_box, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_mono_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_mono_box, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_mono_box, 2, 0);
    lv_obj_set_style_radius(s_mono_box, ROCK_RADIUS_WIDGET, 0);

    s_mono_lbl = lv_label_create(s_mono_box);
    lv_label_set_text(s_mono_lbl, "?");
    lv_obj_set_style_text_font(s_mono_lbl, &font_inter_24, 0);
    lv_obj_set_style_text_color(s_mono_lbl, ROCK_COLOR_MONO_TEXT, 0);
    lv_obj_center(s_mono_lbl);

    // Deck info column (288px)
    lv_obj_t *info = lv_obj_create(deck_top);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 288, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 7, 0);

    s_lbl_np_badge = lv_label_create(info);
    lv_label_set_text(s_lbl_np_badge, "СЕЙЧАС ИГРАЕТ");
    lv_obj_set_style_text_color(s_lbl_np_badge, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_lbl_np_badge, &font_inter_14, 0);

    s_lbl_np_station = lv_label_create(info);
    lv_label_set_text(s_lbl_np_station, "Выберите станцию");
    lv_label_set_long_mode(s_lbl_np_station, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_np_station, 288);
    lv_obj_set_style_text_color(s_lbl_np_station, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_lbl_np_station, &font_inter_24, 0);

    s_lbl_np_subtitle = lv_label_create(info);
    lv_label_set_text(s_lbl_np_subtitle, "");
    lv_label_set_long_mode(s_lbl_np_subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_np_subtitle, 288);
    lv_obj_set_style_text_color(s_lbl_np_subtitle, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_np_subtitle, &font_inter_14, 0);

    lv_obj_t *track_row = lv_obj_create(info);
    lv_obj_remove_style_all(track_row);
    lv_obj_set_size(track_row, 288, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(track_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(track_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(track_row, 9, 0);

    s_livedot = lv_obj_create(track_row);
    lv_obj_remove_style_all(s_livedot);
    lv_obj_set_size(s_livedot, 9, 9);
    lv_obj_set_style_radius(s_livedot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_livedot, ROCK_COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_livedot, LV_OPA_COVER, 0);

    s_lbl_np_track = lv_label_create(track_row);
    lv_label_set_text(s_lbl_np_track, "");
    lv_label_set_long_mode(s_lbl_np_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_np_track, 248);
    lv_obj_set_style_text_color(s_lbl_np_track, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_np_track, &font_inter_18, 0);

    s_cont_np_codec = lv_obj_create(info);
    lv_obj_remove_style_all(s_cont_np_codec);
    lv_obj_set_size(s_cont_np_codec, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_bg_color(s_cont_np_codec, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_cont_np_codec, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cont_np_codec, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(s_cont_np_codec, 1, 0);
    lv_obj_set_style_radius(s_cont_np_codec, 6, 0);
    lv_obj_set_style_pad_hor(s_cont_np_codec, 10, 0);

    s_lbl_np_codec = lv_label_create(s_cont_np_codec);
    lv_label_set_text(s_lbl_np_codec, "");
    lv_obj_set_style_text_color(s_lbl_np_codec, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_np_codec, &font_inter_14, 0);
    lv_obj_center(s_lbl_np_codec);

    lv_obj_t *spec_spacer = lv_obj_create(deck_top);
    lv_obj_remove_style_all(spec_spacer);
    lv_obj_set_flex_grow(spec_spacer, 1);

    // Spectrum 296x168 right in the deck: no backdrop, bars + peak ticks
    s_cont_spectrum = lv_obj_create(deck_top);
    lv_obj_remove_style_all(s_cont_spectrum);
    lv_obj_set_size(s_cont_spectrum, NP_SPEC_W, NP_SPEC_H);

    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        s_spectrum_peaks[i] = lv_obj_create(s_cont_spectrum);
        lv_obj_remove_style_all(s_spectrum_peaks[i]);
        lv_obj_set_size(s_spectrum_peaks[i], NP_SPEC_BAR_W, 2);
        lv_obj_set_pos(s_spectrum_peaks[i], NP_SPEC_BAR_X0 + i * NP_SPEC_BAR_STEP, 0);
        lv_obj_set_style_bg_color(s_spectrum_peaks[i], ROCK_COLOR_WHITE, 0);
        lv_obj_set_style_bg_opa(s_spectrum_peaks[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_spectrum_peaks[i], 1, 0);
        lv_obj_add_flag(s_spectrum_peaks[i], LV_OBJ_FLAG_HIDDEN);

        s_spectrum_bars[i] = lv_obj_create(s_cont_spectrum);
        lv_obj_remove_style_all(s_spectrum_bars[i]);
        lv_obj_set_size(s_spectrum_bars[i], NP_SPEC_BAR_W, NP_SPEC_MIN_H);
        lv_obj_set_pos(s_spectrum_bars[i], NP_SPEC_BAR_X0 + i * NP_SPEC_BAR_STEP,
                       NP_SPEC_H - NP_SPEC_MIN_H);
        lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_BAR_DIM, 0);
        lv_obj_set_style_bg_opa(s_spectrum_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_spectrum_bars[i], ROCK_RADIUS_BAR, 0);
    }

    // Deck separator + footer
    lv_obj_t *sep = lv_obj_create(deck);
    lv_obj_remove_style_all(sep);
    lv_obj_set_width(sep, lv_pct(100));
    lv_obj_set_height(sep, 1);
    lv_obj_set_style_bg_color(sep, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    lv_obj_t *foot = lv_obj_create(deck);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, lv_pct(100), 30);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, 8, 0);

    s_foot_dot = lv_obj_create(foot);
    lv_obj_remove_style_all(s_foot_dot);
    lv_obj_set_size(s_foot_dot, 8, 8);
    lv_obj_set_style_radius(s_foot_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_foot_dot, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(s_foot_dot, LV_OPA_COVER, 0);

    s_lbl_np_foot = lv_label_create(foot);
    lv_label_set_text(s_lbl_np_foot, "Плеер остановлен");
    lv_obj_set_style_text_color(s_lbl_np_foot, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_np_foot, &font_inter_14, 0);

    lv_obj_t *foot_sp = lv_obj_create(foot);
    lv_obj_remove_style_all(foot_sp);
    lv_obj_set_flex_grow(foot_sp, 1);

    lv_obj_t *srv_lbl = lv_label_create(foot);
    lv_label_set_text(srv_lbl, "ROCKSERVER CLOUD");
    lv_obj_set_style_text_color(srv_lbl, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(srv_lbl, &font_inter_14, 0);

    // Transport Bar 768x64 @ (16,364) — lives on the shell screen so it stays
    // visible only while this view is active. Same button set and handlers as
    // before; the cluster is centered per the mockup.
    s_cont_transport = lv_obj_create(gui_screen());
    lv_obj_remove_style_all(s_cont_transport);
    lv_obj_set_size(s_cont_transport, 768, ROCK_TRANSPORT_HEIGHT);
    lv_obj_set_pos(s_cont_transport, 16, 364);
    lv_obj_set_style_bg_color(s_cont_transport, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_cont_transport, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cont_transport, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(s_cont_transport, 1, 0);
    lv_obj_set_style_radius(s_cont_transport, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_style_pad_hor(s_cont_transport, 12, 0);
    lv_obj_set_style_pad_column(s_cont_transport, 16, 0);
    lv_obj_set_flex_flow(s_cont_transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cont_transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Nav: Browse (pill)
    s_btn_browse = gui_make_button(s_cont_transport, 110, 48, "Каталог",
                                   &font_inter_14, on_nav_browse_click, NULL);
    lv_obj_set_style_radius(s_btn_browse, 24, 0);
    lv_obj_set_style_border_color(s_btn_browse, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_btn_browse, 1, 0);

    // Nav: Search (pill)
    s_btn_search = gui_make_button(s_cont_transport, 104, 48, "Поиск",
                                   &font_inter_14, on_nav_search_click, NULL);
    lv_obj_set_style_radius(s_btn_search, 24, 0);
    lv_obj_set_style_border_color(s_btn_search, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_btn_search, 1, 0);

    // Play/Pause dominant circle (64x64; restyled per state in update_transport)
    s_btn_play_pause = gui_make_button(s_cont_transport, 64, 64, ROCK_ICON_PLAY,
                                       &font_inter_24, on_play_pause_click, NULL);
    lv_obj_set_style_bg_color(s_btn_play_pause, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_btn_play_pause, LV_RADIUS_CIRCLE, 0);
    s_lbl_play_pause = lv_obj_get_child(s_btn_play_pause, 0);
    lv_obj_set_style_text_color(s_lbl_play_pause, ROCK_COLOR_WHITE, 0);

    // Stop (56x56 circle)
    s_btn_stop = gui_make_button(s_cont_transport, 56, 56, ROCK_ICON_STOP,
                                 &font_inter_18, on_stop_click, NULL);
    lv_obj_set_style_radius(s_btn_stop, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_btn_stop, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_btn_stop, 1, 0);

    // Volume value + Vol circles ([-] 65% [+] cluster per the mockup)
    s_lbl_vol_val = lv_label_create(s_cont_transport);
    lv_label_set_text(s_lbl_vol_val, " 65% ");
    lv_obj_set_width(s_lbl_vol_val, 64);
    lv_obj_set_style_text_align(s_lbl_vol_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_vol_val, &font_hack_16, 0);
    lv_obj_set_style_text_color(s_lbl_vol_val, ROCK_COLOR_FG, 0);

    s_btn_vol_down = gui_make_button(s_cont_transport, 56, 56, "-",
                                     &font_inter_18, on_vol_down_click, NULL);
    lv_obj_set_style_radius(s_btn_vol_down, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_btn_vol_down, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_btn_vol_down, 1, 0);

    s_btn_vol_up = gui_make_button(s_cont_transport, 56, 56, "+",
                                   &font_inter_18, on_vol_up_click, NULL);
    lv_obj_set_style_radius(s_btn_vol_up, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_btn_vol_up, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_btn_vol_up, 1, 0);

    // Cast pill (ACCENT fill while casting — apply_cast_state)
    s_btn_cast = gui_make_button(s_cont_transport, 110, 48, "Cast",
                                 &font_inter_14, on_cast_btn_click, NULL);
    lv_obj_set_style_radius(s_btn_cast, 24, 0);
    lv_obj_set_style_border_color(s_btn_cast, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_btn_cast, 1, 0);
    s_lbl_cast = lv_obj_get_child(s_btn_cast, 0);
}

void gui_now_playing_show(void)
{
    if (s_view_np) lv_obj_remove_flag(s_view_np, LV_OBJ_FLAG_HIDDEN);
    if (s_cont_transport) lv_obj_remove_flag(s_cont_transport, LV_OBJ_FLAG_HIDDEN);
}

void gui_now_playing_hide(void)
{
    if (s_view_np) lv_obj_add_flag(s_view_np, LV_OBJ_FLAG_HIDDEN);
    if (s_cont_transport) lv_obj_add_flag(s_cont_transport, LV_OBJ_FLAG_HIDDEN);
}

void gui_now_playing_set_station(const char *name, const char *desc, const char *codec)
{
    lv_label_set_text(s_lbl_np_station, name);
    lv_label_set_text(s_lbl_np_subtitle, desc);
    lv_label_set_text(s_lbl_np_codec, codec);
    lv_label_set_text(s_lbl_np_track, "");
    np_set_monogram(name);
}

void gui_now_playing_update_meta(const char *station_id, const char *station_name,
                                const char *title, const char *subtitle)
{
    if (station_id && station_id[0]) {
        gui_set_station_id(station_id);
    }
    if (station_name && station_name[0]) {
        lv_label_set_text(s_lbl_np_station, station_name);
        np_set_monogram(station_name);
    }
    if (title) {
        lv_label_set_text(s_lbl_np_track, title);
    }
    if (subtitle && subtitle[0]) {
        lv_label_set_text(s_lbl_np_subtitle, subtitle);
    }
}

void gui_now_playing_update_transport(rock_gui_playback_t status, uint8_t volume, bool muted)
{
    bool playing = status == ROCK_GUI_PLAYBACK_PLAYING;

    if (s_lbl_play_pause && s_btn_play_pause) {
        if (playing) {
            lv_label_set_text(s_lbl_play_pause, ROCK_ICON_PAUSE);
            lv_obj_set_style_bg_color(s_btn_play_pause, ROCK_COLOR_ACCENT, 0);
            lv_obj_set_style_border_width(s_btn_play_pause, 0, 0);
        } else {
            lv_label_set_text(s_lbl_play_pause, ROCK_ICON_PLAY);
            lv_obj_set_style_bg_color(s_btn_play_pause, ROCK_COLOR_PANEL_2, 0);
            lv_obj_set_style_border_color(s_btn_play_pause, ROCK_COLOR_ACCENT, 0);
            lv_obj_set_style_border_width(s_btn_play_pause, 2, 0);
        }
    }
    if (s_lbl_vol_val) {
        if (muted) {
            lv_label_set_text(s_lbl_vol_val, " MUTE ");
        } else {
            lv_label_set_text_fmt(s_lbl_vol_val, " %u%% ", volume);
        }
    }
    if (s_lbl_np_track) {
        lv_obj_set_style_text_color(s_lbl_np_track, playing ? ROCK_COLOR_ACCENT : ROCK_COLOR_MUTED, 0);
    }
    if (s_livedot) {
        if (playing) lv_obj_remove_flag(s_livedot, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_livedot, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_foot_dot) {
        lv_obj_set_style_bg_color(s_foot_dot, playing ? ROCK_COLOR_GREEN : ROCK_COLOR_MUTED, 0);
    }
    if (s_lbl_np_foot) {
        lv_label_set_text(s_lbl_np_foot, playing ? "Прямой эфир" : "Плеер остановлен");
    }
}

void gui_now_playing_apply_cast_state(bool is_casting, const char *receiver_name)
{
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
        if (is_casting && receiver_name && receiver_name[0]) {
            lv_label_set_text_fmt(s_lbl_np_badge, ROCK_ICON_PLAY " %s", receiver_name);
        } else {
            lv_label_set_text(s_lbl_np_badge, "СЕЙЧАС ИГРАЕТ");
        }
    }
}

void gui_now_playing_spectrum_update(const float *bands, bool playing)
{
    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        if (!s_spectrum_bars[i]) continue;
        float val = bands[i];
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;

        int h = playing ? NP_SPEC_MIN_H + (int)(val * (NP_SPEC_MAX_H - NP_SPEC_MIN_H))
                        : NP_SPEC_MIN_H;
        lv_obj_set_height(s_spectrum_bars[i], h);
        lv_obj_set_y(s_spectrum_bars[i], NP_SPEC_H - h);

        if (val > 0.06f && playing) {
            lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_ACCENT, 0);
        } else {
            lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_BAR_DIM, 0);
        }

        // Peak tick: fast rise, slow decay, hidden while idle
        if (playing) {
            if (val > s_peak_level[i]) s_peak_level[i] = val;
            else {
                s_peak_level[i] -= 0.012f;
                if (s_peak_level[i] < 0.0f) s_peak_level[i] = 0.0f;
            }
            if (s_spectrum_peaks[i]) {
                if (s_peak_level[i] > 0.04f) {
                    int ph = NP_SPEC_MIN_H + (int)(s_peak_level[i] * (NP_SPEC_MAX_H - NP_SPEC_MIN_H));
                    lv_obj_set_y(s_spectrum_peaks[i], NP_SPEC_H - ph - 2);
                    lv_obj_remove_flag(s_spectrum_peaks[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_spectrum_peaks[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            s_peak_level[i] = 0.0f;
            if (s_spectrum_peaks[i]) lv_obj_add_flag(s_spectrum_peaks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void gui_now_playing_present(const char *station_id, const char *title, const char *subtitle)
{
    gui_set_station_id(station_id);
    lv_label_set_text(s_lbl_np_station, title ? title : station_id);
    lv_label_set_text(s_lbl_np_subtitle, subtitle ? subtitle : "");
    np_set_monogram(title ? title : station_id);
}

const char *gui_now_playing_station_text(void)
{
    return lv_label_get_text(s_lbl_np_station);
}

bool gui_now_playing_trigger(const char *action)
{
    if (!strcmp(action, "CATALOG")) {
        if (s_btn_browse) lv_obj_send_event(s_btn_browse, LV_EVENT_CLICKED, NULL);
        return true;
    }
    if (!strcmp(action, "PLAYPAUSE")) {
        if (s_btn_play_pause) lv_obj_send_event(s_btn_play_pause, LV_EVENT_CLICKED, NULL);
        return true;
    }
    if (!strcmp(action, "STOP")) {
        if (s_btn_stop) lv_obj_send_event(s_btn_stop, LV_EVENT_CLICKED, NULL);
        return true;
    }
    if (!strcmp(action, "VOL+")) {
        if (s_btn_vol_up) lv_obj_send_event(s_btn_vol_up, LV_EVENT_CLICKED, NULL);
        return true;
    }
    if (!strcmp(action, "VOL-")) {
        if (s_btn_vol_down) lv_obj_send_event(s_btn_vol_down, LV_EVENT_CLICKED, NULL);
        return true;
    }
    if (!strcmp(action, "CAST")) {
        if (s_btn_cast) lv_obj_send_event(s_btn_cast, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return false;
}
