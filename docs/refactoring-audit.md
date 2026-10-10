# Архитектурный аудит и план рефакторинга (2026-10-10)

Охват: весь компилируемый C-код `main/` (~12K строк без шрифтов) и Rust-код
`src/` (~4.5K строк), FFI-швы в обе стороны, заголовки как ABI. Метод:
полное чтение файлов четырьмя независимыми проходами (BSP-группа,
GUI/platform-группа, audio/display/OTA-группа, Rust/FFI-группа) с перекрёстной
проверкой самых тяжёлых находок по исходникам. Шрифты `font_*.c` (дата-файлы,
~8K строк) не аудировались и в плане не трогаются.

Контекст для калибровки: прошивка выросла из bring-up-проекта (RE-… задачи),
и «BSP»-суффикс файлов давно перестал означать board support — это главный
симптом всех находок ниже.

---

## 1. Сводка находок

### 1.1. Критичные дефекты (баги, не стиль)

| # | Дефект | Где |
|---|--------|-----|
| B1 | **Утечка секрета в консоль.** `rock_ws_send`/`rock_ws_receive` логируют полные кадры (`ESP_LOGI(TAG, "WSS tx: %s", data)`), а inbound `device.command` содержит `stream_uri` с `?token=…`. Rust-контракт прямо обещает обратное (`transport.rs:223`, тест `tests/device_control_contract.rs:391-446`) | `main/control_platform.c:610-628` |
| B2 | **Отравленное состояние после неудачного init.** `rock_player_init` выходит по ошибке после создания `s_state_mutex`, но не уничтожает его; guard `if (s_state_mutex) return 0;` превращает все последующие вызовы в «успех» с мёртвым аудио | `main/player_bsp.c:420-424` |
| B3 | **Неинициализированная память в `%s`.** `esp_app_desc_t app_desc;` заполняется только при успехе `esp_https_ota_get_img_desc`; при провале код продолжает цикл и передаёт мусорный `app_desc.version` в LVGL `lv_label_set_text_fmt` | `main/rock_ota.c:173-189, 205` → `main/display_bsp.c:678` |
| B4 | **TOCTOU на виджет-указателе между задачами.** `rock_ui_splash_dismiss` обнуляет `s_splash_status_lbl` без LVGL-лока; `rock_ui_splash_status` читает его под локом из сетевой задачи | `main/display_bsp.c:328-343`, вызовы из `control_platform.c:118,435,456,475` и `rock_gui.c:1224` |
| B5 | **Гонки на обычных глобалах между задачами:** `s_muted` читается аудио-задачей без мьютекса (`player_bsp.c:198-199` против записи `:586-588`); `s_upstream_bearer[600]` пишется из API-задачи, читается feeder'ом (`relay_bsp.c:1309` vs `:629`); `s_upstream_uri` очищается после join с таймаутом 12 c, feeder может ещё жить (`:1464` vs `:621`); `s_server_sock` общий для caller/feeder без синхронизации (`:92,:1021,:1092,:1454`); `s_ota_in_progress` — обычный bool между задачами (`rock_ota.c:16,:70-73,:130-236`); 64-битный `s_max_write_gap_us` на 32-битном RISC-V под `volatile` — возможны torn reads (`audio_bsp.c:126,:377-384,:720-749`) | см. ссылки |
| B6 | **Функциональные статики — общие буферы поколений задач.** `in_buf/out_buf` в `stream_rx_task` объявлены `static` внутри функции; новый таск спавнится через ~600 мс после стоп-сигнала, старое поколение может ещё писать в те же буферы | `main/player_bsp.c:279-280`, `:485-487` |

### 1.2. God objects (файлы с числом обязанностей)

| Файл | Строк | Обязанностей | Суть |
|------|-------|--------------|------|
| `main/rock_gui.c` | 1968 | ~12 | Экраны, навигация, тач, очередь событий, таймеры/FPS, **серийный CLI-сервер**, вызовы player/Wi-Fi API, **скриншот+RLE+base64-транспорт**, строковый протокол статусов `rock_gui_present`, UTF-8 форматирование |
| `main/relay_bsp.c` | 1477 | ~15 | HTTP-клиент, **самописный HTTP/1.1-сервер**, chunked-writer, TCP-листенер, кольцевой буфер, ICY-фильтр, MP3-синк, декодер, ресемплер 48k, WAV-контейнер, медиа-такт, FreeRTOS-таски, retry-политика, бизнес-политика passthrough/transcode, хранение bearer. **Ноль board-support кода** |
| `main/player_bsp.c` | 841 | ~12 | Аудио-HW, второй ring-buffer, decode-воркер, DAC-таск+PA, стейт-машина, громкость, **UI-оркестрация (8 вызовов `rock_ui_*` из аудио-тасков)**, bearer, SSRF-FFI, телеметрия, **продакшн-селфтест с 16 `assert()` и 100KB MP3** |
| `main/display_bsp.c` | 714 | ~8 | DSI/панель/LVGL/тач/подсветка + splash, legacy-экраны, QR ×2, **Wi-Fi scan list с таксономией auth-mode**, OTA-прогресс-бар. Определяет тип `rock_wifi_scan_item_t` в дисплевом заголовке |
| `main/control_platform.c` | 696 | ~8 | NVS+identity, Wi-Fi STA+реконнект, RF-survey, скан+пуш в UI, HTTP-обёртка, WSS-клиент, SNTP/RNG, **сброс C6 по GPIO**, OTA-хуки. Заголовок — чистый ABI для Rust, реализация — grab-bag |
| `src/firmware/transport.rs` | 737 | ~12 (в одной функции `session()` — 477 строк) | WSS-сессия, heartbeat-watchdog, refresh токена, UUIDv4, **GUI-диспетчер с русскими UX-строками**, каталог+401-retry, политика локального/cast-вывода, исполнение команд |
| `src/firmware/platform.rs` | 803 | ~8 | ~45 extern'ов к 7 подсистемам, repr(C)-зеркала, магические номера событий, HTTP-семантика, RAII-TLS, доменная политика (ключи устройства, эвристика «wifi up») — при doc-комментарии «narrow FFI boundary» |
| `main/cast_bsp.c` | 460 | ~7 | Самописный HTTP-клиент на сокетах, 3 ручных JSON-парсера, mDNS+TXT, **сканер подсети**, политика имён, TLS-обёртка |
| `main/rock_onboarding.c` | 628 | ~5 | SoftAP, HTTP-сервер со 160 строками HTML/CSS/JS в C-строках, raw-socket DNS-сервер, QR, NVS-credentials |

Функции-гиганты (>80 строк): `build_now_playing_view` (207),
`session()` в transport.rs (477), `rock_source_start` (~135),
`rock_display_init` (131, без отката при частичном провале),
`build_cast_overlay` (146), `build_search_view` (120),
`rock_onboarding_run` (115), `rock_platform_init` (114),
`serial_cmd_task` (114), `ota_worker_task` (106), `scan_handler` (99),
`stream_rx_task` (~187), `cast_subnet_scan` (135).

### 1.3. Смешение слоёв

- **Инверсия зависимостей «снизу вверх»**: аудио-таск (`player_bsp.c:207`) и
  decode-воркер (`:240,:324,:376,:405`) напрямую зовут UI (`rock_ui_*`), с
  захардкоженными строками «BUFFERING», «Waiting for stream…». Сетевой слой
  `control_platform.c` зовёт `rock_ui_splash_status`/`rock_ui_wifi_scan_show`;
  OTA-воркер зовёт `rock_ui_ota_progress_show` (`rock_ota.c:205,217`).
- **Круговая зависимость GUI↔BSP**: `display_bsp.c` включает `rock_gui.h` и
  зовёт `rock_gui_present` (`:385,:398,:415`), а `rock_gui.c` зовёт
  `rock_ui_splash_dismiss` из display_bsp (`:1224`). UI разрезан на два модуля,
  ссылающихся друг на друга.
- **Протокол в транспорте**: `ws_frame_kind` (`control_platform.c:334-348`)
  сниффает JSON подстроками `"type":"…"` — таксономия протокола, которой
  владеет Rust, продублирована в C для логов.
- **UX-политика в Rust-транспорте**: русские строки, retry-UX и каденс
  индикатора связи живут в `transport.rs:152-547`.
- **Дисплей знает Wi-Fi-домен**: `rock_wifi_scan_item_t` определён в
  `display_bsp.h:54-59`, `get_auth_mode_str` — в дисплевом модуле.
- **Спектр знает плеер**: `audio_spectrum.c:2,83` жёстко читает снапшот из
  `player_bsp` (при том что `pcm_snapshot.h` — источник-агностичный).
- **Extern-декларации в телах функций** вместо заголовков:
  `rock_gui.c:1934-1935` (Wi-Fi), `relay_bsp.c:82` (`rock_get_local_ip`),
  `player_bsp.c:35` (Rust FFI), `rock_onboarding.c:24` (`rock_qr_encode` —
  функция, физически определённая в Rust `pairing.rs:67`).

Что **чисто** (сохранить как есть): весь CASTV2-протокол принадлежит Rust,
в C ноль JSON-парсинга продакшн-пути; SSRF-валидатор один на оба языка
(`protocol.rs` → export → проверка в `player_bsp.c:474`); esp-idf-sys тронут
ровно дважды в композиционном корне; `device_control` хост-тестируем,
продакшн-Rust почти без unwrap; `pcm_snapshot.h`, `board_bus.c`,
`rock_wifi_storage.c` — образцовые модули; `main.c` — чистый composition root.

### 1.4. DRY (главные дубли)

- **Decode-цикл ×4**: `player_bsp.c:336-390` (stream_rx_task), `:693-734`
  (bringup), `:772-791` (soak) + двоюродный в `relay_bsp.c:755-799` —
  идентичный каркас esp_audio_dec.
- **Ring-buffer реализован дважды**: `relay_bsp.c:136-302` (byte ring) и
  `player_bsp.c:40-131` (pcm ring) — одинаковая идея PSRAM+mutex+wraparound.
- **Wi-Fi скан ×4**: `rock_onboarding.c:41-55`, `:253-273`,
  `control_platform.c:144-154`, `:252-269` — все руками собирают
  `wifi_scan_config_t`.
- **Кнопка+лейбл бойлерплейт ×~20** в `rock_gui.c` (нет `make_button`).
- **Оверлей-квартет**: `show_loading/empty/offline/error`
  (`rock_gui.c:1478-1540`) — один и тот же каркас ×4.
- **Списки станций**: `set_browse_stations` ≡ `set_search_results`
  (`rock_gui.c:1387-1419` vs `:1434-1466`) — ~30 строк дословно.
- **JSON-скрейпинг ×3** в `cast_bsp.c:69-116`; IP-форматирование ×2
  (`:176-180`, `:228-232`).
- **Bearer-форматирование/валидация** ×3 (`relay_bsp.c:630,1305` vs
  `player_bsp.c:469,517`); content-type сниффинг ×2 файла.
- **PING/PONG-блок ×5** в `cast_client.rs:83-466`; wait-loop ×2
  (`:172-194`, `:200-222`); `GuiEvent`-матчинг ×2 (`mod.rs:41-74` vs
  `transport.rs:332-480`); `cstring` ×2, `bounded()` ×2 в Rust.
- **QR-рендер ×2** в `display_bsp.c:434-466` и `:579-645` (~30 строк).
- **NVS-бойлерплейт ×5** (3 в `rock_wifi_storage.c` + 2 в
  `control_platform.c:525-548`).
- **Кросс-языковые константы**: лимит кадра 65536 живёт в 4 местах
  (`protocol.rs:5`, `control_platform.c:35`, `platform.rs:8`,
  `transport.rs:10`); heartbeat 20 s ×2; watchdog 75 s (Rust) vs 60 s (C)
  несогласованные; магические номера событий 1..12 в `platform.rs:441-453`
  дублируют порядок enum в `rock_gui.h:23-37` (молчаливый дрейф при вставке).
- **Две системы цветов**: hex-литералы в `display_bsp.c` (~30 раз) против
  `ROCK_COLOR_*` в `rock_gui.c` при существующем `rockcast_tokens.h`.

### 1.5. Мёртвый код (~700+ строк C и ~100 KB флеша)

- `rock_audio_bringup_start` (`audio_bsp.c:784`) **не вызывается ничем** —
  всё достижимое только через него (~570 строк: tone, pcm_stats, cpu_mon,
  gt911_probe, loopback) — продакшн-мёртв, но компилируется.
- Bringup-селфтест плеера (`player_bsp.c:648-832`) + `test_mp3.h` (100 KB в
  прошивке) + 16 `assert()` (провал = ребут), триггер — `CMD:SELFTEST`.
- Мёртвые статикы (остатки архитектории до unified source): `s_stream_uri`,
  `s_stream_authorization` (из-за них `rock_player_update_bearer` — **no-op**),
  `s_stat_net_reconnects`, `s_stat_max_read_ms` (телеметрия врёт нулями в
  GUI `rock_gui.c:1933`), `s_tx_task`, `s_relay_task_handle`.
- Мёртвое API: `rock_ui_clock_init/set_text` (`display_bsp.c:345-353`),
  макрос `ROCK_CAST_MAX_DEVICES`, событие `ROCK_UI_EVT_VOLUME_SET`,
  `s_track_title/subtitle`, недостижимый `return` в `rock_onboarding_run:627`,
  `s_dns_running` никогда не сбрасывается, мёртвый include `rock_gui.h` в
  `rock_onboarding.c:4`.
- Дубли API-нейминга: `rock_relay_*` — псевдонимы `rock_source_*`
  (`relay_bsp.h:101,107`) — непонятно, что канонично.

### 1.6. Прочее значимое

- `rockcast_tokens.h` — UI-токены (цвета), имя сталкивается с
  auth-токенами (`rock_source_update_bearer` и т.п.).
- Тест-харнесы в продакшн-заголовках: `rock_audio_bringup_start` в
  `audio_bsp.h:13`, bringup-API в `player_bsp.h:20-25`.
- Заголовок `rock_gui.h:3` включает `player_bsp.h` ради одного enum — все
  потребители GUI транзитивно зависят от плеера.
- Молчаливые потери UI-апдейтов: все сеттеры GUI тихо выходят при таймауте
  лока (500/1000/2000 ms — три разных таймаута).
- Insecure TLS-постура каста (skip CN + skip cert verify) задокументирована
  только комментарием внутри `cast_bsp.c:360-369`.
- 64 KiB heap-аллокации на каждый HTTP-вызов в Rust (`platform.rs:238,273`).

---

## 2. План рефакторинга

Принципы «не сломать»:

1. **Каждый шаг собирается и прошивается.** Никаких долгоживущих веток с
   полусобранным состоянием; один PR = один файл или одна фаза.
2. **ABI-заголовки, которые биндит Rust** (`control_platform.h`, `rock_gui.h`,
   `relay_bsp.h`, `player_bsp.h`, `cast_bsp.h`, `display_bsp.h`), по
   возможности не менять сигнатуры вовсе; где меняются — только парным
   коммитом C+Rust в одном PR.
3. **Внутренности раньше интерфейсов**: сначала режем `.c`-файлы и чиним
   зависимости, переименования — последней фазой через `git mv`.
4. Проверка после каждого шага: `idf.py --no-ccache build` + `cargo test`
   (host-контракты) + smoke на железе (play/pause, browse, cast, скриншоты
   через `tools/capture_screen.py`); OTA-шаги — `tools/run_full_ota_test.py`.
5. Перед началом зафиксировать baseline: `idf.py size`, скриншоты экранов,
   серийный лог стабильной сессии.

### Фаза 1 — критичные баги и утечка секрета (маленькие точечные патчи)

Приоритет выше рефакторинга: это поведение, а не структура.

**Выполнено 2026-10-10** (5 файлов, +118/−48, заголовки/ABI не тронуты):

- B1 — из `rock_ws_send`/`rock_ws_receive` убраны логи полных кадров
  (`WSS tx/rx: %s`) вместе с if-блоками; логи `type=…`/`len=…` и
  `ws_frame_kind` сохранены.
- B2 — `rock_player_init`: полный teardown `s_state_mutex` на всех путях
  отказа после его создания (единая метка `fail:`).
- B3 — `esp_app_desc_t app_desc = {0}` + `fw_version = "unknown"` при
  провале `esp_https_ota_get_img_desc`; логика abort/continue не менялась.
- B4 — `rock_ui_splash_dismiss` берёт `lvgl_port_lock(500)` симметрично
  `rock_ui_splash_status` (лок рекурсивный, вложенный вызов из
  `rock_gui.c` безопасен).
- B5 — `s_ota_in_progress` и `s_muted` переведены на `atomic_bool`;
  `s_upstream_uri`/`s_upstream_bearer` защищены config-локом (запись в
  start/update/stop, чтение в `upstream_connect` через снапшот под локом;
  очистка в `rock_source_stop` только когда feeder завершился — при
  таймауте join lingering-feeder ещё может читать буферы, а пропущенная
  очистка безвредна, т.к. start перезаписывает их; `rock_source_update_bearer`
  до первого старта источника возвращает `ESP_ERR_INVALID_STATE`).
- B6 — `in_buf`/`out_buf` в `stream_rx_task` аллоцируются в куче в прологе
  и освобождаются на всех выходах (`fail`/`cleanup`); `dec` перенесён в
  пролог (NULL-init) из-за goto-безопасности.

Верификация: `idf.py --no-ccache build` — без новых предупреждений;
host `cargo test` 12 lib + 21 контрактный зелёные (вкл.
`stream_uri_never_leaks_into_logs_or_state_or_errors`); smoke на плате
(COM6): browse/play/pause/play, входящий `device.command` с stream_uri
принят, полных JSON-кадров в логе нет, строки `WSS send/receive type=…`
на месте. Полный OTA-стенд не поднимался (нужен хотспот RockLab-2g и
прошивка кандидатом v0.1.1): путь `err != ESP_OK` разобран кодом — в UI
уходит константа «unknown».

1. **B1 (логи кадров)**: в `rock_ws_send/rock_ws_receive` оставить только
   `type=… len=…`, убрать `WSS tx: %s` / `WSS rx: %s`. Проверить тестом, что
   токен не появляется в логе.
2. **B2**: в `rock_player_init` при провале после создания мьютекса — полный
   teardown (destroy mutex, обнулить указатель) и возврат ошибки.
3. **B3**: `esp_app_desc_t app_desc = {0};` + при провале чтения дескриптора
   показывать «unknown» вместо мусора (или не звать UI до успешного чтения).
4. **B4**: `rock_ui_splash_dismiss` взять `lvgl_port_lock` (или сделать
   указатель атомарным).
5. **B5 (точечно)**: `s_ota_in_progress` → atomic; чтение `s_muted` в
   `audio_tx_task` под мьютексом (или atomic); запись `s_upstream_bearer` —
   только между сессиями либо под мьютексом feeder'а.
6. **B6**: `in_buf/out_buf` из функциональных статиков `stream_rx_task` — в
   стек/кучу таска (размер уже известен), либо жёсткая гарантия завершения
   старого таска до спавна нового.

### Фаза 2 — вычистка мёртвого кода (−~700 строк C, −100+ KB флеша)

**Выполнено 2026-10-10** (16 файлов, +66/−92, плюс новый `sdkconfig.bringup`):

- Bring-up харнессы гейтированы **на месте** через `#if
  CONFIG_ROCK_BRINGUP_TESTS` (новый Kconfig-флаг, default n; заменил
  `ROCK_PLAYER_SELFTEST`). Вынос в отдельные файлы отложен до фазы 4 —
  сейчас он потребовал бы расшаривать приватные статики без выигрыша.
  Затегейчено: `audio_bsp.c` (tone/pcm_stats/cpu_mon/gt911_probe/loopback/
  capture/play_tone + их статики и объявления `rock_audio_bringup_start`),
  `player_bsp.c` (`player_bringup_task`, `rock_player_bringup_start`,
  `test_mp3.h`), `CMD:SELFTEST` в `rock_gui.c` (всегда компилируется; под
  гейтом — запуск, иначе лог «SELFTEST disabled»), вызов в `main.c`.
  Продакшн-путь (`rock_audio_init/reconfig/pa_set/rx_overflow_cb`,
  `pcm_ring_*`, `s_stat_*`) не гейтился. `s_loop`/`loop_counters_t` остались
  вне гейта: их пишет продакшн-колбэк `rx_overflow_cb`.
- Удалён no-op `rock_player_update_bearer` парной правкой C+Rust:
  `player_bsp.c/h` (включая мёртвые `s_stream_uri`/`s_stream_authorization`),
  `platform.rs` (extern+обёртка), `transport.rs` (вызов при старте сессии;
  refresh-цикл теперь безусловно обновляет настоящий источник через
  `rock_source_update_bearer` — раньше провал no-op молча пропускал refresh).
- Удалены мёртвые: `s_stat_max_read_ms` (+поле из unparsed строки stream
  metrics), `s_tx_task`, `s_relay_task_handle` (в xTaskCreate… → NULL),
  `rock_ui_clock_init/set_text`, `ROCK_CAST_MAX_DEVICES`, `s_track_title/
  subtitle`, мёртвый include `rock_gui.h` в `rock_onboarding.c`,
  недостижимый `return ESP_OK` после `while(1)`, `s_dns_running` (цикл
  упрощен до `while(1)` с комментарием). Гигиена: `<string.h>` добавлен,
  неиспользуемый `esp_check.h` убран (`display_bsp.c`).
- `ROCK_UI_EVT_VOLUME_SET` НЕ удалян (запрещено: номер — часть числового
  FFI-контракта `platform.rs`), помечен комментарием reserved.
- Телеметрия reconnects стала настоящей: `relay_bsp.c` считает успешные
  переподключения упавшего апстрима (`upstream_pump` retry-loop; первичное
  подключение не считается; сброс в `rock_source_start`), экспорт
  `rock_source_reconnect_count()` (аддитивный ABI, Rust-биндинг не нужен),
  `rock_player_get_stats` берёт значение оттуда; формат GUI-строки метрик
  не менялся (его парсит `tools/rock_hwtest.py`).
- `sdkconfig.bringup` (одна строка `CONFIG_ROCK_BRINGUP_TESTS=y`) закоммичен
  в корне — воспроизводимая сборка приёмки:
  `idf.py -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.bringup' build`.
- Исправлена доисторическая поломка харнеса плеера (обнаружена при проверке
  bringup-сборки на плате): встроенный MP3 (~1.3 с ≈ 230 КБ PCM) за один
  проход не достигал `PREROLL_MIN_BYTES` = 512 КБ → статус оставался
  BUFFERING → ассерт `pause()` в TEST 3/5 падал → плата ребуталась (харнес
  автостартует из `main.c`, семантика старого SELFTEST-флага). TEST 2/5
  теперь зацикливает клип, как это всегда делала soak-петля. Правка только
  внутри гейтнутого кода.

Дельта размеров (idf.py size, дефолтная сборка, до/после): Flash
2 385 294 → 2 358 904 (**−26 390**; .rodata 739 088 → 715 904 = −23 184,
.text 1 645 630 → 1 642 424 = −3 206), bin 0x25EC90 → 0x258470 (−29 216),
DIRAM 216 950 → 205 830 (−11 120). Меньше исходной оценки «−100+ KB» по
двум причинам: `test_mp3.h` — это 101 540 байт *исходника* (hex-текст), а в
бинарнике ~20.7 KB констант; аудио-харнес (~570 строк) линкер уже вырезал в
базлайне (--gc-sections, вызовов не было). Гейт убирает код из компиляции
вообще и возвращает явный контроль; bringup-сборка с гейтом собирается
чисто (bin 0x25E9B0 ≈ базлайн).

Верификация: `idf.py --no-ccache build` (дефолт и bringup) — чисто, без
новых предупреждений (Rust: ровно два pre-existing `recover`/
`rock_hosted_recover`, не тронуты); host `cargo test --target
x86_64-pc-windows-msvc` 12 lib + 21 контрактный зелёные; smoke на плате
(COM6, дефолтная сборка): `tools/rock_hwtest.py` **12/12** (WSS за 14.8 s,
browse/search 200, станция играет, pause/volume/stop, heartbeat ≤20 s,
CMD:METRICS матчится regex-ом с `reconnects=`), `CMD:SELFTEST` отвечает
«SELFTEST disabled (build with CONFIG_ROCK_BRINGUP_TESTS=y)», полных
WSS-кадров/stream_uri/bearer в логе нет. Bringup-сборка на плате: харнес
автостартует с бута и завершается «RE-5 PLAYER HARDWARE VERIFICATION:
ALL PASSED!» (soak 30 s: underruns=0, dec_errors=0, WSS-сессия живёт
параллельно); после проверки плата возвращена на дефолтную прошивку.

1. Bring-up харнессы (`audio_bsp.c` ~570 строк; `player_bsp.c` bringup-таск +
   `test_mp3.h` 100 KB; 16 `assert()`) вынести в отдельные файлы
   `rock_bringup_audio.c` / `rock_bringup_player.c` под Kconfig
   `CONFIG_ROCK_BRINGUP_TESTS` (default **n**). Не удалять: `CMD:SELFTEST`
   используется при приёмке платы — переключаем флагом, когда нужен.
2. Удалить мёртвые статикы и API (список в §1.5). Отдельно: решить судьбу
   `rock_player_update_bearer` (сейчас no-op) — либо подключить к
   `rock_source_update_bearer`, либо удалить с вызовами.
3. Удалить мёртвые: `rock_ui_clock_*`, `ROCK_CAST_MAX_DEVICES`,
   `ROCK_UI_EVT_VOLUME_SET`, `s_track_title/subtitle`, мёртвый include,
   unreachable-return, `s_dns_running`.
4. Телеметрия: убрать всегда-нулевые метрики из GUI-лога или начать их
   реально считать.
5. Kconfig-гейт заодно закрывает «тесты в продакшн-заголовках» —
   bringup-API уходит из `audio_bsp.h`/`player_bsp.h`.

### Фаза 3 — разрыв смешения слоёв (инверсия зависимостей, без переездов)

Приём: заменить прямые вызовы «вверх» на колбеки, выставляемые в
композиционном корне (`main.c`). Файлы не переезжают — меняется направление
стрелок.

**Выполнено 2026-10-10** (16 файлов изменено +281/−815; 7 новых файлов
+830 строк: `rock_ui_screens.c/h`, `rock_serial.c/h`, `rock_screenshot.c/h`,
`rock_ffi_exports.h`):

- **A. player → UI через колбеки**: `player_bsp.h/.c` — enum
  `rock_player_ui_state_t` (BUFFERING/STALLED/PLAYING/PAUSED/ERROR),
  `rock_player_ui_cbs_t{on_playback_state}` + `rock_player_set_ui_cbs()`;
  все 9 вызовов `rock_ui_*` (7 продакшн + 2 bringup внутри
  `#if CONFIG_ROCK_BRINGUP_TESTS`) заменены на `notify_ui()` c NULL-чеком;
  include `display_bsp.h` удалён. UI-строки («BUFFERING»,
  «Waiting for stream…», «RockCast Radio», «PLAYBACK ERROR») переехали в
  адаптер `player_ui_on_playback_state` в `main.c`; регистрация сразу после
  `rock_player_init()`, до `rust_main()`.
- **B. platform/OTA → UI через колбеки**: `rock_wifi_scan_item_t` переехал
  из `display_bsp.h` в `control_platform.h` (владелец Wi-Fi-домена; Rust тип
  не биндит); `rock_platform_ui_cbs_t{on_splash_status,on_wifi_scan}` +
  сеттер; 6 вызовов `rock_ui_*` в `control_platform.c` → колбеки;
  `rock_ota_set_progress_cb()`, оба вызова `rock_ui_ota_progress_show` →
  колбек; include `display_bsp.h` удалён из обоих .c. Регистрации в `main.c`:
  платформа — до `rust_main()` (`rock_platform_init` зовётся из Rust), OTA —
  рядом с `rock_ota_init()`. `rock_onboarding.c`→UI не трогали (легальное
  направление app→UI).
- **C. display_bsp → только hardware**: экраны splash, legacy-презентация,
  pairing, Wi-Fi-scan, onboarding, OTA-бар переехали в новый
  `rock_ui_screens.c` (485 строк; статик-виджеты и `s_qr_buffer` с ними);
  QR-рендер ×2 свёрнут в один `qr_canvas_render()` — константы 320/4/177 в
  одном месте. `display_bsp.c` = 239 строк: DSI/panel/LVGL/тач/подсветка;
  include `rock_gui.h` удалён — **круг GUI↔BSP разорван**. Делегирование
  `rock_gui_present/rock_gui_is_active` (быв. display_bsp.c:379-415) уехало
  в `rock_ui_screens.c` — теперь это делегация внутри UI-слоя. `display_bsp.h`
  — только `rock_display_init`; остальные объявления в `rock_ui_screens.h`.
- **D. Серийный CLI → `rock_serial.c`**: `serial_cmd_task` (парсинг CMD:* +
  USB-JTAG I/O, 104 строки, TAG «rock-serial») вынесен из `rock_gui.c`;
  GUI получил узкие публичные `rock_gui_trigger(action)` (вся if-цепочка
  CMD:UI: на статик-виджетах под LVGL-локом) и `rock_gui_log_debug_state()`
  («ui view=…» логирует владелец статиков). `rock_gui.h` больше не включает
  `player_bsp.h`: локальный `rock_gui_playback_t` — числовое зеркало
  `rock_player_status_t` (0..5, ABI идентичен; Rust-биндинг в platform.rs
  передаёт сырой c_int и не менялся). Задача стартует из `rock_gui_start()`
  (виджеты должны существовать, а `rust_main()` не возвращается —
  композиционный корень не может секвенировать после старта GUI).
- **E. Скриншоты → `rock_screenshot.c`**: `rock_gui_dump_screenshot` +
  b64/RLE-транспорт (128 строк) вынесены из `rock_gui.c`; единственный
  потребитель — `rock_serial.c`.
- **F. Extern-декларации → заголовки**: новый `rock_ffi_exports.h` —
  сигнатуры `rock_validate_stream_uri`/`rock_qr_encode` дословно из
  `#[no_mangle]`-определений (protocol.rs/pairing.rs); подключён в
  `player_bsp.c` (быв. extern :35) и `rock_onboarding.c` (:24);
  `relay_bsp.c:82` extern `rock_get_local_ip` → `#include
  "control_platform.h"` (объявление функции добавлено в заголовок —
  аддитивно; include-цикла нет, control_platform.h self-contained).
- **G. `ws_frame_kind` удалён из C**: substring-сниффинг JSON (таксономия
  протокола, которой владеет Rust) убран; C логирует только
  «WSS send result=%d» / «WSS receive len=%d». Kind-логирование перенесено в
  Rust: `transport.rs` после успешного `parse_frame` зовёт новый C-хелпер
  `rock_ws_log_rx_frame(kind)` (по образцу `rock_ws_log_heartbeat_timeout`;
  прямых вызовов ESP-IDF-лога из Rust не появилось — esp_idf_sys по-прежнему
  только в композиционном корне). Формат «WSS receive type=%s» сохранён —
  его парсит `tools/rock_hwtest.py:218`. Бонус точности: kind теперь из
  разобранного Envelope, а не угадан подстроками — в логе виден
  «directory.snapshot», которого в старом хардкод-списке не было.

Дельта размеров (idf.py size, дефолт, до/после): Flash 2 358 904 → 2 361 574
(**+2 670**), DIRAM 205 830 → 205 846 (+16), bin 0x258470 → 0x258ee0 (+752) —
цена колбек-адаптеров и новых модульных гранул при −815 строках в
god-object-файлах (display_bsp.c 714→239, rock_gui.c 1968→1794).

Верификация: после каждого шага A–G `idf.py --no-ccache build` — чисто, без
новых предупреждений (Rust: ровно два pre-existing `recover`/
`rock_hosted_recover`); bringup-сборка
(`-D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.bringup'`) после C и
финально — чисто (bin 0x25f3f0; bringup-сайты плеера и CMD:SELFTEST в
`rock_serial.c` компилируются под гейтом — оба преобразованы на колбеки
внутри `#if`); host `cargo test` 12 lib + 21 контрактный зелёные.
Grep-барьеры: в `player_bsp.c`/`control_platform.c`/`rock_ota.c` нет вызовов
`rock_ui_*` и include `display_bsp.h`; в `display_bsp.c` нет `rock_gui_*`; в
`rock_gui.c` нет `usb_serial_jtag` и extern-деклараций; в `display_bsp.h` нет
`rock_wifi_scan_item_t`. Smoke на плате (COM6, дефолтная сборка):
`tools/rock_hwtest.py` **14/14** (WSS за 16.1 s, browse/search 200, 3/3
станции playing — колбеки BUFFERING→PLAYING вживую, pause/resume/stop/volume,
heartbeat worst gap 22 s, post-TTL reload каталога); вручную: CMD:SELFTEST →
«SELFTEST disabled (build with CONFIG_ROCK_BRINGUP_TESTS=y)», CMD:METRICS →
обе строки на месте («metrics heap=…» от rock-serial + «ui view=…» от
rock-gui/rock_gui_log_debug_state), CMD:VIEW:BROWSE/NP и CMD:SCREENSHOT
(маркеры START/END) работают; в 20-минутном логе 79 строк
«WSS receive type=…» (kind из Rust), «WSS send result=…»/«WSS receive len=…»
на месте, полных WSS-кадров/stream_uri/токенов нет. Первый прогон hwtest дал
13/14 только из-за позднего attach (WSS-строка бута прошла мимо окна) —
повтор flash+hwtest подряд закрыл и его.

1. **player → UI**: в `player_bsp` вместо восьми `rock_ui_*` — структура
   колбеков `rock_player_ui_cbs_t` (on_status, on_now_playing, on_text),
   регистрируется в `main.c`, который подключает её к display/gui. Аудио-слой
   перестаёт знать UI и его строки; строки «BUFFERING» и пр. переезживают
   владельцу колбеков.
2. **platform/OTA → UI**: аналогично `rock_ui_splash_status`,
   `rock_ui_wifi_scan_show`, `rock_ui_ota_progress_show` → колбеки.
3. **GUI↔BSP круг**: из `display_bsp.c` оставить только hardware (DSI, panel,
   LVGL, тач, подсветка). Экраны splash/onboarding/QR/wifi-scan/OTA-bar
   перенести в новый `rock_ui_boot.c`; `rock_wifi_scan_item_t` переехать в
   Wi-Fi-модуль; `display_bsp.h` перестаёт включать/знать `rock_gui.h`.
   QR-рендер ×2 свернуть в один хелпер здесь же.
4. **Серийный CLI** из `rock_gui.c:1855-1968` → `rock_serial.c`
   (диагностика); вместе с ним уходит зависимость GUI от `player_bsp.h`
   (заодно убрать include из `rock_gui.h:3` — статус передавать enum'ом GUI).
5. **Скриншот-dump** (`rock_gui.c:1751-1853`) → `rock_screenshot.c`.
6. Extern-декларации в телах функций → нормальные заголовки (4 места из §1.3).
7. `ws_frame_kind` в C: убрать substring-сниффинг — Rust уже знает тип
   кадра; C логирует только длину (тип логирует Rust у себя).

### Фаза 4 — распил god objects (по одному файлу за PR)

Порядок — от наиболее механического к наиболее тонкому:

1. **`control_platform.c` (696)** → внутри разделить на `rock_wifi_sta.c`
   (STA, реконнект, скан, survey), `rock_net_ws.c` (HTTP/WSS-обёртки),
   `rock_identity.c` (NVS identity). **`control_platform.h` не меняется** —
   это ABI Rust; файлы-потомки включают приватный `control_platform_internal.h`.
2. **`relay_bsp.c` (1477)** → `rock_stream_source.c` (upstream-клиент,
   декодер, ресемплер, pacing, политика passthrough/transcode),
   `rock_relay_http.c` (листенер, serve-петли, chunked-writer),
   `rock_ring.c` (byte ring; сюда же потом унифицировать pcm_ring).
   Схлопнуть дубли: prefill/feeder-петли ×2, serve-петли ×2, retry-петли ×3,
   двойные fail-пути. Псевдонимы `rock_relay_*` пометить deprecated в
   заголовке (удаление — фаза 6).
3. **`player_bsp.c` (841)** → decode-цикл ×3 → одна функция
   `rock_decoder_process(...)`; pcm_ring → `rock_ring.c`; UI уже на
   колбеках (фаза 3); bringup уже за Kconfig (фаза 2). Остаётся
   ~300-400 строк собственно плеера.
4. **`rock_gui.c` (1968)** → каталог `gui/`: `gui_views_now_playing.c`,
   `gui_views_browse.c` (+search — общий list-widget закрывает дубли
   `set_browse/set_search`), `gui_views_cast.c`, `gui_widgets.c`
   (`make_button`, row-list, единый `overlay_set(kind,title,desc,buttons)`),
   `gui_keyboard.c`, `gui_events.c`. View-registry (таблица
   `{id, build, show, hide}`) вместо enum+7 мест правки — добавление экрана
   = один файл + одна строка в таблице (OCP). Заголовок `rock_gui.h` —
   фасад, сигнатуры сохраняются для Rust.
5. **Rust `platform.rs` (803)** → подмодули `platform/{ffi,http,gui,cast,tls}.rs`
   с сохранением pub-интерфейса модуля (consumers не меняются). Доменную
   политику (ключи устройства, RSSI-эвристику) вынести из FFI-слоя в
   `device_control`/`firmware`-логику.
6. **Rust `transport.rs` (737)** → `session()` (477 строк) разбить:
   heartbeats, token refresh, catalog, source-политика (`SourceState` уже
   есть как зачаток), command-exec (двойной local/cast-путь). UX-строки →
   в `presentation`-модуль. Дублирующийся `GuiEvent`-матчинг из
   `mod.rs:41-74` и `transport.rs:332-480` → один диспетчер.
7. **`cast_bsp.c` (460)**: 3 JSON-парсера заменить возвратом сырого ответа и
   парсингом в Rust (в C не должно быть JSON — конвенция проекта уже такая);
   оставшиеся дубли (IP-формат ×2, socket-опции — общий хелпер с relay).

### Фаза 5 — кросс-языковая DRY и контракт C↔Rust

1. **Один источник констант**: `main/rock_protocol_abi.h` (лимит кадра,
   heartbeat-интервал, watchdog, номера событий `rock_ui_event_type_t`).
   esp-idf-sys уже генерирует биндинги из заголовков компонента — Rust берёт
   константы оттуда вместо литералов (`platform.rs:8`, `transport.rs:10`,
   магия 1..12 в `:441-453`). Это устраняет молчаливый дрейф.
2. Content-type: enum `rock_source_content_type_t` в shared-заголовке;
   Rust-лестница `CAST_LOAD_CONTENT_TYPES` и C-сниффинг читают его.
3. Согласовать watchdogи 75/60 s (одно число в ABI-заголовке).
4. Rust error-handling: `Result<(),()>` ×30 и `let _ =` ×31 в `firmware` —
   как минимум логировать проглоченные ошибки; `unreachable!()` в
   `cast_client.rs:268` заменить явным error-путём (panic=abort на борту).
5. Двойные UI-FFI-поверхности: `presentation.rs` (rock_ui_*) и platform.rs
   (rock_gui_*) — после фазы 3/4 consolidate: presentation остаётся
   единственным местом рендера, GUI-фасад — только для state-апдейтов.
6. `STATE_REVISION_COUNTER` (fetch-add без CAS) — пометить ограничение
   single-task инвариантом в документации модуля (или fetch_add).
7. `rock_source_content_type` возвращает mutable static в Rust
   (`platform.rs:776`) — после фазы 4 отдавать копию в буфер вызывавшего.

### Фаза 6 (опционально, наибольший churn) — закрепление на уровне сборки

1. Перевести модули из `main/` в ESP-IDF-компоненты `components/rock_*`:
   REQUIRES-граф физически запрещает зависимость вниз→вверх
   (`rock_player` не может потребовать `rock_ui`; wiring колбеков — в main).
   Это превращает фазу 3 из договорённости в ошибку сборки.
2. Финальные переименования `git mv` (история сохраняется):
   `relay_bsp.c`→`rock_source.c`, `cast_bsp.c`→`rock_cast_discovery.c`,
   `player_bsp.c`→`rock_player.c`, `rockcast_tokens.h`→`rock_ui_tokens.h`,
   удалить deprecated-псевдонимы `rock_relay_*`.
3. Убрать из корня репозитория bring-up-мусор (`scratch_audio.bin` 21 MB,
   `screen*.png`, `test_cast_run*.log`, `ttl_probe.log`) — в `.gitignore`,
   артефакты — в `telemetry/` по образцу.

### Приоритеты и порядок исполнения

| Фаза | Риск | Ценность | Комментарий |
|------|------|----------|-------------|
| 1 баги+секрет | низкий | высокий | точечные патчи, делать сразу — выполнено 2026-10-10 |
| 2 мёртвый код | очень низкий | высокий | механика, минус флеш/память — выполнено 2026-10-10 |
| 3 инверсия зависимостей | средний | высокий | главное архитектурное улучшение — выполнено 2026-10-10 |
| 4 распил файлов | средний | высокий | по одному PR, каждый собирается |
| 5 кросс-языковая DRY | низкий | средний | ABI-заголовок — первый шаг |
| 6 компоненты/переименования | высокий | средний | только после стабилизации 1–5 |

Фазы 1–2 не меняют поведение (кроме исправляемых багов) и могут пройти за
один-два дня. Фаза 3 — самая важная для будущей скорости разработки; фаза 4
разбивается на 7+ независимых PR. Фаза 6 имеет смысл, только если проект
продолжает расти.
