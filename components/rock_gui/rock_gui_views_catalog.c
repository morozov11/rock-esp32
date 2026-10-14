// Catalog view (UC-3): the single station-list screen, merged from the former
// Browse and Search views. One search bar + genre chips on top, one row list
// below; the virtual keyboard overlays the list area while editing. Catalog
// pages and search results both land in the same rows via gui_catalog_set_rows
// (search_results only toggles which side-effects apply). Data behavior is
// unchanged from the two-view era: a page/result set replaces the list.

#include "rock_gui_internal.h"
#include "rock_ui_tokens.h"
#include "rock_fonts.h"

#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

static const char *TAG = "rock_gui_cat";

static lv_obj_t *s_view_catalog = NULL;
static lv_obj_t *s_ta_query = NULL;
static lv_obj_t *s_lbl_count = NULL;
static lv_obj_t *s_kb_catalog = NULL;
static lv_obj_t *s_list_catalog = NULL;
static lv_obj_t *s_pill_box = NULL;
static lv_obj_t *s_lbl_pill = NULL;
static station_row_ui_t s_catalog_rows[ROCK_GUI_MAX_STATIONS];
static size_t s_catalog_rows_count = 0;

// UC-4: Accumulated station catalog in PSRAM + 20-row sliding window
static rock_gui_station_item_t *s_catalog = NULL;
static size_t s_catalog_count = 0;
static char s_catalog_cursor[513] = {0};
static size_t s_window_start = 0;
static bool s_fetching_more = false;
static lv_timer_t *s_fetch_timer = NULL;
static bool s_in_scroll_adjust = false;
static bool s_is_search_mode = false;

static void on_genre_chip_click(lv_event_t *e)
{
    const char *genre = (const char *)lv_event_get_user_data(e);
    if (!genre) return;
    if (s_ta_query) {
        lv_textarea_set_text(s_ta_query, genre);
    }
    gui_enqueue_event(ROCK_UI_EVT_CATALOG_SEARCH, genre, 0);
    rock_gui_show_loading("Поиск по жанру...");
}

void gui_catalog_begin_edit(void)
{
    if (s_kb_catalog) lv_obj_remove_flag(s_kb_catalog, LV_OBJ_FLAG_HIDDEN);
    if (s_list_catalog) lv_obj_add_flag(s_list_catalog, LV_OBJ_FLAG_HIDDEN);
}

static void on_query_edit(lv_event_t *e)
{
    (void)e;
    gui_catalog_begin_edit();
}

static void on_query_clear_click(lv_event_t *e)
{
    on_query_edit(e);
    if (s_ta_query) {
        lv_textarea_set_text(s_ta_query, "");
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *txt = lv_textarea_get_text(s_ta_query);
        if (txt && txt[0] != '\0') {
            gui_enqueue_event(ROCK_UI_EVT_CATALOG_SEARCH, txt, 0);
            rock_gui_show_loading("Поиск станций...");
        }
    }
}

static void on_fetch_timeout(lv_timer_t *timer)
{
    (void)timer;
    s_fetching_more = false;
    if (s_fetch_timer) {
        lv_timer_delete(s_fetch_timer);
        s_fetch_timer = NULL;
    }
    if (s_pill_box) {
        lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGW(TAG, "Catalog fetch more timed out (15s)");
}

void gui_catalog_reset_fetching(void)
{
    if (s_fetch_timer) {
        lv_timer_delete(s_fetch_timer);
        s_fetch_timer = NULL;
    }
    s_fetching_more = false;
    if (s_pill_box) {
        lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool ensure_catalog_buf(void)
{
    if (s_catalog) return true;
    s_catalog = heap_caps_malloc(ROCK_GUI_CATALOG_MAX_ACCUMULATED * sizeof(rock_gui_station_item_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_catalog) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM for catalog (%u bytes)",
                 (unsigned)(ROCK_GUI_CATALOG_MAX_ACCUMULATED * sizeof(rock_gui_station_item_t)));
        return false;
    }
    return true;
}

static void update_count_label(size_t count)
{
    if (!s_lbl_count) return;
    unsigned n = (unsigned)count;
    const char *form = "станций";
    if (n % 10 == 1 && n % 100 != 11) form = "станция";
    else if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) form = "станции";
    lv_label_set_text_fmt(s_lbl_count, "%u %s", n, form);
}

static void update_pill(void)
{
    if (!s_pill_box || !s_lbl_pill) return;
    if (s_fetching_more) {
        lv_label_set_text(s_lbl_pill, "Подгрузка...");
        lv_obj_remove_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
    } else if (s_catalog_cursor[0] == '\0' || s_catalog_count >= ROCK_GUI_CATALOG_MAX_ACCUMULATED) {
        if (s_catalog_count > 0 && !s_is_search_mode) {
            lv_label_set_text(s_lbl_pill, "Конец списка");
            lv_obj_remove_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_window_rows(void)
{
    if (!s_catalog || s_catalog_count == 0) {
        gui_catalog_hide_rows();
        s_catalog_rows_count = 0;
        return;
    }
    if (s_window_start >= s_catalog_count) {
        s_window_start = (s_catalog_count > ROCK_GUI_MAX_STATIONS) ?
                         (s_catalog_count - ROCK_GUI_MAX_STATIONS) : 0;
    }
    size_t remaining = s_catalog_count - s_window_start;
    size_t visible_count = remaining > ROCK_GUI_MAX_STATIONS ? ROCK_GUI_MAX_STATIONS : remaining;
    s_catalog_rows_count = visible_count;
    gui_station_rows_fill(s_catalog_rows, &s_catalog[s_window_start], visible_count, false);
}

static void on_catalog_list_scrolled(lv_event_t *e)
{
    (void)e;
    if (s_in_scroll_adjust) return;
    if (s_is_search_mode) return;
    if (!s_catalog || s_catalog_count == 0) return;

    int32_t bottom = lv_obj_get_scroll_bottom(s_list_catalog);
    int32_t top = lv_obj_get_scroll_top(s_list_catalog);
    int32_t scroll_y = lv_obj_get_scroll_y(s_list_catalog);

    // Near bottom edge of current window:
    if (bottom <= 20) {
        if (s_window_start + ROCK_GUI_MAX_STATIONS < s_catalog_count) {
            size_t step = 10;
            if (s_window_start + ROCK_GUI_MAX_STATIONS + step > s_catalog_count) {
                step = s_catalog_count - (s_window_start + ROCK_GUI_MAX_STATIONS);
            }
            if (step > 0) {
                s_in_scroll_adjust = true;
                s_window_start += step;
                update_window_rows();
                int32_t new_y = scroll_y - (int32_t)(step * 60);
                if (new_y < 0) new_y = 0;
                lv_obj_scroll_to_y(s_list_catalog, new_y, LV_ANIM_OFF);
                s_in_scroll_adjust = false;
            }
        } else if (s_catalog_cursor[0] != '\0' && !s_fetching_more) {
            s_fetching_more = true;
            update_pill();
            if (s_fetch_timer) lv_timer_delete(s_fetch_timer);
            s_fetch_timer = lv_timer_create(on_fetch_timeout, 15000, NULL);
            ESP_LOGI(TAG, "Scrolled to end, requesting browse with cursor='%s'", s_catalog_cursor);
            gui_enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, s_catalog_cursor, 0);
        }
    } else if (top <= 20) {
        // Near top edge of current window:
        if (s_window_start > 0) {
            size_t step = 10;
            if (step > s_window_start) {
                step = s_window_start;
            }
            if (step > 0) {
                s_in_scroll_adjust = true;
                s_window_start -= step;
                update_window_rows();
                int32_t new_y = scroll_y + (int32_t)(step * 60);
                lv_obj_scroll_to_y(s_list_catalog, new_y, LV_ANIM_OFF);
                s_in_scroll_adjust = false;
            }
        }
    }
}

void gui_catalog_build(lv_obj_t *parent)
{
    s_view_catalog = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_catalog);
    lv_obj_set_size(s_view_catalog, 800, 404);
    lv_obj_set_pos(s_view_catalog, 0, 0);
    lv_obj_add_flag(s_view_catalog, LV_OBJ_FLAG_HIDDEN);

    // Header bar: back pill + search field (loupe / text / clear / mic)
    lv_obj_t *header = lv_obj_create(s_view_catalog);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 768, 48);
    lv_obj_set_pos(header, 16, 12);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 16, 0);

    lv_obj_t *btn_back = gui_make_button(header, 110, 44,
                                         ROCK_ICON_ARROW_LEFT " Плеер",
                                         &font_inter_14, gui_on_back_to_np_click, NULL);
    lv_obj_set_style_radius(btn_back, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_border_color(btn_back, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn_back, 1, 0);

    lv_obj_t *bar = lv_obj_create(header);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 0, 44);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_style_bg_color(bar, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_radius(bar, ROCK_RADIUS_WIDGET, 0);
    lv_obj_set_style_pad_left(bar, 14, 0);
    lv_obj_set_style_pad_right(bar, 6, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *loupe = lv_label_create(bar);
    lv_label_set_text(loupe, ROCK_ICON_SEARCH);
    lv_obj_set_style_text_font(loupe, &font_inter_18, 0);
    lv_obj_set_style_text_color(loupe, ROCK_COLOR_MUTED, 0);

    s_ta_query = lv_textarea_create(bar);
    lv_obj_set_height(s_ta_query, 40);
    lv_obj_set_flex_grow(s_ta_query, 1);
    lv_obj_set_style_bg_opa(s_ta_query, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ta_query, 0, 0);
    lv_obj_set_style_pad_all(s_ta_query, 0, 0);
    lv_obj_set_style_text_color(s_ta_query, ROCK_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_ta_query, &font_inter_18, 0);
    lv_textarea_set_one_line(s_ta_query, true);
    lv_textarea_set_max_length(s_ta_query, 128);
    lv_obj_add_event_cb(s_ta_query, on_query_edit, LV_EVENT_CLICKED, NULL);
    lv_textarea_set_placeholder_text(s_ta_query, "Поиск станций...");

    // Clear (icon ✕, same callback as the former text button)
    lv_obj_t *btn_clear = gui_make_button(bar, 36, 36, ROCK_ICON_CROSS,
                                          &font_inter_18, on_query_clear_click, NULL);
    lv_obj_set_style_bg_opa(btn_clear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_clear, 0, 0);
    lv_obj_set_style_radius(btn_clear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn_clear, 0), ROCK_COLOR_MUTED, 0);

    // Microphone: static phase-4 stub — amber at 45% opacity, deliberately
    // non-interactive (no event callback; a press is a silent no-op).
    lv_obj_t *mic = lv_obj_create(bar);
    lv_obj_remove_style_all(mic);
    lv_obj_set_size(mic, 36, 36);
    lv_obj_set_style_radius(mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mic, ROCK_COLOR_MIC_AMBER, 0);
    lv_obj_set_style_bg_opa(mic, 115, 0);
    lv_obj_t *mic_lbl = lv_label_create(mic);
    lv_label_set_text(mic_lbl, ROCK_ICON_MIC);
    lv_obj_set_style_text_font(mic_lbl, &font_inter_18, 0);
    lv_obj_set_style_text_color(mic_lbl, ROCK_COLOR_WHITE, 0);
    lv_obj_center(mic_lbl);

    // Genre chips (48px pills) + right-aligned row counter
    lv_obj_t *cont_chips = lv_obj_create(s_view_catalog);
    lv_obj_remove_style_all(cont_chips);
    lv_obj_set_size(cont_chips, 768, 48);
    lv_obj_set_pos(cont_chips, 16, 68);
    lv_obj_set_flex_flow(cont_chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cont_chips, 8, 0);

    static const char *genres[] = {"Rock", "Pop", "Jazz", "Metal", "Classical", "News", "Electronic"};
    for (size_t i = 0; i < sizeof(genres) / sizeof(genres[0]); i++) {
        lv_obj_t *chip = gui_make_button(cont_chips, LV_SIZE_CONTENT, 48, genres[i],
                                         &font_inter_14, on_genre_chip_click, (void *)genres[i]);
        lv_obj_set_style_radius(chip, 24, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(chip, ROCK_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), ROCK_COLOR_MUTED, 0);
        lv_obj_set_style_pad_hor(chip, 18, 0);
    }

    lv_obj_t *sp = lv_obj_create(cont_chips);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    s_lbl_count = lv_label_create(cont_chips);
    lv_label_set_text(s_lbl_count, "");
    lv_obj_set_style_text_font(s_lbl_count, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_lbl_count, ROCK_COLOR_MUTED, 0);

    // Station list (768x236): visible by default, rows hidden until data
    s_list_catalog = lv_obj_create(s_view_catalog);
    lv_obj_remove_style_all(s_list_catalog);
    lv_obj_set_size(s_list_catalog, 768, 236);
    lv_obj_set_pos(s_list_catalog, 16, 124);
    lv_obj_set_flex_flow(s_list_catalog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_catalog, 4, 0);
    lv_obj_set_scrollbar_mode(s_list_catalog, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        gui_station_row_setup(&s_catalog_rows[i], s_list_catalog, i);
    }

    // Bottom pill container for "Подгрузка..." / "Конец списка"
    s_pill_box = lv_obj_create(s_list_catalog);
    lv_obj_remove_style_all(s_pill_box);
    lv_obj_set_size(s_pill_box, 768, 36);
    lv_obj_set_style_radius(s_pill_box, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_bg_color(s_pill_box, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_pill_box, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_pill_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pill_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lbl_pill = lv_label_create(s_pill_box);
    lv_obj_set_style_text_font(s_lbl_pill, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_lbl_pill, ROCK_COLOR_MUTED, 0);
    lv_label_set_text(s_lbl_pill, "");
    lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_list_catalog, on_catalog_list_scrolled, LV_EVENT_SCROLL, NULL);

    // On-screen virtual keyboard (768x236) overlays the list area while
    // editing (gui_catalog_begin_edit swaps the two).
    s_kb_catalog = lv_keyboard_create(s_view_catalog);
    lv_obj_set_size(s_kb_catalog, 768, 236);
    lv_obj_set_align(s_kb_catalog, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_kb_catalog, 16, 124);
    lv_keyboard_set_textarea(s_kb_catalog, s_ta_query);
    gui_keyboard_install(s_kb_catalog);
    lv_obj_set_style_bg_color(s_kb_catalog, ROCK_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_kb_catalog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_kb_catalog, ROCK_COLOR_BORDER_SUBTLE, 0);
    lv_obj_set_style_border_width(s_kb_catalog, 1, 0);
    lv_obj_set_style_radius(s_kb_catalog, ROCK_RADIUS_PANEL, 0);
    lv_obj_set_style_pad_all(s_kb_catalog, 10, 0);
    lv_obj_set_style_pad_row(s_kb_catalog, 6, 0);
    lv_obj_set_style_pad_column(s_kb_catalog, 6, 0);
    // Keys: PANEL_2 r8 + BORDER_SUBTLE ring
    lv_obj_set_style_bg_color(s_kb_catalog, ROCK_COLOR_PANEL_2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb_catalog, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_kb_catalog, ROCK_COLOR_BORDER_SUBTLE, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb_catalog, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb_catalog, ROCK_RADIUS_WIDGET, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb_catalog, ROCK_COLOR_FG, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kb_catalog, &font_inter_18, LV_PART_ITEMS);
#ifdef LV_BTNMATRIX_CTRL_CUSTOM_1
    // OK key (CHECKED ctrl, see rock_gui_keyboard.c): ACCENT with white glyph
    lv_obj_set_style_bg_color(s_kb_catalog, ROCK_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_kb_catalog, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_kb_catalog, ROCK_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(s_kb_catalog, 0, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_kb_catalog, ROCK_COLOR_WHITE, LV_PART_ITEMS | LV_STATE_CHECKED);
#endif
    lv_obj_add_event_cb(s_kb_catalog, on_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_flag(s_kb_catalog, LV_OBJ_FLAG_HIDDEN);
}

void gui_catalog_show(void)
{
    if (s_view_catalog) lv_obj_remove_flag(s_view_catalog, LV_OBJ_FLAG_HIDDEN);
}

void gui_catalog_hide(void)
{
    if (s_view_catalog) lv_obj_add_flag(s_view_catalog, LV_OBJ_FLAG_HIDDEN);
}

void gui_catalog_clear(void)
{
    gui_catalog_reset_fetching();
    s_catalog_count = 0;
    s_window_start = 0;
    s_catalog_cursor[0] = '\0';
    if (s_catalog) {
        free(s_catalog);
        s_catalog = NULL;
    }
    if (s_pill_box) {
        lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_count) {
        lv_label_set_text(s_lbl_count, "");
    }
}

void gui_catalog_begin(const rock_gui_station_item_t *items, size_t count, const char *next_cursor)
{
    gui_catalog_reset_fetching();
    s_is_search_mode = false;

    if (!ensure_catalog_buf()) return;

    s_catalog_count = 0;
    s_window_start = 0;

    for (size_t i = 0; i < count && s_catalog_count < ROCK_GUI_CATALOG_MAX_ACCUMULATED; i++) {
        s_catalog[s_catalog_count++] = items[i];
    }

    if (next_cursor && next_cursor[0]) {
        strncpy(s_catalog_cursor, next_cursor, sizeof(s_catalog_cursor) - 1);
        s_catalog_cursor[sizeof(s_catalog_cursor) - 1] = '\0';
    } else {
        s_catalog_cursor[0] = '\0';
    }

    update_window_rows();
    if (s_list_catalog) {
        s_in_scroll_adjust = true;
        lv_obj_scroll_to_y(s_list_catalog, 0, LV_ANIM_OFF);
        s_in_scroll_adjust = false;
    }

    update_count_label(s_catalog_count);
    update_pill();

    gui_overlay_hide();
    gui_switch_view(VIEW_CATALOG);
    if (s_catalog_count == 0) {
        gui_overlay_set(GUI_OVERLAY_EMPTY, "Каталог пуст", NULL, true);
    }
}

void gui_catalog_append(const rock_gui_station_item_t *items, size_t count, const char *next_cursor)
{
    gui_catalog_reset_fetching();
    s_is_search_mode = false;

    if (!ensure_catalog_buf()) return;

    size_t old_count = s_catalog_count;

    // Deduplicate by id and append
    for (size_t i = 0; i < count; i++) {
        bool exists = false;
        for (size_t j = 0; j < s_catalog_count; j++) {
            if (strcmp(s_catalog[j].id, items[i].id) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && s_catalog_count < ROCK_GUI_CATALOG_MAX_ACCUMULATED) {
            s_catalog[s_catalog_count++] = items[i];
        }
    }

    if (next_cursor && next_cursor[0]) {
        strncpy(s_catalog_cursor, next_cursor, sizeof(s_catalog_cursor) - 1);
        s_catalog_cursor[sizeof(s_catalog_cursor) - 1] = '\0';
    } else {
        s_catalog_cursor[0] = '\0';
    }

    ESP_LOGI(TAG, "Catalog append: +%u items (total %u), cursor='%s'",
             (unsigned)(s_catalog_count - old_count), (unsigned)s_catalog_count,
             s_catalog_cursor);

    // If visible window was at or near the tail of the old list, advance to show the new page
    if (s_window_start + ROCK_GUI_MAX_STATIONS >= old_count && s_catalog_count > old_count) {
        s_window_start = old_count;
        if (s_window_start >= s_catalog_count && s_catalog_count > 0) {
            s_window_start = (s_catalog_count > ROCK_GUI_MAX_STATIONS) ?
                             (s_catalog_count - ROCK_GUI_MAX_STATIONS) : 0;
        }
        update_window_rows();
        if (s_list_catalog) {
            s_in_scroll_adjust = true;
            lv_obj_scroll_to_y(s_list_catalog, 0, LV_ANIM_OFF);
            s_in_scroll_adjust = false;
        }
    } else {
        update_window_rows();
    }

    update_count_label(s_catalog_count);
    update_pill();

    gui_overlay_hide();
    gui_switch_view(VIEW_CATALOG);
}

void gui_catalog_set_rows(const rock_gui_station_item_t *items, size_t count, bool search_results)
{
    if (search_results) {
        s_is_search_mode = true;
        gui_catalog_reset_fetching();
        size_t limit = count > ROCK_GUI_MAX_STATIONS ? ROCK_GUI_MAX_STATIONS : count;
        s_catalog_rows_count = limit;

        gui_station_rows_fill(s_catalog_rows, items, limit, true);

        if (s_pill_box) {
            lv_obj_add_flag(s_pill_box, LV_OBJ_FLAG_HIDDEN);
        }

        if (s_lbl_count) {
            update_count_label(count);
        }

        if (s_kb_catalog) lv_obj_add_flag(s_kb_catalog, LV_OBJ_FLAG_HIDDEN);
        if (s_list_catalog) {
            lv_obj_remove_flag(s_list_catalog, LV_OBJ_FLAG_HIDDEN);
            s_in_scroll_adjust = true;
            lv_obj_scroll_to_y(s_list_catalog, 0, LV_ANIM_OFF);
            s_in_scroll_adjust = false;
        }

        gui_overlay_hide();
        gui_switch_view(VIEW_CATALOG);
        if (count == 0) gui_overlay_set(GUI_OVERLAY_EMPTY, "Ничего не найдено", NULL, true);
    } else {
        gui_catalog_begin(items, count, NULL);
    }
}

void gui_catalog_hide_rows(void)
{
    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        lv_obj_add_flag(s_catalog_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }
}

station_row_ui_t *gui_catalog_rows(void)
{
    return s_catalog_rows;
}

size_t gui_catalog_rows_count(void)
{
    return s_catalog_rows_count;
}

bool gui_catalog_trigger(const char *action)
{
    if (!strcmp(action, "SEARCH")) {
        gui_catalog_begin_edit();
        gui_switch_view(VIEW_CATALOG);
        return true;
    }
    if (!strncmp(action, "SEARCH:", 7)) {
        gui_switch_view(VIEW_CATALOG);
        lv_textarea_set_text(s_ta_query, action + 7);
        lv_obj_send_event(s_kb_catalog, LV_EVENT_READY, NULL);
        return true;
    }
    if (!strcmp(action, "LOADMORE")) {
        gui_switch_view(VIEW_CATALOG);
        // If there are already accumulated stations beyond the window, advance window:
        if (s_window_start + ROCK_GUI_MAX_STATIONS < s_catalog_count) {
            s_window_start += ROCK_GUI_MAX_STATIONS;
            update_window_rows();
            if (s_list_catalog) {
                s_in_scroll_adjust = true;
                lv_obj_scroll_to_y(s_list_catalog, 0, LV_ANIM_OFF);
                s_in_scroll_adjust = false;
            }
            update_pill();
            ESP_LOGI(TAG, "LOADMORE advanced window to start=%u (count=%u)",
                     (unsigned)s_window_start, (unsigned)s_catalog_count);
            return true;
        }
        // Otherwise, if we have a cursor and not already fetching:
        if (s_catalog_cursor[0] != '\0' && !s_fetching_more) {
            s_fetching_more = true;
            update_pill();
            if (s_fetch_timer) lv_timer_delete(s_fetch_timer);
            s_fetch_timer = lv_timer_create(on_fetch_timeout, 15000, NULL);
            ESP_LOGI(TAG, "LOADMORE requesting browse with cursor='%s'", s_catalog_cursor);
            gui_enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, s_catalog_cursor, 0);
            return true;
        }
        ESP_LOGI(TAG, "LOADMORE ignored: cursor empty (stn count=%u)", (unsigned)s_catalog_count);
        return true;
    }
    return false;
}
