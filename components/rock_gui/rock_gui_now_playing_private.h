#pragma once
#include "rock_gui_internal.h"

void np_view_build(lv_obj_t *parent);
void np_view_show(void);
void np_view_hide(void);
void np_view_set_station(const char *name, const char *desc, const char *codec);
void np_view_update_meta(const char *station_id, const char *station_name, const char *title, const char *subtitle);
void np_view_update_transport(rock_gui_playback_t status);
void np_view_apply_cast_state(bool is_casting, const char *receiver_name);
void np_view_present(const char *station_id, const char *title, const char *subtitle);
const char *np_view_station_text(void);
void np_transport_build(lv_obj_t *parent);
void np_transport_show(void);
void np_transport_hide(void);
void np_transport_update(rock_gui_playback_t status, uint8_t volume, bool muted);
void np_transport_apply_cast_state(bool is_casting);
bool np_transport_trigger(const char *action);
void np_spectrum_build(lv_obj_t *parent);
void np_spectrum_update(const float *bands, bool playing);
