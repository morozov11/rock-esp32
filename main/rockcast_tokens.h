#pragma once

#include "lvgl.h"

// 1. Color palette tokens from docs/rockcast-design-tokens.md
#define ROCK_COLOR_BG              lv_color_hex(0x1A1410) // Основной фон экранов
#define ROCK_COLOR_PANEL           lv_color_hex(0x241C16) // Поверхность базовых панелей
#define ROCK_COLOR_PANEL_2         lv_color_hex(0x2E241C) // Карточки, кнопки 2-го плана
#define ROCK_COLOR_FG              lv_color_hex(0xE8DCC8) // Основной кремовый текст
#define ROCK_COLOR_ACCENT          lv_color_hex(0xC45C26) // Тёплый терракотовый акцент
#define ROCK_COLOR_MUTED           lv_color_hex(0x9A8B78) // Вторичный текст и метаданные
#define ROCK_COLOR_BAR_DIM         lv_color_hex(0x5A4030) // Неактивные шкалы и индикаторы
#define ROCK_COLOR_STROKE          lv_color_hex(0x3A2E24) // Тонкие рамки 1 px
#define ROCK_COLOR_ROW_ALT         lv_color_hex(0x201814) // Нечётная строка списка (зебра)
#define ROCK_COLOR_WHITE           lv_color_hex(0xFFFFFF) // Чистый белый (пики, кнопки)
#define ROCK_COLOR_WARN            lv_color_hex(0xFFAA00) // Предупреждения (egui warn default)
#define ROCK_COLOR_ERROR           lv_color_hex(0xE05353) // Ошибки воспроизведения

// 2. Geometry and spacing (px for 800x480 landscape)
#define ROCK_RADIUS_PANEL          8   // Скругление основных панелей
#define ROCK_RADIUS_WIDGET         6   // Скругление виджетов и статус-бара
#define ROCK_RADIUS_BUTTON         4   // Скругление кнопок и строк списка
#define ROCK_RADIUS_BAR            2   // Скругление полос эквалайзера

#define ROCK_SPACE_XS              4   // Микро-отступ между связанными текстами
#define ROCK_SPACE_SM              6   // Интервал между соседними кнопками
#define ROCK_SPACE_MD              8   // Базовый зазор элементов
#define ROCK_SPACE_LG              12  // Внутренний паддинг карточек
#define ROCK_SPACE_XL              16  // Поля экрана от края матрицы

// 3. Touch element dimensions
#define ROCK_TOUCH_TARGET_MIN      48  // Абсолютный минимум высоты/ширины тач-кнопки
#define ROCK_LIST_ROW_HEIGHT       56  // Высота строки списка станций
#define ROCK_HEADER_HEIGHT         40  // Высота верхнего бара
#define ROCK_TRANSPORT_HEIGHT      64  // Высота панели управления плеером
#define ROCK_STATUS_BAR_HEIGHT     36  // Высота нижнего статус-бара
