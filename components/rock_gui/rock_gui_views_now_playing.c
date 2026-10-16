// Public Now Playing facade. The deck, transport and spectrum keep their own
// widget state in focused implementation files.
#include "rock_gui_now_playing_private.h"

static uint8_t s_last_volume = 65;
static bool s_last_muted;

void gui_now_playing_build(lv_obj_t *parent)
{
    np_view_build(parent);
    np_transport_build(gui_screen());
    gui_now_playing_update_transport(ROCK_GUI_PLAYBACK_IDLE, s_last_volume, s_last_muted);
}
void gui_now_playing_show(void) { np_view_show(); np_transport_show(); }
void gui_now_playing_hide(void) { np_view_hide(); np_transport_hide(); }
void gui_now_playing_set_station(const char *name, const char *desc, const char *codec)
{
    np_view_set_station(name, desc, codec);
    gui_now_playing_update_transport(ROCK_GUI_PLAYBACK_BUFFERING, s_last_volume, s_last_muted);
}
void gui_now_playing_update_meta(const char *station_id, const char *station_name,
                                 const char *title, const char *subtitle)
{
    np_view_update_meta(station_id, station_name, title, subtitle);
}
void gui_now_playing_update_transport(rock_gui_playback_t status, uint8_t volume, bool muted)
{
    s_last_volume = volume;
    s_last_muted = muted;
    np_transport_update(status, volume, muted);
    np_view_update_transport(status);
}
void gui_now_playing_apply_cast_state(bool is_casting, const char *receiver_name)
{
    np_transport_apply_cast_state(is_casting);
    np_view_apply_cast_state(is_casting, receiver_name);
}
void gui_now_playing_spectrum_update(const float *bands, bool playing) { np_spectrum_update(bands, playing); }
void gui_now_playing_present(const char *station_id, const char *title, const char *subtitle)
{
    np_view_present(station_id, title, subtitle);
}
const char *gui_now_playing_station_text(void) { return np_view_station_text(); }
bool gui_now_playing_trigger(const char *action) { return np_transport_trigger(action); }
