#include "rock_gui_now_playing_private.h"
#include "audio_spectrum.h"
#include "rock_ui_tokens.h"
#include <math.h>

#define NP_SPEC_W 296
#define NP_SPEC_H 168
#define NP_SPEC_BAR_W 15
#define NP_SPEC_BAR_STEP 18
#define NP_SPEC_BAR_X0 5
#define NP_SPEC_MIN_H 12
#define NP_SPEC_MAX_H 162

static lv_obj_t *s_spectrum_obj;
static float s_peak_level[SPECTRUM_BANDS_COUNT];
static int s_bar_height[SPECTRUM_BANDS_COUNT];
static bool s_bar_accent[SPECTRUM_BANDS_COUNT];
static int s_peak_y[SPECTRUM_BANDS_COUNT];
static bool s_peak_visible[SPECTRUM_BANDS_COUNT];

static void spectrum_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        // Draw bar
        lv_area_t area;
        area.x1 = coords.x1 + NP_SPEC_BAR_X0 + i * NP_SPEC_BAR_STEP;
        area.x2 = area.x1 + NP_SPEC_BAR_W - 1;
        area.y2 = coords.y1 + NP_SPEC_H - 1;
        area.y1 = coords.y1 + NP_SPEC_H - s_bar_height[i];

        lv_draw_rect_dsc_t bar_dsc;
        lv_draw_rect_dsc_init(&bar_dsc);
        bar_dsc.bg_color = s_bar_accent[i] ? ROCK_COLOR_ACCENT : ROCK_COLOR_BAR_DIM;
        bar_dsc.radius = ROCK_RADIUS_BAR;
        lv_draw_rect(layer, &bar_dsc, &area);

        // Draw peak if visible
        if (s_peak_visible[i]) {
            lv_area_t peak_area;
            peak_area.x1 = area.x1;
            peak_area.x2 = area.x2;
            peak_area.y1 = coords.y1 + s_peak_y[i];
            peak_area.y2 = peak_area.y1 + 1; // высота 2 px

            lv_draw_rect_dsc_t peak_dsc;
            lv_draw_rect_dsc_init(&peak_dsc);
            peak_dsc.bg_color = ROCK_COLOR_WHITE;
            peak_dsc.radius = 1;
            lv_draw_rect(layer, &peak_dsc, &peak_area);
        }
    }
}

void np_spectrum_build(lv_obj_t *parent)
{
    s_spectrum_obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s_spectrum_obj);
    lv_obj_set_size(s_spectrum_obj, NP_SPEC_W, NP_SPEC_H);
    lv_obj_add_event_cb(s_spectrum_obj, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        s_bar_height[i] = NP_SPEC_MIN_H;
        s_bar_accent[i] = false;
        s_peak_level[i] = 0.0f;
        s_peak_y[i] = 0;
        s_peak_visible[i] = false;
    }
}

void np_spectrum_update(const float *bands, bool playing)
{
    for (int i = 0; i < SPECTRUM_BANDS_COUNT; i++) {
        float value = bands ? bands[i] : 0.0f;
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;

        s_bar_accent[i] = playing && (value > 0.06f);
        int height = playing ? NP_SPEC_MIN_H + (int)roundf(value * (NP_SPEC_MAX_H - NP_SPEC_MIN_H)) : NP_SPEC_MIN_H;
        if (height > NP_SPEC_MAX_H) height = NP_SPEC_MAX_H;
        s_bar_height[i] = height;

        if (playing) {
            if (value > s_peak_level[i]) {
                s_peak_level[i] = value;
            } else {
                s_peak_level[i] -= 0.028f;
                if (s_peak_level[i] < 0.0f) {
                    s_peak_level[i] = 0.0f;
                }
            }

            if (s_peak_level[i] > 0.04f) {
                int peak_height = NP_SPEC_MIN_H + (int)roundf(s_peak_level[i] * (NP_SPEC_MAX_H - NP_SPEC_MIN_H));
                if (peak_height > NP_SPEC_MAX_H) peak_height = NP_SPEC_MAX_H;
                s_peak_y[i] = NP_SPEC_H - peak_height - 2;
                s_peak_visible[i] = true;
            } else {
                s_peak_visible[i] = false;
            }
        } else {
            s_peak_level[i] = 0.0f;
            s_peak_visible[i] = false;
        }
    }

    if (s_spectrum_obj) {
        lv_obj_invalidate(s_spectrum_obj);
    }
}
