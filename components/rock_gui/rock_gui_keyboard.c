// Virtual keyboard, phase-4 split out of rock_gui.c: RU/EN/123 keymaps and
// the language-switch input callback shared by the catalog view.

#include "rock_gui_internal.h"

#include <string.h>

// Four rows, explicit language keys; all layouts can return to Russian.
static const char * const kb_map_ru[] = {
    "ё", "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ъ", LV_SYMBOL_BACKSPACE, "\n",
    "EN", "ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "\n",
    "123", "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", ".", "-", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char * const kb_map_en[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "RU", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "123", "z", "x", "c", "v", "b", "n", "m", ".", "-", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char * const kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "RU", "EN", "!", "?", "@", "#", "$", "%", "&", "*", "\n",
    "+", "-", "=", "(", ")", "/", ":", ";", ".", ",", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static lv_buttonmatrix_ctrl_t kb_controls[3][48];

// Style class (phase 3): the OK key is marked CHECKED — this LVGL version's
// btnmatrix CUSTOM_* ctrl bits carry no state mapping, while CHECKED maps to
// LV_STATE_CHECKED — and matched by LV_PART_ITEMS|LV_STATE_CHECKED styles in
// the search view. Special keys stay uniform (readable, minor mockup trim).
static void keyboard_input(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_buttonmatrix_get_button_text(kb, id);
    if (!strcmp(text, "RU")) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
    else if (!strcmp(text, "EN")) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_2);
    else if (!strcmp(text, "123")) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_3);
    else lv_keyboard_def_event_cb(e);
}

void gui_keyboard_install(lv_obj_t *kb)
{
    const char * const *maps[] = {kb_map_ru, kb_map_en, kb_map_num};
    for (int m = 0; m < 3; m++) {
        int button = 0;
        for (int i = 0; maps[m][i][0]; i++) {
            if (!strcmp(maps[m][i], "\n")) continue;
            // Note: LV_KEYBOARD_CTRL_BUTTON_FLAGS carries CHECKED for EVERY
            // key, which would make the CHECKED-styled OK style hit all keys;
            // build the base explicitly and mark only OK as checked.
            lv_buttonmatrix_ctrl_t ctrl =
                LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                (!strcmp(maps[m][i], " ") ? 5 : 1);
            if (!strcmp(maps[m][i], LV_SYMBOL_OK)) ctrl |= LV_BUTTONMATRIX_CTRL_CHECKED;
            kb_controls[m][button++] = ctrl;
        }
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1 + m, maps[m], kb_controls[m]);
    }
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, keyboard_input, LV_EVENT_VALUE_CHANGED, NULL);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
}
