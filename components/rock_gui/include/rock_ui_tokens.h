#pragma once

#include "lvgl.h"

// Phase-3 palette ported from the desktop rockcast theme (approved mockup:
// docs/design/phase3-mockup.html, source rockcast src/app/theme.rs).

// 1. Color tokens
#define ROCK_COLOR_BG              lv_color_hex(0x15110E) // Основной фон экранов
#define ROCK_COLOR_PANEL           lv_color_hex(0x201914) // Поверхность базовых панелей
#define ROCK_COLOR_PANEL_2         lv_color_hex(0x2A211B) // Карточки, кнопки 2-го плана
#define ROCK_COLOR_BORDER          lv_color_hex(0x3B2C23) // Рамки акцентных виджетов
#define ROCK_COLOR_BORDER_SUBTLE   lv_color_hex(0x2D211A) // Рамка 1px всех панелей (панель-рецепт)
#define ROCK_COLOR_FG              lv_color_hex(0xF3EEE9) // Основной текст
#define ROCK_COLOR_ACCENT          lv_color_hex(0xE56020) // Акцент rockcast
#define ROCK_COLOR_MUTED           lv_color_hex(0x9E8D82) // Вторичный текст и метаданные
#define ROCK_COLOR_BAR_DIM         lv_color_hex(0x5A4030) // Неактивные шкалы и индикаторы
#define ROCK_COLOR_ROW_ALT         lv_color_hex(0x201814) // Нечётная строка списка (зебра)
#define ROCK_COLOR_WHITE           lv_color_hex(0xFFFFFF) // Чистый белый (пики, кнопки)
#define ROCK_COLOR_GREEN           lv_color_hex(0x3DDC84) // Live-точки, «Активен», тикер в игре
#define ROCK_COLOR_MIC_AMBER       lv_color_hex(0xE67E22) // Статичная кнопка микрофона (фаза 4)
#define ROCK_COLOR_SEL_TEXT_SEC    lv_color_hex(0x3A281C) // Вторичный текст на ACCENT-строке
#define ROCK_COLOR_MONO_TEXT       lv_color_hex(0xFAEEE4) // Инициалы в монограммах
#define ROCK_COLOR_WARN            lv_color_hex(0xFFAA00) // Предупреждения
#define ROCK_COLOR_ERROR           lv_color_hex(0xE05353) // Ошибки воспроизведения
#define ROCK_COLOR_GOLD_STAR       lv_color_hex(0xF59E0B) // Избранное (резерв)

// Deterministic station monogram color: port of station_color() from the
// desktop theme (h = h*31 + byte, u32 wrap; palette[h % 8]).
uint32_t rock_ui_station_color(const char *name);

// 2. Geometry and spacing (px for 800x480 landscape)
#define ROCK_RADIUS_PANEL          10  // Скругление панелей (панель-рецепт)
#define ROCK_RADIUS_WIDGET         8   // Скругление виджетов и статус-бара
#define ROCK_RADIUS_BUTTON         8   // Скругление кнопок и строк списка
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
