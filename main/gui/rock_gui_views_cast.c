// Cast output menu, phase-4 split out of rock_gui.c: the output-selection
// overlay (device list, local speaker row, scan status).

#include "rock_gui_internal.h"
#include "rockcast_tokens.h"
#include "rock_fonts.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_badge;
    char receiver_id[65];
} cast_device_row_t;

static lv_obj_t *s_cast_overlay = NULL;
static lv_obj_t *s_cast_status_lbl = NULL;
static lv_obj_t *s_cast_spinner = NULL;
static lv_obj_t *s_cast_list = NULL;
static lv_obj_t *s_cast_local_btn = NULL;
static lv_obj_t *s_cast_local_badge = NULL;

static cast_device_row_t s_cast_rows[ROCK_GUI_MAX_CAST_DEVICES];
static size_t s_cast_rows_count = 0;

static void on_cast_close_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_cast_menu();
}

static void on_cast_local_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_cast_menu();
    gui_enqueue_event(ROCK_UI_EVT_CAST_DISCONNECT, NULL, 0);
}

static void on_cast_refresh_click(lv_event_t *e)
{
    (void)e;
    if (s_cast_spinner) lv_obj_remove_flag(s_cast_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_cast_status_lbl) {
        lv_label_set_text(s_cast_status_lbl, "Поиск устройств Chromecast...");
        lv_obj_remove_flag(s_cast_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    gui_enqueue_event(ROCK_UI_EVT_CAST_BROWSE, NULL, 0);
}

static void on_cast_device_row_click(lv_event_t *e)
{
    cast_device_row_t *row = (cast_device_row_t *)lv_event_get_user_data(e);
    if (!row || row->receiver_id[0] == '\0') return;
    rock_gui_dismiss_cast_menu();
    gui_enqueue_event(ROCK_UI_EVT_CAST_SELECT, row->receiver_id, 0);
}

void gui_cast_build(lv_obj_t *parent)
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

    gui_make_button(hdr, 100, 32, "Обновить", &font_inter_14, on_cast_refresh_click, NULL);

    lv_obj_t *btn_close = gui_make_button(hdr, 36, 32, "X", &font_inter_14,
                                          on_cast_close_click, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(btn_close, 0), ROCK_COLOR_MUTED, 0);

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

void gui_cast_hide(void)
{
    if (!s_cast_overlay) return;
    lv_obj_add_flag(s_cast_overlay, LV_OBJ_FLAG_HIDDEN);
}

void gui_cast_menu_show(bool scanning)
{
    if (!s_cast_overlay) return;

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
}

void gui_cast_set_devices(const rock_gui_cast_item_t *items, size_t count)
{
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
}

void gui_cast_menu_dismiss(void)
{
    if (!s_cast_overlay) return;
    lv_obj_add_flag(s_cast_overlay, LV_OBJ_FLAG_HIDDEN);
}

void gui_cast_apply_active_output(bool is_casting)
{
    if (!s_cast_local_badge) return;
    if (is_casting) {
        lv_obj_add_flag(s_cast_local_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_cast_local_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

bool gui_cast_trigger(const char *action)
{
    if (!strncmp(action, "CAST:SELECT:", 12)) {
        int row = atoi(action + 12);
        if (row >= 0 && row < ROCK_GUI_MAX_CAST_DEVICES && s_cast_rows[row].btn &&
            !lv_obj_has_flag(s_cast_rows[row].btn, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_send_event(s_cast_rows[row].btn, LV_EVENT_CLICKED, NULL);
        }
        return true;
    }
    if (!strcmp(action, "CAST:LOCAL")) {
        if (s_cast_local_btn) lv_obj_send_event(s_cast_local_btn, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return false;
}
