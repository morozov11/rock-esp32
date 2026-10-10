// Search view, phase-4 split out of rock_gui.c: query input, genre chips,
// virtual keyboard and the results list.

#include "rock_gui_internal.h"
#include "rockcast_tokens.h"
#include "rock_fonts.h"

#include <string.h>

static lv_obj_t *s_view_search = NULL;
static lv_obj_t *s_search_header = NULL;
static lv_obj_t *s_ta_search = NULL;
static lv_obj_t *s_cont_chips = NULL;
static lv_obj_t *s_kb_search = NULL;
static lv_obj_t *s_list_search = NULL;
static station_row_ui_t s_search_rows[ROCK_GUI_MAX_STATIONS];
static size_t s_search_rows_count = 0;

static void on_genre_chip_click(lv_event_t *e)
{
    const char *genre = (const char *)lv_event_get_user_data(e);
    if (!genre) return;
    if (s_ta_search) {
        lv_textarea_set_text(s_ta_search, genre);
    }
    gui_enqueue_event(ROCK_UI_EVT_CATALOG_SEARCH, genre, 0);
    rock_gui_show_loading("Поиск по жанру...");
}

void gui_search_begin_edit(void)
{
    if (s_kb_search) lv_obj_remove_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
    if (s_list_search) lv_obj_add_flag(s_list_search, LV_OBJ_FLAG_HIDDEN);
}

static void on_search_edit(lv_event_t *e)
{
    (void)e;
    gui_search_begin_edit();
}

static void on_search_clear_click(lv_event_t *e)
{
    on_search_edit(e);
    if (s_ta_search) {
        lv_textarea_set_text(s_ta_search, "");
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *txt = lv_textarea_get_text(s_ta_search);
        if (txt && txt[0] != '\0') {
            gui_enqueue_event(ROCK_UI_EVT_CATALOG_SEARCH, txt, 0);
            rock_gui_show_loading("Поиск станций...");
        }
    }
}

void gui_search_build(lv_obj_t *parent)
{
    s_view_search = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_search);
    lv_obj_set_size(s_view_search, 800, 404);
    lv_obj_set_pos(s_view_search, 0, 0);
    lv_obj_add_flag(s_view_search, LV_OBJ_FLAG_HIDDEN);

    // Header bar with Back + Input + Clear
    s_search_header = lv_obj_create(s_view_search);
    lv_obj_remove_style_all(s_search_header);
    lv_obj_set_size(s_search_header, 800, 48);
    lv_obj_set_pos(s_search_header, 0, 0);
    lv_obj_set_style_pad_left(s_search_header, 16, 0);
    lv_obj_set_style_pad_right(s_search_header, 16, 0);
    lv_obj_set_flex_flow(s_search_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_search_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    gui_make_button(s_search_header, 110, 40, ROCK_ICON_ARROW_LEFT " Назад",
                    &font_inter_14, gui_on_back_to_np_click, NULL);

    s_ta_search = lv_textarea_create(s_search_header);
    lv_obj_set_size(s_ta_search, 520, 40);
    lv_obj_set_style_bg_color(s_ta_search, ROCK_COLOR_PANEL_2, 0);
    lv_obj_set_style_text_color(s_ta_search, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_ta_search, &font_inter_18, 0);
    lv_obj_set_style_radius(s_ta_search, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_border_color(s_ta_search, ROCK_COLOR_STROKE, 0);
    lv_obj_set_style_border_width(s_ta_search, 1, 0);
    lv_textarea_set_one_line(s_ta_search, true);
    lv_textarea_set_max_length(s_ta_search, 128);
    lv_obj_add_event_cb(s_ta_search, on_search_edit, LV_EVENT_CLICKED, NULL);
    lv_textarea_set_placeholder_text(s_ta_search, "Поиск станций...");

    lv_obj_t *btn_clear = gui_make_button(s_search_header, 90, 40, "Очистить",
                                          &font_inter_14, on_search_clear_click, NULL);
    lv_obj_set_style_pad_left(btn_clear, 8, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn_clear, 0), ROCK_COLOR_MUTED, 0);

    // Genre Chips Container (x=16, y=50, w=768, h=36)
    s_cont_chips = lv_obj_create(s_view_search);
    lv_obj_remove_style_all(s_cont_chips);
    lv_obj_set_size(s_cont_chips, 768, 36);
    lv_obj_set_pos(s_cont_chips, 16, 50);
    lv_obj_set_flex_flow(s_cont_chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_cont_chips, 8, 0);

    static const char *genres[] = {"Rock", "Pop", "Jazz", "Metal", "Classical", "News", "Electronic"};
    for (size_t i = 0; i < sizeof(genres) / sizeof(genres[0]); i++) {
        lv_obj_t *chip = gui_make_button(s_cont_chips, LV_SIZE_CONTENT, 32, genres[i],
                                         &font_inter_14, on_genre_chip_click, (void *)genres[i]);
        lv_obj_set_style_radius(chip, 16, 0);
        lv_obj_set_style_pad_left(chip, 12, 0);
        lv_obj_set_style_pad_right(chip, 12, 0);
    }

    // Results container (scrollable list, initially hidden or under keyboard)
    s_list_search = lv_obj_create(s_view_search);
    lv_obj_remove_style_all(s_list_search);
    lv_obj_set_size(s_list_search, 768, 308);
    lv_obj_set_pos(s_list_search, 16, 90);
    lv_obj_set_flex_flow(s_list_search, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_search, 4, 0);
    lv_obj_add_flag(s_list_search, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        gui_station_row_setup(&s_search_rows[i], s_list_search, i);
    }

    // On-Screen Virtual Keyboard (x=16, y=92, w=768, h=220)
    // Four rows with 55 px each before gaps (satisfies the 44 px target).
    s_kb_search = lv_keyboard_create(s_view_search);
    lv_obj_set_size(s_kb_search, 768, 220);
    lv_obj_set_align(s_kb_search, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_kb_search, 16, 92);
    lv_keyboard_set_textarea(s_kb_search, s_ta_search);
    gui_keyboard_install(s_kb_search);
    lv_obj_set_style_pad_all(s_kb_search, 4, 0);
    lv_obj_set_style_pad_row(s_kb_search, 4, 0);
    lv_obj_set_style_pad_column(s_kb_search, 4, 0);
    lv_obj_set_style_text_font(s_kb_search, &font_inter_18, 0);
    lv_obj_set_style_bg_color(s_kb_search, ROCK_COLOR_PANEL, 0);
    lv_obj_add_event_cb(s_kb_search, on_kb_event, LV_EVENT_READY, NULL);
}

void gui_search_show(void)
{
    if (s_view_search) lv_obj_remove_flag(s_view_search, LV_OBJ_FLAG_HIDDEN);
}

void gui_search_hide(void)
{
    if (s_view_search) lv_obj_add_flag(s_view_search, LV_OBJ_FLAG_HIDDEN);
}

void gui_search_set_results(const rock_gui_station_item_t *items, size_t count)
{
    size_t limit = count > ROCK_GUI_MAX_STATIONS ? ROCK_GUI_MAX_STATIONS : count;
    s_search_rows_count = limit;

    gui_station_rows_fill(s_search_rows, items, limit, true);

    if (s_kb_search) lv_obj_add_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
    if (s_list_search) lv_obj_remove_flag(s_list_search, LV_OBJ_FLAG_HIDDEN);

    gui_overlay_hide();
    gui_switch_view(VIEW_SEARCH);
}

void gui_search_hide_rows(void)
{
    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        lv_obj_add_flag(s_search_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }
}

station_row_ui_t *gui_search_rows(void)
{
    return s_search_rows;
}

size_t gui_search_rows_count(void)
{
    return s_search_rows_count;
}

bool gui_search_trigger(const char *action)
{
    if (!strcmp(action, "SEARCH")) {
        // Formerly: on_search_edit(NULL) + click on the transport Search
        // button (whose callback only switches to this view).
        gui_search_begin_edit();
        gui_switch_view(VIEW_SEARCH);
        return true;
    }
    if (!strncmp(action, "SEARCH:", 7)) {
        gui_switch_view(VIEW_SEARCH);
        lv_textarea_set_text(s_ta_search, action + 7);
        lv_obj_send_event(s_kb_search, LV_EVENT_READY, NULL);
        return true;
    }
    return false;
}
