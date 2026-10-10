// Now-Playing view + transport bar, phase-4 split out of rock_gui.c. This
// file owns the now-playing static widgets; the facade (rock_gui.c) drives
// it through the rock_gui_internal.h entry points.

#include "rock_gui_internal.h"
#include "rockcast_tokens.h"
#include "rock_fonts.h"
#include "audio_spectrum.h"

#include <string.h>

static lv_obj_t *s_view_np = NULL;
static lv_obj_t *s_lbl_np_badge = NULL;
static lv_obj_t *s_lbl_np_station = NULL;
static lv_obj_t *s_lbl_np_subtitle = NULL;
static lv_obj_t *s_lbl_np_track = NULL;
static lv_obj_t *s_cont_np_codec = NULL;
static lv_obj_t *s_lbl_np_codec = NULL;

static lv_obj_t *s_cont_spectrum = NULL;
static lv_obj_t *s_spectrum_bars[SPECTRUM_BANDS_COUNT];

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

void gui_now_playing_build(lv_obj_t *parent)
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

    // Transport Bar (x=16, y=330, w=768, h=64) — lives on the shell screen
    // so it stays visible only while this view is active.
    s_cont_transport = lv_obj_create(gui_screen());
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
    s_btn_browse = gui_make_button(s_cont_transport, 100, 48, "Каталог",
                                   &font_inter_14, on_nav_browse_click, NULL);

    // Nav: Search
    s_btn_search = gui_make_button(s_cont_transport, 90, 48, "Поиск",
                                   &font_inter_14, on_nav_search_click, NULL);
    lv_obj_set_style_pad_left(s_btn_search, 8, 0);

    // Spacer
    lv_obj_t *sp1 = lv_obj_create(s_cont_transport);
    lv_obj_remove_style_all(sp1);
    lv_obj_set_flex_grow(sp1, 1);

    // Play/Pause Dominant Button (64x52)
    s_btn_play_pause = gui_make_button(s_cont_transport, 64, 52, ROCK_ICON_PLAY,
                                       &font_inter_24, on_play_pause_click, NULL);
    lv_obj_set_style_bg_color(s_btn_play_pause, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_btn_play_pause, 6, 0);
    s_lbl_play_pause = lv_obj_get_child(s_btn_play_pause, 0);
    lv_obj_set_style_text_color(s_lbl_play_pause, ROCK_COLOR_WHITE, 0);

    // Stop Button (54x52)
    s_btn_stop = gui_make_button(s_cont_transport, 54, 52, ROCK_ICON_STOP,
                                 &font_inter_18, on_stop_click, NULL);
    lv_obj_set_style_radius(s_btn_stop, 6, 0);

    // Spacer
    lv_obj_t *sp2 = lv_obj_create(s_cont_transport);
    lv_obj_remove_style_all(sp2);
    lv_obj_set_flex_grow(sp2, 1);

    // Volume Controls: [-] 65% [+]
    s_btn_vol_down = gui_make_button(s_cont_transport, 48, 48, "-",
                                     &font_inter_18, on_vol_down_click, NULL);

    s_lbl_vol_val = lv_label_create(s_cont_transport);
    lv_label_set_text(s_lbl_vol_val, " 65% ");
    lv_obj_set_style_text_font(s_lbl_vol_val, &font_hack_16, 0);
    lv_obj_set_style_text_color(s_lbl_vol_val, ROCK_COLOR_FG, 0);

    s_btn_vol_up = gui_make_button(s_cont_transport, 48, 48, "+",
                                   &font_inter_18, on_vol_up_click, NULL);

    // Cast dominant action button
    s_btn_cast = gui_make_button(s_cont_transport, 68, 48, "Cast",
                                 &font_inter_14, on_cast_btn_click, NULL);
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
}

void gui_now_playing_update_meta(const char *station_id, const char *title, const char *subtitle)
{
    if (station_id && station_id[0]) {
        if (strcmp(gui_station_id(), station_id) != 0) {
            const char *cur = lv_label_get_text(s_lbl_np_station);
            if (!cur || cur[0] == '\0') {
                lv_label_set_text(s_lbl_np_station, station_id);
            }
        }
        gui_set_station_id(station_id);
    }
    if (title && title[0]) {
        lv_label_set_text(s_lbl_np_station, title);
    }
    if (subtitle && subtitle[0]) {
        lv_label_set_text(s_lbl_np_subtitle, subtitle);
    }
}

void gui_now_playing_update_transport(rock_gui_playback_t status, uint8_t volume, bool muted)
{
    if (s_lbl_play_pause) {
        if (status == ROCK_GUI_PLAYBACK_PLAYING) {
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

        int h = 4 + (int)(val * 64.0f);
        lv_obj_set_height(s_spectrum_bars[i], h);
        lv_obj_set_y(s_spectrum_bars[i], 74 - h);

        if (val > 0.08f && playing) {
            lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_ACCENT, 0);
        } else {
            lv_obj_set_style_bg_color(s_spectrum_bars[i], ROCK_COLOR_BAR_DIM, 0);
        }
    }
}

void gui_now_playing_present(const char *station_id, const char *title, const char *subtitle)
{
    gui_set_station_id(station_id);
    lv_label_set_text(s_lbl_np_station, title ? title : station_id);
    lv_label_set_text(s_lbl_np_subtitle, subtitle ? subtitle : "");
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
