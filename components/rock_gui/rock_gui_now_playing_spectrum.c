#include "rock_gui_now_playing_private.h"
#include "audio_spectrum.h"
#include "rock_ui_tokens.h"

#define NP_SPEC_W 296
#define NP_SPEC_H 168
#define NP_SPEC_BAR_W 15
#define NP_SPEC_BAR_STEP 18
#define NP_SPEC_BAR_X0 5
#define NP_SPEC_MIN_H 12
#define NP_SPEC_MAX_H 162

static lv_obj_t *s_bars[SPECTRUM_BANDS_COUNT];
static lv_obj_t *s_peaks[SPECTRUM_BANDS_COUNT];
static float s_peak_level[SPECTRUM_BANDS_COUNT];

void np_spectrum_build(lv_obj_t *parent)
{
    lv_obj_t *spectrum = lv_obj_create(parent);
    lv_obj_remove_style_all(spectrum);
    lv_obj_set_size(spectrum, NP_SPEC_W, NP_SPEC_H);
    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        s_peaks[i] = lv_obj_create(spectrum);
        lv_obj_remove_style_all(s_peaks[i]);
        lv_obj_set_size(s_peaks[i], NP_SPEC_BAR_W, 2);
        lv_obj_set_pos(s_peaks[i], NP_SPEC_BAR_X0 + i * NP_SPEC_BAR_STEP, 0);
        lv_obj_set_style_bg_color(s_peaks[i], ROCK_COLOR_WHITE, 0);
        lv_obj_set_style_bg_opa(s_peaks[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_peaks[i], 1, 0);
        lv_obj_add_flag(s_peaks[i], LV_OBJ_FLAG_HIDDEN);
        s_bars[i] = lv_obj_create(spectrum);
        lv_obj_remove_style_all(s_bars[i]);
        lv_obj_set_size(s_bars[i], NP_SPEC_BAR_W, NP_SPEC_MIN_H);
        lv_obj_set_pos(s_bars[i], NP_SPEC_BAR_X0 + i * NP_SPEC_BAR_STEP, NP_SPEC_H - NP_SPEC_MIN_H);
        lv_obj_set_style_bg_color(s_bars[i], ROCK_COLOR_BAR_DIM, 0);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_bars[i], ROCK_RADIUS_BAR, 0);
    }
}

void np_spectrum_update(const float *bands, bool playing)
{
    static uint32_t tick;
    static int last_h[SPECTRUM_BANDS_COUNT], last_peak_y[SPECTRUM_BANDS_COUNT];
    static bool last_accent[SPECTRUM_BANDS_COUNT], was_playing, initialized;
    if (playing && (++tick & 1)) return;
    bool state_flip = !initialized || playing != was_playing;
    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        lv_obj_t *bar = s_bars[i];
        if (!bar) continue;
        float value = bands[i];
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        bool accent = playing && value > 0.06f;
        int height = playing ? NP_SPEC_MIN_H + ((int)(value * (NP_SPEC_MAX_H - NP_SPEC_MIN_H)) / 3) * 3 : NP_SPEC_MIN_H;
        if (height > NP_SPEC_MAX_H) height = NP_SPEC_MAX_H;
        if (state_flip || !initialized || height != last_h[i]) {
            lv_obj_set_height(bar, height); lv_obj_set_y(bar, NP_SPEC_H - height); last_h[i] = height;
        }
        if (state_flip || !initialized || accent != last_accent[i]) {
            lv_obj_set_style_bg_color(bar, accent ? ROCK_COLOR_ACCENT : ROCK_COLOR_BAR_DIM, 0); last_accent[i] = accent;
        }
        lv_obj_t *peak = s_peaks[i];
        if (playing) {
            if (value > s_peak_level[i]) s_peak_level[i] = value;
            else if ((s_peak_level[i] -= 0.024f) < 0.0f) s_peak_level[i] = 0.0f;
            if (peak && s_peak_level[i] > 0.04f) {
                int peak_height = NP_SPEC_MIN_H + ((int)(s_peak_level[i] * (NP_SPEC_MAX_H - NP_SPEC_MIN_H)) / 3) * 3;
                int peak_y = NP_SPEC_H - peak_height - 2;
                if (state_flip || !initialized || peak_y != last_peak_y[i]) {
                    lv_obj_set_y(peak, peak_y); lv_obj_remove_flag(peak, LV_OBJ_FLAG_HIDDEN); last_peak_y[i] = peak_y;
                }
            } else if (peak && !lv_obj_has_flag(peak, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(peak, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            s_peak_level[i] = 0.0f;
            if (peak && !lv_obj_has_flag(peak, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(peak, LV_OBJ_FLAG_HIDDEN);
        }
    }
    was_playing = playing;
    initialized = true;
}
