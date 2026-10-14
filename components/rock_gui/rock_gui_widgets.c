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

// -------------------------------------------------------------- monogram ---

uint32_t rock_ui_station_color(const char *name)
{
    static const uint32_t palette[] = {
        0x382216, 0x481A16, 0x302824, 0x522814,
        0x3D2C1D, 0x441B17, 0x282523, 0x5A2E15,
    };
    uint32_t h = 0;
    for (const uint8_t *p = (const uint8_t *)(name ? name : ""); *p; p++) {
        h = h * 31u + *p;
    }
    return palette[h % 8];
}

// First 1-2 alphanumeric codepoints (ASCII or Cyrillic), uppercased, UTF-8.
static void initials_of(const char *name, char *out, size_t out_sz)
{
    size_t n = 0;
    int taken = 0;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p && taken < 2; p++) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 1;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 2;
        } else {
            continue; // surrogate / continuation garbage: skip the byte
        }
        bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                     (cp >= 0x0410 && cp <= 0x044F) || cp == 0x0401 || cp == 0x0451;
        if (!alnum) continue;
        if (cp >= 'a' && cp <= 'z') cp -= 0x20;
        else if (cp >= 0x0430 && cp <= 0x044F) cp -= 0x20;
        else if (cp == 0x0451) cp = 0x0401;
        if (cp < 0x80 && n + 1 < out_sz) {
            out[n++] = (char)cp;
        } else if (cp < 0x800 && n + 2 < out_sz) {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (n + 3 < out_sz) {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            break;
        }
        taken++;
    }
    if (n == 0 && out_sz > 1) out[n++] = '?';
    out[n] = '\0';
}

void gui_monogram_apply(lv_obj_t *box, lv_obj_t *lbl, const char *name)
{
    char initials[8];
    initials_of(name, initials, sizeof(initials));
    lv_obj_set_style_bg_color(box, lv_color_hex(rock_ui_station_color(name)), 0);
    lv_label_set_text(lbl, initials);
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
    lv_obj_set_style_pad_left(row_ui->row, 14, 0);
    lv_obj_set_style_pad_right(row_ui->row, 16, 0);
    lv_obj_set_style_pad_column(row_ui->row, 12, 0);
    lv_obj_set_flex_flow(row_ui->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_ui->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row_ui->row, on_station_row_click, LV_EVENT_CLICKED, row_ui);

    // Station monogram 40x40 (fill/initials set per name in gui_station_rows_fill)
    row_ui->icon_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(row_ui->icon_box);
    lv_obj_set_size(row_ui->icon_box, 40, 40);
    lv_obj_set_style_bg_opa(row_ui->icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row_ui->icon_box, ROCK_RADIUS_WIDGET, 0);

    row_ui->icon_lbl = lv_label_create(row_ui->icon_box);
    lv_label_set_text(row_ui->icon_lbl, "?");
    lv_obj_set_style_text_font(row_ui->icon_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->icon_lbl, ROCK_COLOR_MONO_TEXT, 0);
    lv_obj_center(row_ui->icon_lbl);

    // Text box (name + desc)
    lv_obj_t *text_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(text_box);
    lv_obj_set_size(text_box, 544, 48);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

    row_ui->name_lbl = lv_label_create(text_box);
    lv_label_set_text(row_ui->name_lbl, "Station Name");
    lv_label_set_long_mode(row_ui->name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row_ui->name_lbl, 540);
    lv_obj_set_style_text_font(row_ui->name_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->name_lbl, ROCK_COLOR_FG, 0);

    row_ui->desc_lbl = lv_label_create(text_box);
    lv_label_set_text(row_ui->desc_lbl, "Genre / Tags");
    lv_label_set_long_mode(row_ui->desc_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row_ui->desc_lbl, 540);
    lv_obj_set_style_text_font(row_ui->desc_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(row_ui->desc_lbl, ROCK_COLOR_MUTED, 0);

    // Codec/bitrate meta, right-aligned
    row_ui->codec_lbl = lv_label_create(row_ui->row);
    lv_label_set_text(row_ui->codec_lbl, "128k");
    lv_obj_set_style_text_font(row_ui->codec_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(row_ui->codec_lbl, ROCK_COLOR_MUTED, 0);
    lv_obj_set_width(row_ui->codec_lbl, 92);
    lv_obj_set_style_text_align(row_ui->codec_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    // Play action indicator (swapped for the mini-eq on the playing row)
    row_ui->play_lbl = lv_label_create(row_ui->row);
    lv_label_set_text(row_ui->play_lbl, ROCK_ICON_PLAY);
    lv_obj_set_style_text_font(row_ui->play_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(row_ui->play_lbl, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_width(row_ui->play_lbl, 26);
    lv_obj_set_style_text_align(row_ui->play_lbl, LV_TEXT_ALIGN_CENTER, 0);

    row_ui->eq_box = lv_obj_create(row_ui->row);
    lv_obj_remove_style_all(row_ui->eq_box);
    lv_obj_set_size(row_ui->eq_box, 26, 16);
    static const int eq_h[4] = {6, 12, 9, 11};
    for (int b = 0; b < 4; b++) {
        lv_obj_t *bar = lv_obj_create(row_ui->eq_box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, eq_h[b]);
        lv_obj_set_pos(bar, b * 5, 16 - eq_h[b]);
        lv_obj_set_style_bg_color(bar, ROCK_COLOR_WHITE, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 1, 0);
    }
    lv_obj_add_flag(row_ui->eq_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
}

void gui_station_rows_fill(station_row_ui_t *rows, const rock_gui_station_item_t *items,
                           size_t limit, bool log_rows)
{
    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        station_row_ui_t *row_ui = &rows[i];
        if (i < limit && items) {
            strncpy(row_ui->station_id, items[i].id, sizeof(row_ui->station_id) - 1);
            const char *display_name = items[i].name[0] ? items[i].name : items[i].id;
            lv_label_set_text(row_ui->name_lbl, display_name);
            if (log_rows) {
                ESP_LOGI(TAG, "Search row [%d]: id='%s' name='%s'", (int)i, items[i].id, items[i].name);
            }

            gui_monogram_apply(row_ui->icon_box, row_ui->icon_lbl, display_name);

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

            // Playing station: full ACCENT row (mockup .row.cur)
            if (items[i].is_playing) {
                lv_obj_set_style_bg_color(row_ui->row, ROCK_COLOR_ACCENT, 0);
                lv_obj_set_style_text_color(row_ui->name_lbl, ROCK_COLOR_BG, 0);
                lv_obj_set_style_text_color(row_ui->desc_lbl, ROCK_COLOR_SEL_TEXT_SEC, 0);
                lv_obj_set_style_text_color(row_ui->codec_lbl, ROCK_COLOR_SEL_TEXT_SEC, 0);
                lv_obj_add_flag(row_ui->play_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(row_ui->eq_box, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_bg_color(row_ui->row, (i % 2 == 0) ? ROCK_COLOR_PANEL : ROCK_COLOR_ROW_ALT, 0);
                lv_obj_set_style_text_color(row_ui->name_lbl, ROCK_COLOR_FG, 0);
                lv_obj_set_style_text_color(row_ui->desc_lbl, ROCK_COLOR_MUTED, 0);
                lv_obj_set_style_text_color(row_ui->codec_lbl, ROCK_COLOR_MUTED, 0);
                lv_obj_remove_flag(row_ui->play_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(row_ui->eq_box, LV_OBJ_FLAG_HIDDEN);
            }

            lv_obj_remove_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            row_ui->station_id[0] = '\0';
            lv_obj_add_flag(row_ui->row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ----------------------------------------------------------- state overlay ---

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_overlay_card = NULL;
static lv_obj_t *s_overlay_spinner = NULL;
static lv_obj_t *s_overlay_icon_circle = NULL;
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
    if (gui_active_view() == VIEW_CATALOG) {
        if (gui_catalog_is_search_active()) {
            gui_catalog_exit_search();
        } else {
            gui_switch_view(VIEW_NOW_PLAYING);
        }
    } else {
        gui_switch_view(VIEW_NOW_PLAYING);
    }
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

    s_overlay_card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_overlay_card);
    lv_obj_set_size(s_overlay_card, 520, 260);
    lv_obj_center(s_overlay_card);
    lv_obj_set_style_bg_color(s_overlay_card, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_overlay_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_overlay_card, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(s_overlay_card, 1, 0);
    lv_obj_set_style_radius(s_overlay_card, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_flex_flow(s_overlay_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_overlay_card, 20, 0);
    lv_obj_set_style_pad_row(s_overlay_card, 14, 0);

    s_overlay_spinner = lv_spinner_create(s_overlay_card);
    lv_obj_set_size(s_overlay_spinner, 56, 56);
    lv_obj_set_style_arc_width(s_overlay_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_overlay_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_overlay_spinner, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_overlay_spinner, ROCK_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_overlay_spinner, ROCK_COLOR_PANEL_2, LV_PART_MAIN);

    s_overlay_icon_circle = lv_obj_create(s_overlay_card);
    lv_obj_remove_style_all(s_overlay_icon_circle);
    lv_obj_set_size(s_overlay_icon_circle, 56, 56);
    lv_obj_set_style_radius(s_overlay_icon_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_overlay_icon_circle, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_opa(s_overlay_icon_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_overlay_icon_circle, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_overlay_icon_circle, 1, 0);
    lv_obj_add_flag(s_overlay_icon_circle, LV_OBJ_FLAG_HIDDEN);

    s_overlay_icon = lv_label_create(s_overlay_icon_circle);
    lv_label_set_text(s_overlay_icon, ROCK_ICON_BOLT);
    lv_obj_set_style_text_font(s_overlay_icon, &font_inter_24, 0);
    lv_obj_set_style_text_color(s_overlay_icon, ROCK_COLOR_WARN, 0);
    lv_obj_center(s_overlay_icon);

    s_overlay_title = lv_label_create(s_overlay_card);
    lv_label_set_text(s_overlay_title, "Загрузка...");
    lv_obj_set_style_text_font(s_overlay_title, &font_inter_18, 0);
    lv_obj_set_style_text_color(s_overlay_title, ROCK_COLOR_FG, 0);

    s_overlay_desc = lv_label_create(s_overlay_card);
    lv_label_set_text(s_overlay_desc, "Пожалуйста, подождите");
    lv_obj_set_style_text_font(s_overlay_desc, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_overlay_desc, ROCK_COLOR_MUTED, 0);

    lv_obj_t *btn_box = lv_obj_create(s_overlay_card);
    lv_obj_remove_style_all(btn_box);
    lv_obj_set_size(btn_box, LV_SIZE_CONTENT, 48);
    lv_obj_set_flex_flow(btn_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_box, 12, 0);

    s_overlay_btn1 = gui_make_button(btn_box, 176, 48, "Повторить",
                                     &font_inter_14, on_overlay_retry_click, NULL);
    lv_obj_set_style_bg_color(s_overlay_btn1, ROCK_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_overlay_btn1, 10, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_overlay_btn1, 0), ROCK_COLOR_WHITE, 0);

    s_overlay_btn2 = gui_make_button(btn_box, 176, 48, "Назад",
                                     &font_inter_14, on_overlay_back_click, NULL);
    lv_obj_set_style_radius(s_overlay_btn2, 10, 0);

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

    if (s_overlay_card) {
        lv_obj_set_height(s_overlay_card, kind == GUI_OVERLAY_ERROR ? 280 : 260);
    }
    if (s_overlay_spinner) {
        if (k->spinner) lv_obj_remove_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_overlay_icon_circle) {
        if (k->icon) {
            lv_label_set_text(s_overlay_icon, k->icon_text);
            lv_obj_set_style_text_color(s_overlay_icon, lv_color_hex(k->icon_color_hex), 0);
            lv_obj_remove_flag(s_overlay_icon_circle, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_overlay_icon_circle, LV_OBJ_FLAG_HIDDEN);
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
    if (!s_overlay || lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) return false;
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
