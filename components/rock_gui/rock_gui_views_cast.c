// Cast output menu, phase-4 split out of rock_gui.c: the output-selection
// overlay (device list, local speaker row, scan status). Phase 3 restyle per
// docs/design/phase3-mockup.html; row set and handlers are unchanged.

#include "rock_gui_internal.h"
#include "rock_ui_tokens.h"
#include "rock_fonts.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *lbl_name;
    lv_obj_t *badge;       // "Играет" box (dot + text), hidden unless active
    lv_obj_t *badge_dot;
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

// One output row: PANEL_2 r8 + BORDER_SUBTLE, 40x40 icon box, name, status badge.
static lv_obj_t *cast_make_badge(lv_obj_t *parent, const char *text, lv_obj_t **dot_out)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(badge, 7, 0);

    lv_obj_t *dot = lv_obj_create(badge);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, ROCK_COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(badge);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl, ROCK_COLOR_GREEN, 0);

    if (dot_out) *dot_out = dot;
    return badge;
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
    lv_obj_set_size(card, 600, 310);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);

    // Top Header: Title, Refresh (44px), Close (44px)
    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, 44);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 12, 0);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Выход звука");
    lv_obj_set_style_text_font(title, &font_inter_18, 0);
    lv_obj_set_style_text_color(title, ROCK_COLOR_FG, 0);
    lv_obj_set_flex_grow(title, 1);

    lv_obj_t *btn_refresh = gui_make_button(hdr, 120, 44, "Обновить",
                                            &font_inter_14, on_cast_refresh_click, NULL);
    lv_obj_set_style_radius(btn_refresh, 22, 0);
    lv_obj_set_style_border_color(btn_refresh, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn_refresh, 1, 0);

    lv_obj_t *btn_close = gui_make_button(hdr, 44, 44, ROCK_ICON_CROSS,
                                          &font_inter_18, on_cast_close_click, NULL);
    lv_obj_set_style_radius(btn_close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(btn_close, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn_close, 1, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn_close, 0), ROCK_COLOR_MUTED, 0);

    // Scrollable List container
    s_cast_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_cast_list);
    lv_obj_set_size(s_cast_list, lv_pct(100), 200);
    lv_obj_set_flex_flow(s_cast_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cast_list, 6, 0);

    // Row 0: Local speaker
    s_cast_local_btn = lv_button_create(s_cast_list);
    lv_obj_set_size(s_cast_local_btn, lv_pct(100), 52);
    lv_obj_set_style_bg_color(s_cast_local_btn, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_border_color(s_cast_local_btn, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(s_cast_local_btn, 1, 0);
    lv_obj_set_style_radius(s_cast_local_btn, ROCK_RADIUS_WIDGET, 0);
    lv_obj_set_style_pad_left(s_cast_local_btn, 12, 0);
    lv_obj_set_style_pad_right(s_cast_local_btn, 12, 0);
    lv_obj_set_style_pad_column(s_cast_local_btn, 12, 0);
    lv_obj_set_flex_flow(s_cast_local_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cast_local_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_cast_local_btn, on_cast_local_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *loc_ibox = lv_obj_create(s_cast_local_btn);
    lv_obj_remove_style_all(loc_ibox);
    lv_obj_set_size(loc_ibox, 40, 40);
    lv_obj_set_style_bg_color(loc_ibox, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(loc_ibox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(loc_ibox, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(loc_ibox, 1, 0);
    lv_obj_set_style_radius(loc_ibox, ROCK_RADIUS_WIDGET, 0);
    lv_obj_t *loc_ico = lv_label_create(loc_ibox);
    lv_label_set_text(loc_ico, ROCK_ICON_BOLT);
    lv_obj_set_style_text_font(loc_ico, &font_inter_18, 0);
    lv_obj_set_style_text_color(loc_ico, ROCK_COLOR_FG, 0);
    lv_obj_center(loc_ico);

    lv_obj_t *lbl_loc = lv_label_create(s_cast_local_btn);
    lv_label_set_text(lbl_loc, "Динамик радио (локальный)");
    lv_obj_set_style_text_font(lbl_loc, &font_inter_14, 0);
    lv_obj_set_style_text_color(lbl_loc, ROCK_COLOR_FG, 0);
    lv_obj_set_flex_grow(lbl_loc, 1);

    s_cast_local_badge = cast_make_badge(s_cast_local_btn, "Активен", NULL);

    // Status box (spinner + label)
    lv_obj_t *status_box = lv_obj_create(s_cast_list);
    lv_obj_remove_style_all(status_box);
    lv_obj_set_size(status_box, lv_pct(100), 36);
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
        lv_obj_set_size(btn, lv_pct(100), 52);
        lv_obj_set_style_bg_color(btn, ROCK_COLOR_PANEL_2, 0);
        lv_obj_set_style_border_color(btn, ROCK_COLOR_BORDER_SUBTLE, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, ROCK_RADIUS_WIDGET, 0);
        lv_obj_set_style_pad_left(btn, 12, 0);
        lv_obj_set_style_pad_right(btn, 12, 0);
        lv_obj_set_style_pad_column(btn, 12, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(btn, on_cast_device_row_click, LV_EVENT_CLICKED, &s_cast_rows[i]);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *ibox = lv_obj_create(btn);
        lv_obj_remove_style_all(ibox);
        lv_obj_set_size(ibox, 40, 40);
        lv_obj_set_style_bg_color(ibox, ROCK_COLOR_PANEL, 0);
        lv_obj_set_style_bg_opa(ibox, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(ibox, ROCK_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(ibox, 1, 0);
        lv_obj_set_style_radius(ibox, ROCK_RADIUS_WIDGET, 0);
        lv_obj_t *ico = lv_label_create(ibox);
        lv_label_set_text(ico, ROCK_ICON_WIFI);
        lv_obj_set_style_text_font(ico, &font_inter_18, 0);
        lv_obj_set_style_text_color(ico, ROCK_COLOR_FG, 0);
        lv_obj_center(ico);

        lv_obj_t *lbl_name = lv_label_create(btn);
        lv_label_set_text(lbl_name, "");
        lv_obj_set_style_text_font(lbl_name, &font_inter_14, 0);
        lv_obj_set_style_text_color(lbl_name, ROCK_COLOR_FG, 0);
        lv_obj_set_flex_grow(lbl_name, 1);

        s_cast_rows[i].btn = btn;
        s_cast_rows[i].lbl_name = lbl_name;
        s_cast_rows[i].badge = cast_make_badge(btn, "Играет", &s_cast_rows[i].badge_dot);
        s_cast_rows[i].lbl_badge = NULL; // text label lives inside the badge box
        lv_obj_add_flag(s_cast_rows[i].badge, LV_OBJ_FLAG_HIDDEN);
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
            // Active receiver: full ACCENT row (dark text, white dot) per mockup
            if (items[i].is_active) {
                lv_obj_set_style_bg_color(s_cast_rows[i].btn, ROCK_COLOR_ACCENT, 0);
                lv_obj_set_style_border_color(s_cast_rows[i].btn, ROCK_COLOR_ACCENT, 0);
                lv_obj_set_style_text_color(s_cast_rows[i].lbl_name, ROCK_COLOR_BG, 0);
                if (s_cast_rows[i].badge_dot) {
                    lv_obj_set_style_bg_color(s_cast_rows[i].badge_dot, ROCK_COLOR_WHITE, 0);
                }
                lv_obj_t *badge_lbl = s_cast_rows[i].badge
                    ? lv_obj_get_child(s_cast_rows[i].badge, 1) : NULL;
                if (badge_lbl) lv_obj_set_style_text_color(badge_lbl, ROCK_COLOR_SEL_TEXT_SEC, 0);
                if (s_cast_rows[i].badge) lv_obj_remove_flag(s_cast_rows[i].badge, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_bg_color(s_cast_rows[i].btn, ROCK_COLOR_PANEL_2, 0);
                lv_obj_set_style_border_color(s_cast_rows[i].btn, ROCK_COLOR_BORDER_SUBTLE, 0);
                lv_obj_set_style_text_color(s_cast_rows[i].lbl_name, ROCK_COLOR_FG, 0);
                if (s_cast_rows[i].badge_dot) {
                    lv_obj_set_style_bg_color(s_cast_rows[i].badge_dot, ROCK_COLOR_GREEN, 0);
                }
                lv_obj_t *badge_lbl = s_cast_rows[i].badge
                    ? lv_obj_get_child(s_cast_rows[i].badge, 1) : NULL;
                if (badge_lbl) lv_obj_set_style_text_color(badge_lbl, ROCK_COLOR_GREEN, 0);
                if (s_cast_rows[i].badge) lv_obj_add_flag(s_cast_rows[i].badge, LV_OBJ_FLAG_HIDDEN);
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
    }
    return false;
}
