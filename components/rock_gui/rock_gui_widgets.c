// Shared GUI widgets, phase-4 split out of rock_gui.c: the button boilerplate,
// the modal state overlay (former show_loading/empty/offline/error quartet)
// and the station list row shared by the Browse and Search views.

#include "rock_gui_internal.h"
#include "rock_ui_tokens.h"
#include "rock_fonts.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "rock-gui";

// ---------------------------------------------------------------- button ---

lv_obj_t *gui_make_button(lv_obj_t *parent, int32_t w, int32_t h, const char *label,
                          const lv_font_t *font, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(btn, ROCK_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, ROCK_COLOR_FG, 0);
    lv_obj_center(lbl);
    return btn;
}

void gui_on_back_to_np_click(lv_event_t *e)
{
    (void)e;
    gui_switch_view(VIEW_NOW_PLAYING);
}

// ------------------------------------------------------------ station row ---

static void on_station_row_click(lv_event_t *e)
{
    station_row_ui_t *row_data = (station_row_ui_t *)lv_event_get_user_data(e);
    if (!row_data || row_data->station_id[0] == '\0') return;

    lv_obj_t *ticker = gui_ticker_label();
    if (ticker) {
        lv_label_set_text_fmt(ticker, "Starting: %s...", lv_label_get_text(row_data->name_lbl));
    }
    gui_set_station_id(row_data->station_id);
    gui_now_playing_set_station(lv_label_get_text(row_data->name_lbl),
                                lv_label_get_text(row_data->desc_lbl),
                                lv_label_get_text(row_data->codec_lbl));
    gui_enqueue_event(ROCK_UI_EVT_PLAY_STATION, row_data->station_id, 0);
    gui_switch_view(VIEW_NOW_PLAYING);
}

void gui_station_row_setup(station_row_ui_t *row_ui, lv_obj_t *parent, int index)
{
    row_ui->row = lv_obj_create(parent);
    lv_obj_remove_style_all(row_ui->row);
    lv_obj_set_size(row_ui->row, 768, ROCK_LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(row_ui->row, (index % 2 == 0) ? ROCK_COLOR_PANEL : ROCK_COLOR_ROW_ALT, 0);
    lv_obj_set_style_bg_opa(row_ui->row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row_ui->row, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_pad_left(row_ui->row, 12, 0);
    lv_obj_set_style_pad_right(row_ui->row, 16, 0);
    lv_obj_set_flex_flow(row_ui->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_ui->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row_ui->row, on_station_row_click, LV_EVENT_CLICKED, row_ui);

    // Left Icon box 40x40
    row_ui->icon_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(row_ui->icon_box);
    lv_obj_set_size(row_ui->icon_box, 40, 40);
    lv_obj_set_style_bg_color(row_ui->icon_box, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(row_ui->icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row_ui->icon_box, 4, 0);

    row_ui->icon_lbl = lv_label_create(row_ui->icon_box);
    lv_label_set_text(row_ui->icon_lbl, "R");
    lv_obj_set_style_text_font(row_ui->icon_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->icon_lbl, ROCK_COLOR_MUTED, 0);
    lv_obj_center(row_ui->icon_lbl);

    // Text box (name + desc)
    lv_obj_t *text_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(text_box);
    lv_obj_set_size(text_box, 500, 48);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(text_box, 12, 0);

    row_ui->name_lbl = lv_label_create(text_box);
    lv_label_set_text(row_ui->name_lbl, "Station Name");
    lv_label_set_long_mode(row_ui->name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row_ui->name_lbl, 490);
    lv_obj_set_style_text_font(row_ui->name_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->name_lbl, ROCK_COLOR_FG, 0);

    row_ui->desc_lbl = lv_label_create(text_box);
    lv_label_set_text(row_ui->desc_lbl, "Genre / Tags");
    lv_label_set_long_mode(row_ui->desc_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row_ui->desc_lbl, 490);
    lv_obj_set_style_text_font(row_ui->desc_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(row_ui->desc_lbl, ROCK_COLOR_MUTED, 0);

    // Codec capsule
    row_ui->codec_lbl = lv_label_create(row_ui->row);
    lv_label_set_text(row_ui->codec_lbl, "128k");
    lv_obj_set_style_text_font(row_ui->codec_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(row_ui->codec_lbl, ROCK_COLOR_MUTED, 0);
    lv_obj_set_width(row_ui->codec_lbl, 70);

    // Play action indicator
    row_ui->play_lbl = lv_label_create(row_ui->row);
    lv_label_set_text(row_ui->play_lbl, ROCK_ICON_PLAY);
    lv_obj_set_style_text_font(row_ui->play_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->play_lbl, ROCK_COLOR_ACCENT, 0);

    lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
}

void gui_station_rows_fill(station_row_ui_t *rows, const rock_gui_station_item_t *items,
                           size_t limit, bool log_rows)
{
    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        station_row_ui_t *row_ui = &rows[i];
        if (i < limit && items) {
            strncpy(row_ui->station_id, items[i].id, sizeof(row_ui->station_id) - 1);
            lv_label_set_text(row_ui->name_lbl, items[i].name[0] ? items[i].name : items[i].id);
            if (log_rows) {
                ESP_LOGI(TAG, "Search row [%d]: id='%s' name='%s'", (int)i, items[i].id, items[i].name);
            }

            char initial[4] = "R";
            if (items[i].name[0]) {
                size_t n = ((uint8_t)items[i].name[0] < 0x80) ? 1 : (((uint8_t)items[i].name[0] < 0xE0) ? 2 : 3);
                memcpy(initial, items[i].name, n);
                initial[n] = '\0';
            }
            lv_label_set_text(row_ui->icon_lbl, initial);

            char sub[128];
            snprintf(sub, sizeof(sub), "%s", items[i].tags[0] ? items[i].tags : "Radio");
            lv_label_set_text(row_ui->desc_lbl, sub);

            char cdc[32];
            if (items[i].bitrate_kbps > 0) {
                snprintf(cdc, sizeof(cdc), "%luk %s", (unsigned long)items[i].bitrate_kbps,
                         items[i].codec[0] ? items[i].codec : "MP3");
            } else {
                snprintf(cdc, sizeof(cdc), "%s", items[i].codec[0] ? items[i].codec : "MP3");
            }
            lv_label_set_text(row_ui->codec_lbl, cdc);

            lv_obj_remove_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            row_ui->station_id[0] = '\0';
            lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ----------------------------------------------------------- state overlay ---

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_overlay_spinner = NULL;
static lv_obj_t *s_overlay_icon = NULL;
static lv_obj_t *s_overlay_title = NULL;
static lv_obj_t *s_overlay_desc = NULL;
static lv_obj_t *s_overlay_btn1 = NULL;
static lv_obj_t *s_overlay_btn2 = NULL;

static void on_overlay_retry_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_state();
    gui_enqueue_event(ROCK_UI_EVT_RETRY, NULL, 0);
}

static void on_overlay_back_click(lv_event_t *e)
{
    (void)e;
    rock_gui_dismiss_state();
    if (gui_active_view() == VIEW_SEARCH) gui_search_begin_edit();
    else gui_switch_view(VIEW_NOW_PLAYING);
}

void gui_overlay_build(lv_obj_t *parent)
{
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, 800, 326);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, ROCK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 520, 260);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 12, 0);

    s_overlay_spinner = lv_spinner_create(card);
    lv_obj_set_size(s_overlay_spinner, 48, 48);
    lv_obj_set_style_arc_color(s_overlay_spinner, ROCK_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_overlay_spinner, ROCK_COLOR_PANEL_2, LV_PART_MAIN);

    s_overlay_icon = lv_label_create(card);
    lv_label_set_text(s_overlay_icon, ROCK_ICON_BOLT);
    lv_obj_set_style_text_font(s_overlay_icon, &font_inter_24, 0);
    lv_obj_set_style_text_color(s_overlay_icon, ROCK_COLOR_WARN, 0);
    lv_obj_add_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);

    s_overlay_title = lv_label_create(card);
    lv_label_set_text(s_overlay_title, "Загрузка...");
    lv_obj_set_style_text_font(s_overlay_title, &font_inter_18, 0);
    lv_obj_set_style_text_color(s_overlay_title, ROCK_COLOR_FG, 0);

    s_overlay_desc = lv_label_create(card);
    lv_label_set_text(s_overlay_desc, "Пожалуйста, подождите");
    lv_obj_set_style_text_font(s_overlay_desc, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_overlay_desc, ROCK_COLOR_MUTED, 0);

    lv_obj_t *btn_box = lv_obj_create(card);
    lv_obj_remove_style_all(btn_box);
    lv_obj_set_size(btn_box, LV_SIZE_CONTENT, 48);
    lv_obj_set_flex_flow(btn_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_box, 16, 0);

    s_overlay_btn1 = gui_make_button(btn_box, 160, 44, "Повторить",
                                     &font_inter_14, on_overlay_retry_click, NULL);

    s_overlay_btn2 = gui_make_button(btn_box, 160, 44, "Назад",
                                     &font_inter_14, on_overlay_back_click, NULL);
    lv_obj_set_style_bg_color(s_overlay_btn2, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_overlay_btn2, 0), ROCK_COLOR_WHITE, 0);

    lv_obj_add_flag(btn_box, LV_OBJ_FLAG_HIDDEN);
}

void gui_overlay_set(gui_overlay_kind_t kind, const char *title, const char *desc,
                     bool show_buttons)
{
    if (!s_overlay) return;

    // Per-kind presentation, unified from the former quartet. Note the
    // deliberate title/desc asymmetry: error keeps a fixed title and routes
    // the message to desc; the other kinds do the opposite. Icon colors are
    // raw hex: lv_color_hex() is not a constant expression here.
    static const struct {
        bool spinner;
        bool icon;
        const char *icon_text;
        uint32_t icon_color_hex;
        const char *default_title;
        const char *default_desc;
    } kinds[] = {
        [GUI_OVERLAY_LOADING] = { .spinner = true,  .icon = false,
            .default_title = "Загрузка...",
            .default_desc = "Пожалуйста, подождите" },
        [GUI_OVERLAY_EMPTY] = { .spinner = false, .icon = false,
            .default_title = "Ничего не найдено",
            .default_desc = "Попробуйте изменить поисковый запрос" },
        [GUI_OVERLAY_OFFLINE] = { .spinner = false, .icon = true,
            .icon_text = ROCK_ICON_BOLT, .icon_color_hex = 0xFFAA00,
            .default_title = "Сервер недоступен",
            .default_desc = "Проверьте сеть. Управление плеером доступно." },
        [GUI_OVERLAY_ERROR] = { .spinner = false, .icon = true,
            .icon_text = ROCK_ICON_CROSS, .icon_color_hex = 0xE05353,
            .default_title = "Ошибка воспроизведения",
            .default_desc = "Поток станции временно недоступен" },
    };
    const typeof(kinds[0]) *k = &kinds[kind];

    if (s_overlay_spinner) {
        if (k->spinner) lv_obj_remove_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_overlay_icon) {
        if (k->icon) {
            lv_label_set_text(s_overlay_icon, k->icon_text);
            lv_obj_set_style_text_color(s_overlay_icon, lv_color_hex(k->icon_color_hex), 0);
            lv_obj_remove_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_overlay_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_overlay_title) lv_label_set_text(s_overlay_title, title ? title : k->default_title);
    if (s_overlay_desc) lv_label_set_text(s_overlay_desc, desc ? desc : k->default_desc);
    if (s_overlay_btn1) {
        if (show_buttons) lv_obj_remove_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lv_obj_get_parent(s_overlay_btn1), LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void gui_overlay_hide(void)
{
    if (!s_overlay) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool gui_overlay_trigger(const char *action)
{
    if (!strcmp(action, "RETRY")) {
        if (s_overlay_btn1) lv_obj_send_event(s_overlay_btn1, LV_EVENT_CLICKED, NULL);
        return true;
    }
    if (!strcmp(action, "BACK")) {
        if (s_overlay_btn2) lv_obj_send_event(s_overlay_btn2, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return false;
}
