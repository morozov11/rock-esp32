#pragma once

#include "lvgl.h"

extern const lv_font_t font_inter_14;
extern const lv_font_t font_inter_18;
extern const lv_font_t font_inter_24;
extern const lv_font_t font_hack_16;

// FontAwesome icons merged into Inter font (UTF-8 strings)
#define ROCK_ICON_PLAY       "\xEF\x81\x8B" // U+F04B
#define ROCK_ICON_PAUSE      "\xEF\x81\x8C" // U+F04C
#define ROCK_ICON_STOP       "\xEF\x81\x8D" // U+F04D
#define ROCK_ICON_SEARCH     "\xEF\x80\x82" // U+F002
#define ROCK_ICON_MIC        "\xEF\x84\xB0" // U+F130 (microphone per center directive)
#define ROCK_ICON_CROSS      "\xEF\x80\x8D" // U+F00D
#define ROCK_ICON_BOLT       "\xEF\x83\xA7" // U+F0E7
#define ROCK_ICON_WIFI       "\xEF\x87\xAB" // U+F1EB
#define ROCK_ICON_ARROW_LEFT "\xEF\x81\xA0" // U+F060
