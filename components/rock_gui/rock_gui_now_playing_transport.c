#include "rock_gui_now_playing_private.h"
#include "rock_fonts.h"
#include "rock_ui_tokens.h"

#include <string.h>

static lv_obj_t *s_container, *s_catalog, *s_play_pause, *s_play_pause_label, *s_spinner;
static lv_obj_t *s_stop, *s_stop_label, *s_volume_down, *s_volume_label, *s_volume_up, *s_cast, *s_cast_label;
static lv_obj_t *s_voice;

static void on_catalog(lv_event_t *e)
{
    (void)e;
    gui_switch_view(VIEW_CATALOG);
    // A first-page browse would reset the view state (query, results,
    // scroll). Skip it when the catalog still holds content; an empty or
    // stale-cleared view falls through and fetches page one as before.
    if (!gui_catalog_has_content()) {
        gui_enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, "", 0);
    }
}
static void on_play_pause(lv_event_t *e) { (void)e; if (!lv_obj_has_flag(s_play_pause, LV_OBJ_FLAG_CLICKABLE)) return; gui_enqueue_event(gui_playback_status() == ROCK_GUI_PLAYBACK_PLAYING ? ROCK_UI_EVT_PLAYBACK_PAUSE : ROCK_UI_EVT_PLAYBACK_PLAY, NULL, 0); }
static void on_stop(lv_event_t *e) { (void)e; if (!lv_obj_has_flag(s_stop, LV_OBJ_FLAG_CLICKABLE)) return; gui_enqueue_event(ROCK_UI_EVT_PLAYBACK_STOP, NULL, 0); }
static void on_volume_down(lv_event_t *e) { (void)e; gui_enqueue_event(ROCK_UI_EVT_VOLUME_CHANGE, NULL, -5); }
static void on_volume_up(lv_event_t *e) { (void)e; gui_enqueue_event(ROCK_UI_EVT_VOLUME_CHANGE, NULL, 5); }
static void on_cast(lv_event_t *e) { (void)e; rock_gui_show_cast_menu(true); gui_enqueue_event(ROCK_UI_EVT_CAST_BROWSE, NULL, 0); }
static void on_voice(lv_event_t *e) { (void)e; gui_enqueue_event(ROCK_UI_EVT_VOICE_TOGGLE, NULL, 0); }

void np_transport_voice_state(int state)
{
    if (!s_voice) return;
    bool active = state >= 1 && state <= 3;
    lv_label_set_text(lv_obj_get_child(s_voice, 0), active ? "Отмена" : "Голос");
    lv_obj_set_style_bg_color(s_voice, active ? ROCK_COLOR_ERROR : ROCK_COLOR_MIC_AMBER, 0);
}

static void button_style(lv_obj_t *button, lv_color_t background, lv_color_t border, int width)
{
    lv_obj_set_style_bg_color(button, background, 0);
    lv_obj_set_style_border_color(button, border, 0);
    lv_obj_set_style_border_width(button, width, 0);
}

void np_transport_build(lv_obj_t *parent)
{
    s_container = lv_obj_create(parent);
    lv_obj_remove_style_all(s_container);
    lv_obj_set_size(s_container, 768, ROCK_TRANSPORT_HEIGHT);
    lv_obj_set_pos(s_container, 16, ROCK_TRANSPORT_Y);
    lv_obj_set_style_bg_color(s_container, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_container, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(s_container, 1, 0);
    lv_obj_set_style_radius(s_container, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_style_pad_hor(s_container, 12, 0);
    lv_obj_set_style_pad_column(s_container, 10, 0);
    lv_obj_set_flex_flow(s_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_catalog = gui_make_button(s_container, 110, 48, "Каталог", &font_inter_14, on_catalog, NULL);
    lv_obj_set_style_radius(s_catalog, 24, 0); button_style(s_catalog, ROCK_COLOR_PANEL_2, ROCK_COLOR_BORDER, 1);
    s_play_pause = gui_make_button(s_container, 64, 64, ROCK_ICON_PLAY, &font_inter_24, on_play_pause, NULL);
    lv_obj_set_style_radius(s_play_pause, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_color(s_play_pause, ROCK_COLOR_ACCENT, 0);
    s_play_pause_label = lv_obj_get_child(s_play_pause, 0); lv_obj_set_style_text_color(s_play_pause_label, ROCK_COLOR_WHITE, 0);
    s_spinner = lv_spinner_create(s_play_pause);
    lv_obj_set_size(s_spinner, 24, 24); lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN); lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_spinner, true, LV_PART_INDICATOR); lv_obj_set_style_arc_color(s_spinner, ROCK_COLOR_ACCENT, LV_PART_INDICATOR); lv_obj_set_style_arc_color(s_spinner, ROCK_COLOR_PANEL_2, LV_PART_MAIN);
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_CLICKABLE); lv_obj_center(s_spinner); lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    s_stop = gui_make_button(s_container, 56, 56, ROCK_ICON_STOP, &font_inter_18, on_stop, NULL);
    lv_obj_set_style_radius(s_stop, LV_RADIUS_CIRCLE, 0); button_style(s_stop, ROCK_COLOR_PANEL_2, ROCK_COLOR_BORDER, 1); s_stop_label = lv_obj_get_child(s_stop, 0);
    s_volume_label = lv_label_create(s_container); lv_label_set_text(s_volume_label, " 65% "); lv_obj_set_width(s_volume_label, 64); lv_obj_set_style_text_align(s_volume_label, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_text_font(s_volume_label, &font_hack_16, 0); lv_obj_set_style_text_color(s_volume_label, ROCK_COLOR_FG, 0);
    s_volume_down = gui_make_button(s_container, 56, 56, "-", &font_inter_18, on_volume_down, NULL); lv_obj_set_style_radius(s_volume_down, LV_RADIUS_CIRCLE, 0); button_style(s_volume_down, ROCK_COLOR_PANEL_2, ROCK_COLOR_BORDER, 1);
    s_volume_up = gui_make_button(s_container, 56, 56, "+", &font_inter_18, on_volume_up, NULL); lv_obj_set_style_radius(s_volume_up, LV_RADIUS_CIRCLE, 0); button_style(s_volume_up, ROCK_COLOR_PANEL_2, ROCK_COLOR_BORDER, 1);
    s_cast = gui_make_button(s_container, 110, 48, "Cast", &font_inter_14, on_cast, NULL); lv_obj_set_style_radius(s_cast, 24, 0); button_style(s_cast, ROCK_COLOR_PANEL_2, ROCK_COLOR_BORDER, 1); s_cast_label = lv_obj_get_child(s_cast, 0);
    s_voice = gui_make_button(s_container, 92, 48, "Голос", &font_inter_14, on_voice, NULL);
    lv_obj_set_style_radius(s_voice, 24, 0);
    lv_obj_set_style_bg_color(s_voice, ROCK_COLOR_MIC_AMBER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_voice, 0), ROCK_COLOR_WHITE, 0);
}
void np_transport_show(void) { if (s_container) lv_obj_remove_flag(s_container, LV_OBJ_FLAG_HIDDEN); }
void np_transport_hide(void) { if (s_container) lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN); }

void np_transport_update(rock_gui_playback_t status, uint8_t volume, bool muted)
{
    if (!s_play_pause || !s_play_pause_label || !s_stop || !s_stop_label) return;
    bool buffering = status == ROCK_GUI_PLAYBACK_BUFFERING;
    bool idle = status == ROCK_GUI_PLAYBACK_STOPPED || status == ROCK_GUI_PLAYBACK_IDLE;
    bool playing = status == ROCK_GUI_PLAYBACK_PLAYING;
    if (buffering) {
        lv_obj_remove_flag(s_play_pause, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(s_play_pause_label, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN); button_style(s_play_pause, ROCK_COLOR_PANEL, ROCK_COLOR_BORDER_SUBTLE, 1);
        lv_obj_remove_flag(s_stop, LV_OBJ_FLAG_CLICKABLE); button_style(s_stop, ROCK_COLOR_PANEL, ROCK_COLOR_BORDER_SUBTLE, 1); lv_obj_set_style_text_color(s_stop_label, ROCK_COLOR_MUTED, 0);
    } else {
        lv_obj_add_flag(s_play_pause, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN); lv_label_set_text(s_play_pause_label, playing ? ROCK_ICON_PAUSE : ROCK_ICON_PLAY); lv_obj_set_style_text_color(s_play_pause_label, ROCK_COLOR_WHITE, 0); lv_obj_remove_flag(s_play_pause_label, LV_OBJ_FLAG_HIDDEN);
        button_style(s_play_pause, playing ? ROCK_COLOR_ACCENT : ROCK_COLOR_PANEL_2, ROCK_COLOR_ACCENT, playing ? 0 : 2);
        if (idle) { lv_obj_remove_flag(s_stop, LV_OBJ_FLAG_CLICKABLE); button_style(s_stop, ROCK_COLOR_PANEL, ROCK_COLOR_BORDER_SUBTLE, 1); lv_obj_set_style_text_color(s_stop_label, ROCK_COLOR_MUTED, 0); }
        else { lv_obj_add_flag(s_stop, LV_OBJ_FLAG_CLICKABLE); button_style(s_stop, ROCK_COLOR_PANEL_2, ROCK_COLOR_BORDER, 1); lv_obj_set_style_text_color(s_stop_label, ROCK_COLOR_FG, 0); }
    }
    if (muted) lv_label_set_text(s_volume_label, " MUTE "); else lv_label_set_text_fmt(s_volume_label, " %u%% ", volume);
}
void np_transport_apply_cast_state(bool is_casting)
{
    if (!s_cast) return;
    lv_obj_set_style_bg_color(s_cast, is_casting ? ROCK_COLOR_ACCENT : ROCK_COLOR_PANEL_2, 0);
    if (s_cast_label) lv_obj_set_style_text_color(s_cast_label, is_casting ? ROCK_COLOR_WHITE : ROCK_COLOR_FG, 0);
}
bool np_transport_trigger(const char *action)
{
    lv_obj_t *button = !strcmp(action, "CATALOG") ? s_catalog : !strcmp(action, "VOL+") ? s_volume_up : !strcmp(action, "VOL-") ? s_volume_down : !strcmp(action, "CAST") ? s_cast : NULL;
    if (!button && !strcmp(action, "PLAYPAUSE")) button = s_play_pause;
    if (!button && !strcmp(action, "STOP")) button = s_stop;
    if (!button) return false;
    if ((button != s_play_pause && button != s_stop) || lv_obj_has_flag(button, LV_OBJ_FLAG_CLICKABLE)) lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    return true;
}
