# UC: Единый список станций (каталог + поиск) — план работ

Статус: утверждён 2026-10-13.
Владелец: план-coordination см. [`plan-control.md`](plan-control.md) (таблица «Точки входа»).
Эталон поведения: приложение rockmobile (`C:\repos\rockmobile`).
Область: **только rock-esp32**. Репозиторий rockserver НЕ меняется (см. «Ключевое решение»).

Каждый шаг UC-1…UC-7 — самостоятельная карточка для передачи отдельному агенту.
При передаче агенту копируйте: его карточку + раздел «Общие правила для всех агентов» +
раздел «Карта кода» (только нужные файлы). Агент обязан выполнить «Критерии приёмки».

---

## 1. Ключевое решение: сервер не трогаем

Формулировка задачи — «единый список с подгрузкой и поиском, как в rockmobile».
Проверено по коду rockmobile ([StationRepository.kt](C:/repos/rockmobile/app/src/main/java/com/rockmobile/data/repository/StationRepository.kt)):

- «Единый список» в rockmobile — это **клиентский UI-паттерн**, а не протокол: один
  экран, один список, одно состояние, куда «каталог» и «поиск» просто кладут разные данные.
- **Просмотр** — локальный каталог (SQLite, 16 825 станций); сервер не участвует.
- **Поиск** — один запрос, ≤20 результатов, **без пагинации навсегда**; результат
  целиком заменяет список; при сбое старый список остаётся на экране.
- Единственная пагинация — курсорная в browse-эндпоинте, rockmobile использует её
  только для полной вычитки каталога циклом
  ([StationSources.kt:40-53](C:/repos/rockmobile/app/src/main/java/com/rockmobile/data/stations/StationSources.kt)).

Серверная часть устройства это уже поддерживает, **изменения не нужны**:

- `GET /api/v1/device-control/catalog/stations` уже принимает `cursor`
  (rockserver `src/http/device_catalog.rs:114-189`, валидация `^[A-Za-z0-9-]+$`,
  1..=512 байт) и возвращает `next_cursor` (равен id последней станции полной
  страницы — keyset-пагинация по `ORDER BY s.id`).
- `GET /api/v1/device-control/catalog/search` уже возвращает страницу ≤20 без
  курсора — ровно то, что потребляет rockmobile.

Прошивка сегодня не использует ни то, ни другое: `catalog_path()` не отправляет
курсор, а пришедший `next_cursor` выбрасывается в `rock_gui.c`
(`(void)next_cursor;`). Всё, что нужно, — научить прошивку пользоваться
существующим контрактом и объединить два экрана в один.

Догрузка *результатов поиска* за пределы 20 невозможна без серверной разработки
(ранжированный поиск не курсоруется — замороженное контрактом RS-1 решение).
Это осознанно вынесено в бэклог (раздел 9).

---

## 2. Целевая архитектура

### 2.1 Распределение состояния

| Состояние | Владелец | Где |
| --- | --- | --- |
| Накопленный каталоговый список (до 128 записей) + курсор + окно | **C (GUI)** | PSRAM |
| Видимые 20 строк (переиспользуемые LVGL-виджеты) | **C (GUI)** | internal |
| Результаты поиска (≤20) — отдельный массив, каталог не затирают | **C (GUI)** | internal |
| Маршрутизация запросов (refresh / load-more / search), busy, retry | **Rust** | `Session` |
| Кэш id→имя (для Now-Playing) | **Rust** | `CATALOG_CACHE` (как сейчас) |

### 2.2 Потоки данных

Догрузка каталога:

```
скролл к концу накопленного списка (C)
  → gui_enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, text=cursor)   // хук уже есть
  → Rust: GuiEvent::CatalogBrowse(cursor)
  → load_catalog(LoadMore{cursor})
  → GET /api/v1/device-control/catalog/stations?limit=20&cursor=<id>
  → parse_catalog_page → rock_gui_catalog_append(items, count, next_cursor)
  → C: дедуп по id, append в PSRAM-список, окно перезаполняется, скролл продолжается
```

Поиск (паттерн rockmobile — заменяем видимый список, каталог храним):

```
submit запроса (C) → ROCK_UI_EVT_CATALOG_SEARCH(q)
  → GET /catalog/search?limit=20&q=... → rock_gui_set_search_results(items, count)
  → C: search-режим, видимый список заменён, накопленный каталог сохранён
очистка запроса / «назад» (C) → локальный возврат к каталогу, БЕЗ запроса на сервер
```

### 2.3 Память

- Накопленный список в C — `rock_gui_station_item_t[128]` (~378 Б/запись ≈ 48 КиБ) —
  **только PSRAM** (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`), как 2 МиБ ring в
  rock_player. Internal RAM дефицитна: ~33–44 КиБ свободно во время воспроизведения
  (см. [`memory-budget.md`](memory-budget.md)).
- Видимое окно — те же 20 пре-созданных строк, что сейчас
  (`ROCK_GUI_MAX_STATIONS`), никакой виртуализации LVGL не требуется.

---

## 3. Карта кода (на 2026-10-13, до коммита текущего WIP)

Стоки указаны для ориентира — сверяйтесь по символам, номера уплывут после первых же правок.

### Rust (host-приложение)

| Файл | Что там |
| --- | --- |
| `src/device_control/catalog.rs` | Контракт: `catalog_path(query)` :12-35 (курсор НЕ принимает), `DeviceCatalogPageDto` :82-88 (`next_cursor` есть), `DeviceSearchPageDto` :90-94 (без курсора — так и надо), `parse_catalog_page` :154, `parse_search_page` :182, `should_retry_catalog_with_fresh_token` :206 (401 → одна повторная попытка). Лимиты: :3-9 (страница ≤20, запрос ≤128, страница ≤64 КиБ). |
| `src/firmware/transport/catalog.rs` | `CATALOG_CACHE` :10; `load_catalog(token, query)` :90-145 — единственная функция загрузки, синхронная, вызывает `gui_show_loading` → HTTP GET → parse → `gui_set_browse_stations`/`gui_set_search_results`. |
| `src/firmware/transport/gui_sink.rs` | `on_catalog_browse` :119-122 и `on_catalog_search` :124-127 — ставят `self.retry` и зовут `load_catalog`. Образец «heartbeat вокруг блокирующей операции» — `on_cast_select` :162-176. |
| `src/firmware/transport/session.rs` | `Session` :15-38 (`retry: Option<GuiEvent>` :27); `drain_gui_events` :136-150 — реинъекция Retry. |
| `src/firmware/transport/mod.rs` | Главный цикл `session()` :85-106 (тик: heartbeat → drain_gui_events → … → ws_receive(50)); `run()` :33-41 зовёт `gui_mark_catalog_stale()` на выходе. |
| `src/firmware/platform/gui.rs` | `GuiEvent` :11-23 (`CatalogBrowse` **без** текста); декодирование :43 **выбрасывает text** для browse; `gui_set_browse_stations` :102-131 (стек-массив `[RockGuiStationItem; 20]`, курсор передаётся в C); `gui_set_search_results` :133-156. |
| `src/presentation.rs` | Строки UI (`ui_strings`, `tr::`). |
| `tests/device_control_contract.rs` | Host-тесты контракта (запуск: `cargo test`). Фикстуры берутся из чекаута rockserver через `ROCKSERVER_ROOT`. |

### C (GUI, LVGL)

| Файл | Что там |
| --- | --- |
| `components/rock_gui/include/rock_gui.h` | `ROCK_GUI_MAX_STATIONS 20` :11; `rock_gui_station_item_t` :25-32; enum событий :34-47 (`ROCK_UI_EVT_CATALOG_BROWSE // text: optional cursor` :41 — возвратный канал для курсора уже предусмотрен); `rock_ui_event_t` :57-61 (`text[513]`, `value`); `rock_gui_set_browse_stations` :93-94; `rock_gui_set_search_results` :99; `rock_gui_switch_view(int)` :138; `rock_gui_trigger` :146; `rock_gui_log_debug_state` :152. |
| `components/rock_gui/rock_gui.c` | Реестр вью :75-79 (`VIEW_NOW_PLAYING`, `VIEW_BROWSE`, `VIEW_SEARCH`); `gui_switch_view` :108-122; очередь событий глубиной 32 (:291) + `gui_enqueue_event` :83; `rock_gui_set_browse_stations` :446-454 — **`(void)next_cursor;` выбрасывает курсор**; `rock_gui_trigger` :602-631 (сериальные UI-действия). |
| `components/rock_gui/rock_gui_views_browse.c` | Экран каталога: `s_browse_rows[20]` :13-14; скроллируемый список 768×296 (~5 строк видно); `gui_browse_set_stations` :78-97 (заполнение, счётчик «N станций», свитч вью). |
| `components/rock_gui/rock_gui_views_search.c` | Экран поиска: чипы жанров :19-28 (заполняют textarea и сабмитят); `on_kb_event` :50-60 (`LV_EVENT_READY` → enqueue `ROCK_UI_EVT_CATALOG_SEARCH`); textarea :106-117 (maxlen 128, кнопка очистки); `gui_search_set_results` :218-230; `s_search_rows[20]` :16-17. |
| `components/rock_gui/rock_gui_widgets.c` | Общий строковый виджет `station_row_ui_t` + `gui_station_row_setup` :126-207, `gui_station_rows_fill` :209-260 (чередующийся фон, «128k MP3», подсветка играющей); клик строки → `ROCK_UI_EVT_PLAY_STATION` :109-124; оверлеи `gui_overlay_set` :366-424. |
| `components/rock_gui/rock_gui_keyboard.c` | Экранная клавиатура RU/EN/123, 768×236. |
| `components/rock_gui/rock_serial.c` | USB-JTAG диагностика: разбор `CMD:` строк :48-87 (`SELFTEST`, `UI:<action>`, `METRICS`, `SCREENSHOT`, `VIEW:NP\|BROWSE\|SEARCH`). Через неё работают tools/*.py. |
| `components/rock_gui/rock_gui_internal.h` | Внутренние структуры (в т.ч. `station_row_ui_t` :55-65). |

### Смежное

- Воспроизведение выбранной станции: клик строки → `gui_sink.rs:13-43` (`on_play_station`) → WSS `station.play_station` → ответ `station.play_stream` → `media.rs` открывает прокси `/api/v1/stations/<id>/stream`. **Этот путь не меняем.**
- Дизайн-токены: [`rockcast-design-tokens.md`](rockcast-design-tokens.md), макет `docs/design/phase3-mockup.html`.

---

## 4. Общие правила для всех агентов

1. **rockserver не трогать.** Все изменения — только в `C:\repos\rock-esp32`.
2. **ABI не перенумеровывать.** Значения `rock_ui_event_type_t` и констант из
   `main/rock_protocol_abi.h` генерируются в Rust через `build.rs` (`crate::abi`).
   Новые элементы enum — только в конец; существующие не переименовывать и не
   переставлять. Новые FFI-функции — с новыми именами.
3. **LVGL-дисциплина**: любое обращение к виджетам — под `lvgl_port_lock(1000)`.
4. **Память**: новые буферы >4 КиБ — только PSRAM (`MALLOC_CAP_SPIRAM`).
   Не увеличивать потребление internal RAM; проверять `CMD:METRICS`.
5. **Границы строк**: id/name ≤128+NUL, tags ≤63+NUL, codec ≤15+NUL, курсор ≤512,
   текст события ≤512. Копирование — через существующие bounded-хелперы.
6. **Формат `rock_gui_log_debug_state`** разбирается tools/*.py — при изменении
   формата обновить и tools (проверить грепом `VIEW`/`debug` в `tools/`).
7. **Проверки перед коммитом**: `cargo test` (host-тесты зелёные), `idf.py build`
   (через skill `esp-idf-tools`). Для шагов с GUI — прошивка и smoke через serial
   (`CMD:UI:...`, `CMD:SCREENSHOT`), как в `tools/watch_serial.py`.
8. **Коммиты**: conventional commits по образцу истории (`feat(gui): ...`,
   `fix(audio): ...`). Один шаг = один-два коммита.
9. Сервер для ручной проверки: базовый URL в `CONFIG_ROCKSERVER_BASE_URL`;
   в каталоге сейчас 41 станция (3 страницы по 20) — это тест-сет для пагинации.

---

## 5. Граф шагов и рекомендации по моделям

| Шаг | Название | Зависимости | Слой | Класс модели* | Оценка |
| --- | --- | --- | --- | --- | --- |
| UC-0 | Завершить и закоммитить текущий WIP | — | Rust+C | топ | 1 сессия |
| UC-1 | Курсор в `catalog_path` + host-тесты | UC-0 | Rust | средний | 0.5 сессии |
| UC-2 | Маршрутизация `CatalogBrowse(cursor)`, LoadMore, busy, heartbeat | UC-1 | Rust | топ | 1 сессия |
| UC-3 | Объединение экранов Browse+Search → Catalog | UC-0 | C (LVGL) | топ | 1–2 сессии |
| UC-4 | Накопление в PSRAM + окно 20 строк + append-FFI + скролл-триггер | UC-2, UC-3 | C+Rust | топ | 2 сессии |
| UC-5 | Режим поиска как в rockmobile (замена списка, возврат без рефетча) | UC-3 (UC-4 для полной картины) | C | средний | 0.5–1 сессия |
| UC-6 | Состояния и деградация (inline-индикаторы, ошибки не затирают список) | UC-4, UC-5 | C+Rust | средний | 1 сессия |
| UC-7 | Приёмка, serial-инструменты, документация | все | C+Rust+docs | средний | 1 сессия |

\* «топ» = GLM-5.3 / Claude Sonnet 4.5 / GPT-5.2-Codex; «средний» = класс
Sonnet/Haiku или GLM младшей линейки. Мотивировка: UC-2/UC-4 — конкурентность,
память и C↔Rust координация, там дешёвая модель ошибается дорого; UC-1/UC-6/UC-7 —
механика и документация.

Параллелизация: цепочка UC-1→UC-2 (Rust) не пересекается по файлам с UC-3 (C) —
можно запускать одновременно двумя агентами. UC-4 стыкует обе ветки.

---

## 6. Карточки шагов

### UC-0. Предусловие: завершить текущий WIP

**Зачем.** В рабочей копии висит незакоммиченный перенос `transport.rs` на модули
(`src/firmware/transport/*`), tolerance избыточных тегов и правки GUI. План
опирается на эту структуру — сначала зафиксировать.

**Задачи.**
1. Проверить `git status` / `git diff`: transport-сплит, `tolerate_overflow_tags`,
   удаление демо-станций из `rock_gui_start`, `tools/*.py`.
2. `cargo test` + `idf.py build` зелёные; smoke на железе по желанию.
3. Закоммитить (логично: `refactor(firmware): split transport into modules` и т.п.).

**Приёмка.** `git status` чистый; тесты и сборка зелёные.

---

### UC-1. Rust-контракт: курсор в `catalog_path` + host-тесты

**Контекст.** `src/device_control/catalog.rs:12-35` — `catalog_path(query: Option<&str>)`
всегда строит `limit=20` без курсора. Серверный контракт: параметр `cursor`
опциональный, ≤512 байт, без управляющих символов; `next_cursor` в ответе —
id последней станции полной страницы. Поиск (`query: Some`) курсора не имеет
вообще (у search-эндпоинта нет параметра `cursor`, лишний параметр даст 400).

**Задачи.**
1. Новая сигнатура:
   ```rust
   pub fn catalog_path(query: Option<&str>, cursor: Option<&str>) -> Result<String, CatalogError>
   ```
   Семантика:
   - `query=None, cursor=None` → `/api/v1/device-control/catalog/stations?limit=20`;
   - `query=None, cursor=Some(c)` → добавить `&cursor=<percent-encoded c>`;
   - `query=Some(q), cursor=Some(_)` → `Err(CatalogError::Malformed)` —
     **инвариант: поиск не курсоруется**, ошибка на этапе сборки пути;
   - курсор трактуется как непрозрачная строка (НЕ проверять charset `^[A-Za-z0-9-]+$`,
     чтобы не сломаться при смене кодировки курсора сервером): непустой, ≤512 байт,
     без control-символов, `Some("")` эквивалентен `None`; кодирование — тем же
     percent-encoder'ом, что и для `q`.
2. Обновить единственного вызывающего: `src/firmware/transport/catalog.rs`
   (`load_catalog`) — передавать `None` (поведение не меняется).
3. Тесты в `tests/device_control_contract.rs` (по образцу
   `catalog_limits_and_utf8_query_are_enforced`):
   - `(None, None)` → путь без `cursor=`;
   - `(None, Some("station-news-001"))` → `...&cursor=station-news-001`;
   - UTF-8/небезопасные символы курсора процент-кодируются;
   - `(None, Some(""))` ≡ `(None, None)`;
   - курсор длиннее 512 / с control-символами → `Malformed`;
   - `(Some("рок"), Some("x"))` → `Malformed`.

**Приёмка.** `cargo test` зелёный; `idf.py build` зелёный; поведение прошивки не изменилось.

**Границы.** Не трогать DTO/парсеры (`DeviceCatalogPageDto` уже корректен), не трогать GUI.

---

### UC-2. Rust: маршрутизация LoadMore, busy-coalescing, heartbeat вокруг fetch

**Контекст.**
- `platform/gui.rs:43` — декодирование `ROCK_UI_EVT_CATALOG_BROWSE` выбрасывает
  `text` (курсор). Заголовок `rock_gui.h:41` уже документирует «text: optional cursor».
- `gui_sink.rs:119-127` — browse/search не различают «обновить» и «догрузить».
- `load_catalog` (`transport/catalog.rs:90-145`) выполняется синхронно в тике
  сессии: HTTP-таймаут 10 с, при 401 — вторая попытка; watchdog heartbeat 75 с.
  Быстрый скролл может поставить в очередь (глубина 32) несколько load-more.
- Образец «heartbeat вокруг блокирующей операции»: `gui_sink.rs:162-176`
  (`on_cast_select` перед медленным Cast LOAD).

**Задачи.**
1. `GuiEvent::CatalogBrowse` → `CatalogBrowse(String)` (cursor); декодер
   `platform/gui.rs` передаёт `text`. Обратная совместимость: C сейчас шлёт `""`
   (`rock_gui_views_now_playing.c` кнопка «Каталог`) — пустая строка = «refresh».
2. В `transport/catalog.rs` ввести тип запроса вместо `Option<&str>`:
   ```rust
   pub(super) enum CatalogRequest {
       Refresh,
       LoadMore { cursor: String },
       Search { query: String },
   }
   ```
   `load_catalog` принимает его; `Refresh`/`LoadMore` → `catalog_path(None, cursor)`,
   `Search` → `catalog_path(Some(q), None)`. **До UC-4 оба варианта продолжают
   звать существующий `gui_set_browse_stations`** (LoadMore временно заменяет
   список страницей N — это промежуточное, о нем явно в коммит-месседже).
3. `gui_sink.rs`: `on_catalog_browse(cursor)` — пусто → `Refresh`, иначе
   `LoadMore`; `self.retry` хранит событие с курсором (реинъекция Retry через
   `session.rs:136-150` начнёт работать для догрузки автоматически).
4. Дедупликация догрузки: в `Session` завести `catalog_last_cursor: Option<String>`;
   событие `LoadMore` с курсором, равным последнему отправленному и ещё не
   завершённому... учитывая синхронность fetch в тике, достаточно отбрасывать
   подряд идущие дубликаты одного курсора (второй запрос с тем же курсором без
   промежуточного обновления данных = дубль).
5. Heartbeat-вокруг-fetch: перед `platform::get` в `load_catalog` отправить
   heartbeat и сбросить `last_heartbeat_ack` (перенести `load_catalog` в метод
   `Session` или передать нужные поля — по усмотрению исполнителя, FFI и
   обработчики не менять).
6. Логирование: `platform::log_msg` с курсором (обрезанным) и режимом — для
   приёмки через serial.

**Приёмка.** `cargo test`; прошивка: `CMD:UI:CATALOG` — в serial-логе виден
`limit=20` без курсора; после UC-4 — с курсором. Серийный стресс: несколько
быстрых «догрузок» не порождают дублирующихся GET (лог/счётчик запросов).

**Границы.** Не менять DTO/эндпоинты, не трогать C-код (кроме ничего — C шлёт
курсор только после UC-4), не выносить fetch в отдельную задачу (бэклог).

---

### UC-3. C (LVGL): объединить Browse+Search в один экран Catalog

**Контекст.** Два параллельных экрана: реестр `rock_gui.c:75-79`
(`VIEW_BROWSE`, `VIEW_SEARCH` — у каждого свои build/show/hide), свои массивы
строк (`s_browse_rows[20]`, `s_search_rows[20]`), свои sink-функции, автореин
`gui_browse_set_stations`/`gui_search_set_results` сами свитчат вью. Дубль
зафиксирован в [`refactoring-audit.md`](refactoring-audit.md) («Списки станций:
`set_browse_stations` ≡ `set_search_results`»). В этом шаге пагинации ещё нет —
поведение данных как сейчас (первая страница / замена), меняется только экран.

**Целевой UI** (по rockmobile, токены из `rockcast-design-tokens.md`):
- один экран `VIEW_CATALOG`: сверху — однострочный search-bar (textarea, maxlen
  128, кнопка очистки) и ряд чипов жанров (7, как сейчас в поиске);
- ниже — список станций (контейнер 768×236, как сегодня в поиске), строка —
  существующий общий `station_row_ui_t`;
- тап по search-bar → показать клавиатуру поверх области списка
  (`rock_gui_keyboard.c`, как сейчас); OK/`LV_EVENT_READY` → enqueue
  `ROCK_UI_EVT_CATALOG_SEARCH`; чип жанра заполняет textarea и сабмитит (как :19-28);
- кнопка «назад» в хедере → `VIEW_NOW_PLAYING` (как сейчас);
- кнопка «Каталог» на Now-Playing ведёт на новый экран (замена обеих кнопок
  «Каталог»/«Поиск» — `rock_gui_views_now_playing.c:56-67`).

**Задачи.**
1. Реестр: `VIEW_BROWSE`+`VIEW_SEARCH` → один `VIEW_CATALOG` (файлы
   `rock_gui_views_browse.c`+`rock_gui_views_search.c` → один
   `rock_gui_views_catalog.c`; «общий list-widget» из аудита здесь же:
   один массив строк и одна `gui_catalog_set_rows`).
2. Обновить потребителей: `rock_gui_switch_view(int)` (индексы!),
   `rock_gui_trigger` (`CMD:UI:CATALOG`, `SEARCH`, `SEARCH:<q>`, `STATION:<row>`,
   `BACK` — должны работать), `CMD:VIEW:` в `rock_serial.c` → `NP|CATALOG`
   (алиасы `BROWSE`/`SEARCH` оставить как синонимы `CATALOG`, чтобы старые
   прогонки tools не падали), `rock_gui_mark_catalog_stale`, оверлейные «Назад»
   (`rock_gui_widgets.c:281-287`).
3. `rock_gui_log_debug_state`: печатать `VIEW=CATALOG` и счётчики — синхронно
   обновить парсеры в `tools/` (греп по `VIEW`).
4. Русские строки — через `src/presentation.rs` (`tr::`) для Rust-части и явно
   в C, по образцу существующих.

**Приёмка.** `idf.py build`; на железе: `CMD:VIEW:CATALOG` открывает экран;
`CMD:UI:SEARCH:rock` показывает результаты в том же экране; `CMD:UI:CATALOG` —
возврат к первой странице; `CMD:SCREENSHOT` — скриншоты приложить к коммиту;
`CMD:UI:STATION:0` запускает воспроизведение. Поиск и каталог — один экран,
переключений вью не видно.

**Границы.** Не трогать Rust (кроме строк, если понадобятся), Now-Playing, Cast,
клавиатуру как компонент. Пагинация — НЕ в этом шаге.

---

### UC-4. Пагинация: накопление в PSRAM, окно 20 строк, append-FFI, скролл-триггер

**Контекст.** После UC-2 Rust умеет слать `GET ...&cursor=`, после UC-3 есть
один экран. Сейчас `rock_gui_set_browse_stations` заменяет до 20 строк, курсор
выбрасывается (`rock_gui.c:446-454`). Сервер: страница ≤20, `next_cursor` есть
у полной страницы, `None` = конец. В каталоге 41 станция (3 страницы).

**Задачи — C.**
1. `rock_gui.h`: константа `ROCK_GUI_CATALOG_MAX_ACCUMULATED 128`; новые FFI:
   ```c
   // Сброс накопленного списка и запись первой страницы.
   void rock_gui_catalog_begin(const rock_gui_station_item_t *items, size_t count,
                               const char *next_cursor);
   // Догрузка: дедуп по id, append, обновить курсор.
   void rock_gui_catalog_append(const rock_gui_station_item_t *items, size_t count,
                                const char *next_cursor);
   ```
   `rock_gui_set_browse_stations` сделать тонкой обёрткой над `begin` (или
   удалить, обновив Rust-вызовы).
2. Накопленный список: `rock_gui_station_item_t *s_catalog` — `heap_caps_malloc(
   ROCK_GUI_CATALOG_MAX_ACCUMULATED * sizeof(...), MALLOC_CAP_SPIRAM)` при первом
   использовании; плюс `s_catalog_count`, `s_catalog_cursor[513]`,
   `s_window_start` (индекс первой видимой строки). Все обращения — под
   LVGL-локом; освобождать в `rock_gui_mark_catalog_stale`/при переполнении cap.
3. Окно: 20 существующих строк-виджетов заполняются из `s_catalog[
   s_window_start .. s_window_start+20)`. На `LV_EVENT_SCROLLED` контейнера:
   - прокрутка к нижнему краю окна и `s_window_start+20 < s_catalog_count` →
     сдвинуть окно вперёд (на видимой шаг, ≤20) и перезаполнить; скомпенсировать
     `lv_obj_scroll_to_y(..., LV_ANIM_OFF)`, чтобы скролл не «прыгал»;
   - аналогично вверх для возврата;
   - нижний край накопленного списка + непустой `s_catalog_cursor` + не идёт
     загрузка → `gui_enqueue_event(ROCK_UI_EVT_CATALOG_BROWSE, s_catalog_cursor,
     0)` и показать пилюлю «Подгрузка...» внизу списка; флаг-предохранитель
     `s_fetching_more` снимается в `begin`/`append`/показе оверлея ошибки
     (страховка от таймаута: авто-снятие через ~15 с через lv_timer).
4. Счётчик в хедере списка — от `s_catalog_count` («41 станция», плюрализация
   уже есть). Пустой курсор → пилюля «Конец списка» (или ничего), триггер
   отключён. Достижение cap'а — то же, что конец.
5. `rock_gui_trigger`: добавить действие `LOADMORE` (эмулирует скролл-триггер —
   для приёмки без тача).

**Задачи — Rust.**
6. `platform/gui.rs`: обёртки `gui_catalog_begin`/`gui_catalog_append` (по образцу
   `gui_set_browse_stations`, включая курсор через `cstring`); в
   `transport/catalog.rs` `Refresh` → `begin`, `LoadMore` → `append`.

**Приёмка.** На железе с 41 станцией: открыть каталог → 20 строк; доскроллить/
`CMD:UI:LOADMORE` → строки 21–40 без возврата к началу; ещё раз → 41–41, «конец»;
счётчик «41 станция»; повторные LOADMORE дедуплицированы (лог); `CMD:METRICS` —
free internal heap не просел (буфер в PSRAM); после UC-5 быстрый переход
поиск→назад сохраняет накопленный список и позицию скролла.

**Границы.** По-прежнему не трогаем rockserver; `ROCK_GUI_MAX_STATIONS` (окно)
не увеличивать; полный async fetch — бэклог.

---

### UC-5. Режим поиска как в rockmobile: замена видимого списка, возврат без рефетча

**Контекст.** Rockmobile ([StationRepository.kt:46-63](C:/repos/rockmobile/app/src/main/java/com/rockmobile/data/repository/StationRepository.kt),
[StationsViewModel.kt](C:/repos/rockmobile/app/src/main/java/com/rockmobile/ui/stations/StationsViewModel.kt)):
результаты поиска — отдельный «каталог», который подменяет видимый список; при
очистке запроса происходит возврат к прежнему каталогу; сбой поиска не затирает
список. У нас после UC-4 каталог накоплен в C — возврат возможен локально, даже
проще, чем в rockmobile (там рефетч).

**Задачи (только C, поверх UC-3/UC-4).**
1. Отдельное хранилище результатов: `s_search_items[ROCK_GUI_MAX_STATIONS]` +
   `s_search_count` + `s_search_active`. `rock_gui_set_search_results` пишет его,
   переключает видимый список на поиск (окно перезаполняется из поиска),
   `s_search_active=true`.
2. Кнопка очистки search-bar / «назад» из результатов: локальный возврат —
   `s_search_active=false`, окно из `s_catalog`, **без enqueue и без HTTP**;
   позиция скролла каталога восстановить (хранить `s_window_start` каталога
   отдельно от окна поиска).
3. Rust при поиске НЕ трогает каталог: `load_catalog(Search)` зовёт только
   `gui_set_search_results` (уже так — убедиться и зафиксировать тестом логов).
4. Сбой поиска (оверлей ошибки) не очищает ни каталог, ни прошлые результаты;
   Retry повторяет поиск (существующая реинъекция).

**Приёмка.** На железе: накопить 41 станцию → `CMD:UI:SEARCH:rock` → результаты
(≤20) в том же экране; очистить запрос → каталог из 41 на прежней позиции, в
serial-логе **нет** нового GET каталога; `CMD:UI:SEARCH:<мусор>` + ошибка сервера
→ оверлей, после «Повторить»/«Назад» список цел.

---

### UC-6. Состояния и деградация

**Контекст.** Сейчас любая загрузка показывает полноэкранный оверлей
`gui_show_loading` (`transport/catalog.rs:95-99`) — при догрузке это стирает
контекст. Пустые состояния: «Каталог пуст» / «Ничего не найдено» — разные
оверлеи, в едином экране должны зависеть от режима.

**Задачи.**
1. Rust: `Refresh` (первая загрузка) — полноэкранный оверлей как сейчас;
   `LoadMore` и `Search` — БЕЗ полноэкранного оверлея: поиск — спиннер в
   search-bar (или пилюля), догрузка — пилюля «Подгрузка...» из UC-4.
   При необходимости добавить крошечный FFI `rock_gui_catalog_set_busy(bool)`.
2. Пустые состояния: в `VIEW_CATALOG` пустая страница каталога → «Каталог пуст»,
   пустые результаты поиска → «Ничего не найдено» с «Назад» к каталогу
   (локальный возврат UC-5).
3. Таймауты/ошибки HTTP при догрузке: пилюля-ошибка внизу списка + повтор по
   тапу (enqueue того же курсора), список не прячется. Полный оверлей ошибки —
   только для `Refresh`.
4. `rock_gui_mark_catalog_stale`: погасить строки, освободить PSRAM-список,
   сбросить счётчики/курсор/окно; сообщение о потере связи — как сейчас.

**Приёмка.** Скриптовый прогон по serial: все переходы (первая загрузка /
догрузка / поиск / пусто / ошибка / offline) не оставляют «застрявших» оверлеев;
список виден во время догрузки и поиска.

---

### UC-7. Приёмка, инструменты, документация

**Задачи.**
1. `tools/`: синхронизировать парсеры debug-state с новым форматом (UC-3);
   при необходимости расширить `watch_serial.py` сценарием
   каталог→догрузка×2→поиск→очистка→поиск→ошибка.
2. Скриншоты финального UX (`CMD:SCREENSHOT`) → `docs/uc-catalog-*.png`
   (по образцу `re6-*.png`).
3. [`refactoring-audit.md`](refactoring-audit.md): закрыть пункт
   «Списки станций: `set_browse_stations` ≡ `set_search_results`» и строку про
   общий list-widget в плане разбиения `rock_gui.c`.
4. [`memory-budget.md`](memory-budget.md): +48 КиБ PSRAM под накопленный список,
   факт после замеров (`CMD:METRICS`, `rock_gui_get_metrics`).
5. [`plan-control.md`](plan-control.md): отметить план выполненным.
6. Итоговая физическая приёмка: полный чек-лист из раздела 7 на железе.

**Приёмка.** Документы обновлены; скриншоты приложены; финальный прогон зелёный.

---

## 7. Стратегия проверки (сквозная)

1. **Host**: `cargo test` — контрактные тесты (UC-1 расширяет).
2. **Сборка**: `idf.py build` (skill `esp-idf-tools`).
3. **Прошивка + serial** (образец — `tools/watch_serial.py`):
   - `CMD:VIEW:CATALOG` → экран открыт, 20 строк, счётчик;
   - `CMD:UI:LOADMORE` ×2 → 41 станция, «конец», без дублей GET (лог);
   - `CMD:UI:SEARCH:rock` → результаты; очистка → каталог без GET;
   - `CMD:UI:STATION:0` → воспроизведение, meta, стоп;
   - `CMD:METRICS` → internal heap стабилен; `CMD:SCREENSHOT` → визуально.
4. **Деградация**: заглушить сервер → оверлеи/пилюли по UC-6, восстановление.

## 8. Риски

| Риск | Митигация |
| --- | --- |
| «Прыжок» скролла при сдвиге окна | Компенсация `scroll_y`; фолбэк — постраничные стрелки вместо непрерывного скролла (решение принимает исполнитель UC-4 по факту на железе). |
| Переполнение очереди событий (32) при быстром скролле | Флаг `s_fetching_more` + дедуп курсора в Rust (UC-2/UC-4). |
| Голодание heartbeat при сериях догрузок | Heartbeat-вокруг-fetch (UC-2, образец в `gui_sink.rs:162-176`); полный async — бэклог. |
| Регресс internal RAM | PSRAM-плейсмент обязателен; замеры `CMD:METRICS` в приёмке UC-4/UC-7. |
| Дрейф строк в карточках | Карта кода сверяется по символам, не по номерам строк. |

## 9. Бэклог (осознанно вне плана)

- **Серверный курсор для ранжированного поиска** (RS-1-поправка, openapi 0.7.0):
  нужна только если захотим догрузку результатов поиска >20. Варианты и оценки —
  в журнале обсуждения 2026-10-13 (offset-курсор поверх `SearchConstraints.offset`
  или `q` на browse-эндпоинте по образцу `list_admin`).
- Полная асинхронность каталогного fetch (отдельная задача + очередь результатов).
- Локальный фильтр по накопленному списку (мгновенный отклик, офлайн-поиск) —
  аналог `filterStations` в rockmobile.
- Персистенция каталога во flash (NVS/LittleFS) для офлайн-старта, с пином
  версии по SHA-256 (как bundled-каталог rockmobile).
- Подсветка `is_playing` текущей станции в накопленном списке.
