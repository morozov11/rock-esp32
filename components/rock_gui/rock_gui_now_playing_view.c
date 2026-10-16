#include "rock_gui_now_playing_private.h"
#include "rock_fonts.h"
#include "rock_ui_tokens.h"

static lv_obj_t *s_view, *s_mono_box, *s_mono_label, *s_badge, *s_station, *s_subtitle;
static lv_obj_t *s_live_dot, *s_track, *s_codec, *s_footer_dot, *s_footer;

static lv_obj_t *container(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    return obj;
}

static void set_monogram(const char *name)
{
    if (s_mono_box && s_mono_label) gui_monogram_apply(s_mono_box, s_mono_label, name);
}

void np_view_build(lv_obj_t *parent)
{
    s_view = container(parent);
    lv_obj_set_size(s_view, 800, 404);
    lv_obj_set_pos(s_view, 0, 0);

    lv_obj_t *deck = container(s_view);
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

    lv_obj_t *top = container(deck);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_flex_grow(top, 1);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 18, 0);

    s_mono_box = container(top);
    lv_obj_set_size(s_mono_box, 96, 96);
    lv_obj_set_style_bg_color(s_mono_box, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_mono_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_mono_box, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_mono_box, 2, 0);
    lv_obj_set_style_radius(s_mono_box, ROCK_RADIUS_WIDGET, 0);
    s_mono_label = lv_label_create(s_mono_box);
    lv_label_set_text(s_mono_label, "?");
    lv_obj_set_style_text_font(s_mono_label, &font_inter_24, 0);
    lv_obj_set_style_text_color(s_mono_label, ROCK_COLOR_MONO_TEXT, 0);
    lv_obj_center(s_mono_label);

    lv_obj_t *info = container(top);
    lv_obj_set_size(info, 288, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 7, 0);
    s_badge = lv_label_create(info);
    lv_label_set_text(s_badge, "СЕЙЧАС ИГРАЕТ");
    lv_obj_set_style_text_color(s_badge, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_badge, &font_inter_14, 0);
    s_station = lv_label_create(info);
    lv_label_set_text(s_station, "Выберите станцию");
    lv_label_set_long_mode(s_station, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_station, 288);
    lv_obj_set_style_text_color(s_station, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_station, &font_inter_24, 0);
    s_subtitle = lv_label_create(info);
    lv_label_set_text(s_subtitle, "");
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_subtitle, 288);
    lv_obj_set_style_text_color(s_subtitle, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_subtitle, &font_inter_14, 0);

    lv_obj_t *track_row = container(info);
    lv_obj_set_size(track_row, 288, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(track_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(track_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(track_row, 9, 0);
    s_live_dot = container(track_row);
    lv_obj_set_size(s_live_dot, 9, 9);
    lv_obj_set_style_radius(s_live_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live_dot, ROCK_COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_live_dot, LV_OPA_COVER, 0);
    s_track = lv_label_create(track_row);
    lv_label_set_text(s_track, "");
    lv_label_set_long_mode(s_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_track, 248);
    lv_obj_set_style_text_color(s_track, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_track, &font_inter_18, 0);

    lv_obj_t *codec_box = container(info);
    lv_obj_set_size(codec_box, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_bg_color(codec_box, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(codec_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(codec_box, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(codec_box, 1, 0);
    lv_obj_set_style_radius(codec_box, 6, 0);
    lv_obj_set_style_pad_hor(codec_box, 10, 0);
    s_codec = lv_label_create(codec_box);
    lv_label_set_text(s_codec, "");
    lv_obj_set_style_text_color(s_codec, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_codec, &font_inter_14, 0);
    lv_obj_center(s_codec);

    lv_obj_t *spacer = container(top);
    lv_obj_set_flex_grow(spacer, 1);
    np_spectrum_build(top);

    lv_obj_t *sep = container(deck);
    lv_obj_set_width(sep, lv_pct(100));
    lv_obj_set_height(sep, 1);
    lv_obj_set_style_bg_color(sep, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_t *foot = container(deck);
    lv_obj_set_size(foot, lv_pct(100), 30);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, 8, 0);
    s_footer_dot = container(foot);
    lv_obj_set_size(s_footer_dot, 8, 8);
    lv_obj_set_style_radius(s_footer_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_footer_dot, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(s_footer_dot, LV_OPA_COVER, 0);
    s_footer = lv_label_create(foot);
    lv_label_set_text(s_footer, "Плеер остановлен");
    lv_obj_set_style_text_color(s_footer, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_footer, &font_inter_14, 0);
    lv_obj_t *foot_spacer = container(foot);
    lv_obj_set_flex_grow(foot_spacer, 1);
    lv_obj_t *server = lv_label_create(foot);
    lv_label_set_text(server, "ROCKSERVER CLOUD");
    lv_obj_set_style_text_color(server, ROCK_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(server, &font_inter_14, 0);
}

void np_view_show(void) { if (s_view) lv_obj_remove_flag(s_view, LV_OBJ_FLAG_HIDDEN); }
void np_view_hide(void) { if (s_view) lv_obj_add_flag(s_view, LV_OBJ_FLAG_HIDDEN); }
void np_view_set_station(const char *name, const char *desc, const char *codec)
{
    lv_label_set_text(s_station, name); lv_label_set_text(s_subtitle, desc);
    lv_label_set_text(s_codec, codec); lv_label_set_text(s_track, ""); set_monogram(name);
}
void np_view_update_meta(const char *station_id, const char *station_name, const char *title, const char *subtitle)
{
    if (station_id && station_id[0]) gui_set_station_id(station_id);
    if (station_name && station_name[0]) { lv_label_set_text(s_station, station_name); set_monogram(station_name); }
    if (title) lv_label_set_text(s_track, title);
    if (subtitle && subtitle[0]) lv_label_set_text(s_subtitle, subtitle);
}
void np_view_update_transport(rock_gui_playback_t status)
{
    bool playing = status == ROCK_GUI_PLAYBACK_PLAYING;
    if (s_track) lv_obj_set_style_text_color(s_track, playing ? ROCK_COLOR_ACCENT : ROCK_COLOR_MUTED, 0);
    if (s_live_dot) { if (playing) lv_obj_remove_flag(s_live_dot, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_live_dot, LV_OBJ_FLAG_HIDDEN); }
    if (s_footer_dot) lv_obj_set_style_bg_color(s_footer_dot, playing ? ROCK_COLOR_GREEN : status == ROCK_GUI_PLAYBACK_BUFFERING ? ROCK_COLOR_WARN : ROCK_COLOR_MUTED, 0);
    if (s_footer) lv_label_set_text(s_footer, playing ? "Прямой эфир" : status == ROCK_GUI_PLAYBACK_BUFFERING ? "Подключение..." : status == ROCK_GUI_PLAYBACK_PAUSED ? "Пауза" : "Плеер остановлен");
}
void np_view_apply_cast_state(bool is_casting, const char *receiver_name)
{
    if (!s_badge) return;
    if (is_casting && receiver_name && receiver_name[0]) lv_label_set_text_fmt(s_badge, ROCK_ICON_PLAY " %s", receiver_name);
    else lv_label_set_text(s_badge, "СЕЙЧАС ИГРАЕТ");
}
void np_view_present(const char *station_id, const char *title, const char *subtitle)
{
    gui_set_station_id(station_id); lv_label_set_text(s_station, title ? title : station_id);
    lv_label_set_text(s_subtitle, subtitle ? subtitle : ""); set_monogram(title ? title : station_id);
}
const char *np_view_station_text(void) { return lv_label_get_text(s_station); }
