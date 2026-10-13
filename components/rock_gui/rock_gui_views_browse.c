// Browse view, phase-4 split out of rock_gui.c: the catalog list screen.

#include "rock_gui_internal.h"
#include "rock_ui_tokens.h"
#include "rock_fonts.h"

static lv_obj_t *s_view_browse = NULL;
static lv_obj_t *s_browse_header = NULL;
static lv_obj_t *s_lbl_browse_title = NULL;
static lv_obj_t *s_lbl_browse_count = NULL;
static lv_obj_t *s_list_browse = NULL;

static station_row_ui_t s_browse_rows[ROCK_GUI_MAX_STATIONS];
static size_t s_browse_rows_count = 0;

void gui_browse_build(lv_obj_t *parent)
{
    s_view_browse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view_browse);
    lv_obj_set_size(s_view_browse, 800, 404);
    lv_obj_set_pos(s_view_browse, 0, 0);
    lv_obj_add_flag(s_view_browse, LV_OBJ_FLAG_HIDDEN);

    // Sub-header (h=48): back pill + title + station counter
    s_browse_header = lv_obj_create(s_view_browse);
    lv_obj_remove_style_all(s_browse_header);
    lv_obj_set_size(s_browse_header, 768, 48);
    lv_obj_set_pos(s_browse_header, 16, 12);
    lv_obj_set_flex_flow(s_browse_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_browse_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_browse_header, 16, 0);

    lv_obj_t *btn_back = gui_make_button(s_browse_header, 110, 44,
                                         ROCK_ICON_ARROW_LEFT " Плеер",
                                         &font_inter_14, gui_on_back_to_np_click, NULL);
    lv_obj_set_style_radius(btn_back, ROCK_RADIUS_BUTTON, 0);
    lv_obj_set_style_border_color(btn_back, ROCK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn_back, 1, 0);

    s_lbl_browse_title = lv_label_create(s_browse_header);
    lv_label_set_text(s_lbl_browse_title, "КАТАЛОГ РАДИОСТАНЦИЙ");
    lv_obj_set_style_text_font(s_lbl_browse_title, &font_inter_18, 0);
    lv_obj_set_style_text_color(s_lbl_browse_title, ROCK_COLOR_FG, 0);

    lv_obj_t *sp = lv_obj_create(s_browse_header);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    s_lbl_browse_count = lv_label_create(s_browse_header);
    lv_label_set_text(s_lbl_browse_count, "");
    lv_obj_set_style_text_font(s_lbl_browse_count, &font_inter_14, 0);
    lv_obj_set_style_text_color(s_lbl_browse_count, ROCK_COLOR_MUTED, 0);

    // Scrollable List container (768x296: exactly five 56px rows)
    s_list_browse = lv_obj_create(s_view_browse);
    lv_obj_remove_style_all(s_list_browse);
    lv_obj_set_size(s_list_browse, 768, 296);
    lv_obj_set_pos(s_list_browse, 16, 68);
    lv_obj_set_flex_flow(s_list_browse, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_browse, 4, 0);
    lv_obj_set_scrollbar_mode(s_list_browse, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        gui_station_row_setup(&s_browse_rows[i], s_list_browse, i);
    }
}

void gui_browse_show(void)
{
    if (s_view_browse) lv_obj_remove_flag(s_view_browse, LV_OBJ_FLAG_HIDDEN);
}

void gui_browse_hide(void)
{
    if (s_view_browse) lv_obj_add_flag(s_view_browse, LV_OBJ_FLAG_HIDDEN);
}

void gui_browse_set_stations(const rock_gui_station_item_t *items, size_t count)
{
    size_t limit = count > ROCK_GUI_MAX_STATIONS ? ROCK_GUI_MAX_STATIONS : count;
    s_browse_rows_count = limit;

    gui_station_rows_fill(s_browse_rows, items, limit, false);

    if (s_lbl_browse_count) {
        // display-only counter with Russian pluralization
        unsigned n = (unsigned)count;
        const char *form = "станций";
        if (n % 10 == 1 && n % 100 != 11) form = "станция";
        else if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) form = "станции";
        lv_label_set_text_fmt(s_lbl_browse_count, "%u %s", n, form);
    }

    gui_overlay_hide();
    gui_switch_view(VIEW_BROWSE);
    if (count == 0) gui_overlay_set(GUI_OVERLAY_EMPTY, "Каталог пуст", NULL, true);
}

void gui_browse_hide_rows(void)
{
    for (size_t i = 0; i < ROCK_GUI_MAX_STATIONS; i++) {
        lv_obj_add_flag(s_browse_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }
}

station_row_ui_t *gui_browse_rows(void)
{
    return s_browse_rows;
}

size_t gui_browse_rows_count(void)
{
    return s_browse_rows_count;
}
