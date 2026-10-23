# Каст на Chromecast с радио (RE-17, локальный UI) — передача состояния

Дата: 2026-09-17. Документ написан при передаче работы между агентами:
зафиксировано решение владельца, итог разведки (без единой строки кода),
снимок дизайна и промпт продолжения. Этот файл — living-документ задачи;
исполнитель продолжает вести в нём записи о ходе и верификации.

## 1. Решение владельца 2026-09-17 (перестановка приоритетов)

Владелец остановил серверную задачу RS-7 на фазе чтения и переопределил
приоритет: **радио должно выводить звук на Chromecast уже сейчас**, пока
локальный динамик физически не подключён (F13/RE-12). Требования:

- выбор выхода (куда выводить звук) в локальном UI радио, «по примеру
  rockcast»;
- при выборе станции звук играет на выбранный выход;
- голосовое управление и управление с мобильника — другие задачи, НЕ в скопе.

Из этого следует архитектурное следствие, подтверждённое чтением кода:
**локальному касту серверные chromecast.*-команды не нужны**. Радио уже
получает `station.play_stream` с `stream_uri` по санкционированному пути
(`station.play_station` на себя → сервер резолвит → устройство), поэтому
устройство само может отдать этот URL ресиверу. Вся каст-логика (mDNS,
CASTV2) живёт на устройстве; через сервер не проходит ни одна
chromecast.*-команда.

Граница с RC-4/RM-4 (запись центра 2026-09-17: «Cast/relay до RS-7 не
разрешены»): это ограничение касается удалённого управления через сервер
(rockcast desktop, RockMobile), где Cast-команды пришлось бы маршрутизировать
RockServer'ом. Локальный каст с радио эту границу не пересекает. Удалённое
cast-управление (RM-3, повторное объявление media.chromecast у desktop
rockcast) по-прежнему ждёт RS-7.

## 2. Текущее состояние репозиториев (проверено git status)

- **rock-esp32**: рабочий код не тронут, HEAD `8d47cd3` (main). Незакоммиченная
  правка `docs/plan-control.md` — работа центра управления (RC-3/RC-4),
  НЕ трогать и не коммитить её вместе со своими изменениями.
- **rockserver**: RS-7 остановлена на чтении, `src/` и `api/` не менялись,
  коммитов нет. Незакоммиченные docs/mockups в rockserver — параллельная
  работа центра (RC-4), не трогать.
- Каста в прошивке ещё нет: ни Rust-модуля, ни C-слоя, ни UI. Дизайн ниже.

## 3. Что изучено и где это лежит (карта для исполнителя)

### Прошивка (rock-esp32)

- Главный цикл/реконнект: `src/firmware/mod.rs` (`rust_main`).
- Сессионный цикл: `src/firmware/transport.rs` (`session()`, ~строка 170):
  поллинг GUI-событий → `station.play_station` на себя (строки ~229–245),
  ожидание резолва (pending, 30 с), приём `station.play_stream` →
  `platform::player_play_stream(uri, station_id)` (строки ~385–401);
  heartbeat каждые 20 с, ACK-watchdog 45 с (строки ~206–219).
- FFI-граница: `src/firmware/platform.rs` — `GuiEvent` (~строка 342),
  player-функции (~263–340). Паттерн: C предоставляет транспорт/железо,
  Rust владеет протоколом и политикой.
- GUI: `main/rock_gui.h` (enum событий, строка 23), `main/rock_gui.c` —
  views enum (строка 26: NOW_PLAYING/BROWSE/SEARCH), transport-бар
  (строки ~552–662, там скрытая заглушка «Голос» `s_btn_voice` — готовое
  место под Cast-кнопку), бейдж Now-Playing `s_lbl_np_badge` (строка 67),
  оверлеи loading/empty/error. LVGL живёт в своей задаче (esp_lvgl_port) —
  блокировки Rust-цикла UI не убивают.
- Прецедент блокирующих операций в сессионном цикле: `load_catalog`
  (`transport.rs`, ~строка 123) делает блокирующий HTTP с таймаутом ~10 с.
- Публикация состояния: `src/device_control/player.rs`
  `full_state_snapshot` (~строка 332) — playback/volume/display. Сервер
  сегодня игнорирует лишнее поле `output` (в rockserver
  `DeviceRuntimeState` нет `deny_unknown_fields`) — проверено чтением.
- Манифест: `src/device_control/protocol.rs` `registration()` (~строка 61) —
  `media.chromecast` НЕ объявлен. Честность манифеста: capability добавляем
  только после живой приёмки на плате; при изменении манифеста поднимаем
  revision (урок RC-3: сервер отвергает конфликтующие revision).
- C-транспорт: `main/control_platform.c` — esp_wifi через ESP-Hosted 3.0.7
  (`main/idf_component.yml`), WSS через esp_websocket_client (своя задача +
  однослотовая очередь кадров, `rock_ws_receive` строка ~577), HTTP через
  esp_http_client + crt_bundle.
- Сборка: `idf.py --no-ccache build` (PowerShell-профиль IDF 6.1, см. README);
  при OOM собирать `ninja -C build -j 4` (журнал центра, 2026-09-09).
  Перед прошивкой заново детектить COM (был COM6).
- Host-тесты: `tests/device_control_contract.rs` — паттерн: чистые функции
  протокола тестируются на хосте, FFI — нет.

### Эталон rockcast (READ ONLY, `C:\repos\rockcast`) — источник портирования

- Protobuf CastMessage (поля 1–7) + auth challenge: `src/cast/proto.rs` —
  портируется в firmware Rust как есть, без зависимостей.
- Константы: `src/cast/channel/consts.rs` — SENDER_ID `sender-0`,
  RECEIVER_ID `receiver-0`, NS_CONNECTION/HEARTBEAT/RECEIVER/MEDIA/DEVICEAUTH,
  DEFAULT_MEDIA_RECEIVER `CC1AD845`.
- Кадрирование: 4 байта big-endian длины + protobuf (`src/cast/channel/mod.rs`,
  `send`); лимит входного кадра 2 МиБ (`recv.rs`, `read_one`); heartbeat:
  PING каждые 5 с, чужой PING → PONG немедленно (`recv.rs`, `session.rs`).
- TLS: rustls с ОТКЛЮЧЁННОЙ верификацией сертификата
  (`src/cast/channel/tls.rs`, NoCertVerification) — ресиверы Cast используют
  self-signed, это стандарт экосистемы; по каналу не ходят секреты.
- Последовательность подключения: `src/cast/client/mod.rs` — CONNECT →
  (GET_STATUS →) LAUNCH DMR (`CC1AD845`) → LOAD (`streamType: LIVE`,
  autoplay) → ожидание MEDIA_STATUS; best-effort PLAY-nudge после LOAD
  (обход «молчащего» live-потока); budget'ы: GET_STATUS 8 с, LAUNCH 12 с,
  LOAD 15 с. Отключение (`send_stop`): MEDIA STOP → RECEIVER STOP → CLOSE ×2
  → пауза 150 мс до разрыва TLS.
- Discovery: mDNS `_googlecast._tcp.local.` (TXT: `fn`=имя, `md`=модель,
  `id`; `src/cast/discovery/mdns.rs`) + unicast-фоллбек для сломанного
  мультикаста: проба TCP :8009 по /24 + HTTP `:8008/setup/eureka_info`
  (`discovery/{subnet,eureka,lan}.rs`).
- Кэш ресиверов для opaque-идентификаторов: `src/device_control/output.rs` —
  UUID-`receiver_id`, TTL 60 с, ≤32 шт., санитизация имени ≤128 симв.
- Форма cast-состояния: `src/device_control/protocol.rs` (~строка 117) —
  `output: {mode: local|chromecast|relay, receiver_id?}`.

### rockserver — выжимка для будущей RS-7 (чтобы не повторять разведку)

- Замороженные planned-схемы в `api/openapi.yaml`: `ChromecastCommand`
  (~1928: discover без полей; connect c receiver_id 1..128; disconnect без
  полей), `ChromecastCapability` (~1472: actions discover/connect/disconnect,
  discovery_ttl_seconds 5..300), `ChromecastReceiver` + CommandResultOutput
  .receivers (~2047), `OutputRuntimeState` (~1690: mode + receiver_id;
  литеральных полей casting(bool)/receiver_name НЕТ — флаг каста это
  `mode=="chromecast"`). Рантайм: `DeviceRuntimeState`
  (`src/device_control/state.rs`) поля `output` не имеет — состояние с
  `output` молча теряется при парсинге (serde игнорирует неизвестное).
- Остатки RS-6, корневые причины найдены: `log_speech_error` игнорирует
  ошибку (`src/http/voice.rs` ~1245), `SpeechProviderError` — голая строка
  без класса (`src/voice/speech.rs`); тишина → speech_provider_error только
  в буферном v1 (`src/providers/yandex_speechkit.rs`: «audio buffer is
  empty» ~84, «empty transcript» ~112); streaming v3 уже даёт пустые
  updates → speech_not_recognized. Направление фикса: класс ошибки
  auth/network/protocol/no_speech, no_speech → speech_not_recognized.

## 4. Снимок дизайна (решения исполнителя, не реализовано)

1. **Без изменений сервера.** `station.play_station` → `play_stream` как есть;
   chromecast.* не отправляем и не принимаем; `media.chromecast` в манифест
   не добавляем до живой приёмки (потом + bump revision).
2. **Rust владеет протоколом** — новый модуль `src/device_control/cast.rs`
   (protobuf можно отдельным `cast_proto.rs`): порт `proto.rs` из rockcast
   как есть; кэш ресиверов (UUID, TTL 60 с, ≤32, санитизация имён);
   стейт-машина CASTV2 поверх FFI-байтового пайпа. Блокирующие
   последовательности из сессионного цикла с ЖЁСТКИМИ бюджетами
   (тighter rockcast, из-за heartbeat-watchdog 45 с): connect TCP+TLS ≤5 с,
   GET_STATUS 3 с, LAUNCH 8 с, LOAD 10 с. Оверлей GUI «Подключение…» на это
   время (LVGL живёт в своей задаче). В steady-state: pump в каждом цикле
   (~50 мс, recv с таймаутом 0–10 мс), PING каждые 5 с, PONG сразу.
3. **C-слой** `main/cast_bsp.c/.h` (+ строка в `main/CMakeLists.txt`):
   - `rock_cast_discover(timeout_ms, out[], max)` — компонент ESP-IDF
     `espressif/mdns` (добавить в `main/idf_component.yml`,
     `mdns_query_ptr("_googlecast","_tcp",…)`), разбор TXT, выбор IPv4;
   - `rock_cast_tls_open(host, port, timeout_ms)/send/recv(timeout)/close` —
     собственный SSL-контекст mbedtls с `MBEDTLS_SSL_VERIFY_NONE`
     (self-signed ресиверы; НЕ включать глобальный CONFIG_ESP_TLS_INSECURE —
     только этот контекст; обоснование — п. 5).
4. **Интеграция transport.rs**: новые GUI-события CAST_BROWSE/CAST_SELECT/
   CAST_DISCONNECT; PlayStream при активном касте → LOAD на ресивер
   (локальный плеер не стартует), lifecycle-ответы командам — как сегодня;
   Stop → cast STOP; громкость → SET_VOLUME (0..100 → 0.0..1.0);
   disconnect → send_stop → локальный перезапуск удержанного URI;
   смерть cast-сокета → детерминированный возврат на локальный выход +
   публикация состояния. Удерживать последний (station_id, stream_uri)
   только в RAM (не NVS, не логи). Публикация `output {mode, receiver_id?}`
   в `full_state_snapshot` (форма rockcast).
5. **GUI**: Cast-кнопка в transport-баре (место заглушки «Голос»);
   экран/оверлей выбора выхода: «Динамик радио (локальный)» + найденные
   ресиверы + спиннер поиска + пустое состояние с retry; бейдж
   «▶ <имя ресивера>» на Now-Playing; подсветка Cast-кнопки при активном
   касте.
6. **Тесты**: host-тесты proto roundtrip, кэша (границы/истечение/
   opaque-id), снапшота состояния с output, решающей функции роутинга
   выхода; затем `idf.py --no-ccache build` (ninja -j 4 при OOM).

## 5. Риски и открытые вопросы

- **Главный железный риск — mDNS-мультикаст через ESP-Hosted 3.0.7 (C6)**:
  не проверен. Фоллбек (как в rockcast): unicast-проба TCP :8009 по /24 +
  eureka_info :8008 — в v1 НЕ делаем, добавим, если мультикаст на плате
  не заработает (модуль discovery строить так, чтобы фоллбек вставлялся
  сбоку).
- Версия `espressif/mdns`, совместимая с IDF 6.1 — выяснится сборкой.
- Однослотовая очередь WS-кадров при длинных cast-операциях — существующий
  класс риска (как load_catalog); бюджеты п. 4 держат heartbeat под
  watchdog 45 с.
- Безопасность VERIFY_NONE: по cast-каналу не ходят секреты (только URL
  потока и заголовок); OTA и серверный TLS не затронуты (свой контекст).
  Задокументировать в коде и в отчёте центру.
- `media.chromecast` в манифест — только после живой приёмки; вместе с
  bump revision (урок RC-3).

## 6. Приёмка (уменьшенный скоп владельца)

- Cast-кнопка на Now-Playing открывает выбор выхода; mDNS находит ресиверы;
  выбор подключает; выбранная станция играет на ресивере, локальный выход
  молчит;
- бейдж «играет на <имя>»; громкость ± действует на ресивер; disconnect
  детерминированно возвращает локальное воспроизведение;
- нет ресиверов, недоступный ресивер, обрыв связи посреди каста —
  ограниченные детерминированные состояния с внятным UI;
- stream URI не появляется в логах/NVS; host-тесты и сборка зелёные;
  живая приёмка владельцем на плате (COM детектить заново);
- после приёмки: media.chromecast в манифест + записи в docs + отчёт центру.

## 7. Реализация RE-17 и верификация (2026-09-17)

### Выполненные компоненты
1. **Rust / device_control**:
   - `src/device_control/cast_proto.rs` [NEW]: чистая реализация Protobuf proto2 для `CastMessage` и `DeviceAuthMessage` без внешних зависимостей. Кадрирование: 4-байтный big-endian префикс длины (`encode_framed`).
   - `src/device_control/cast.rs` [NEW]: константы экосистемы CASTV2 (`CC1AD845`, `NS_*`), кэш `ReceiverCache` (TTL 60 с, лимит 32 записи, стабильные синтетические ID `rc-XXXX`, санитайзер имени), конструкторы сообщений (`msg_connect`, `msg_launch_dmr`, `msg_load_stream`, `msg_stop_media`, `msg_set_volume`, `msg_ping`, `msg_pong`), парсер сессии DMR `parse_dmr` и классификатор `classify_media_status`.
   - `src/device_control/player.rs` [MODIFIED]: расширение `full_state_snapshot_with_output` для публикации `"output": {"mode": "local"|"chromecast", "receiver_id": ...}` без ломания базового `full_state_snapshot`.
   - `src/device_control/mod.rs` [MODIFIED]: экспортированы модули `cast` и `cast_proto`.
2. **C / ESP-IDF BSP**:
   - `main/idf_component.yml` [MODIFIED]: добавлена зависимость `espressif/mdns: "^1.4"`.
   - `main/CMakeLists.txt` [MODIFIED]: добавлены `cast_bsp.c` в `SRCS` и `mdns`, `esp-tls`, `mbedtls` в `PRIV_REQUIRES`.
   - `main/cast_bsp.h` [NEW] & `main/cast_bsp.c` [NEW]:
     - `rock_cast_discover`: mDNS-сканер сервисов `_googlecast._tcp`, извлекающий IPv4, порт 8009 и TXT-поля (`fn`, `md`, `id`).
     - `rock_cast_tls_open/send/recv/close`: клиент на базе `esp_tls` с `skip_common_name = true` (самоподписанные сертификаты Chromecast на порту 8009). Глобальная настройка `CONFIG_ESP_TLS_INSECURE` не затрагивается.
3. **GUI (LVGL v9)**:
   - `main/rock_gui.h` [MODIFIED] & `main/rock_gui.c` [MODIFIED]:
     - Скрытая заглушка голоса в transport-баре заменена на активную кнопку `s_btn_cast` ("Cast").
     - Создан модальный оверлей выбора выхода (локальный динамик + список ресиверов + статус сканирования + кнопка обновления + закрытие).
     - При активном касте кнопка подсвечивается акцентным цветом, а бейдж трека обновляется на `"▶ <имя ресивера>"`.
     - События выбора/отключения передаются через очередь в Rust (`ROCK_UI_EVT_CAST_*`).
4. **Rust Firmware / Transport**:
   - `src/firmware/platform.rs` [MODIFIED]: FFI-декларации и безопасные обёртки для `rock_cast_discover`, `CastTlsStream`, `gui_show_cast_menu`, `gui_set_cast_devices`, `gui_update_cast_status`, обработка событий `GuiEvent::CastBrowse`, `GuiEvent::CastSelect`, `GuiEvent::CastDisconnect`.
   - `src/firmware/cast_client.rs` [NEW]: высокоуровневый клиент Chromecast (управление сессией TLS, запуск DMR, загрузка потока, периодический PING каждые 5 с / ответ на PONG, управление громкостью, детерминированный stop/disconnect).
   - `src/firmware/transport.rs` [MODIFIED]:
     - Обработка `CastBrowse` (mDNS поиск и обновление GUI), `CastSelect` (подключение к ресиверу и переключение текущего потока), `CastDisconnect` (возврат на локальный плеер).
     - Маршрутизация `PlayStream`, `PlaybackAction`, `VolumeAction` на Chromecast при активном касте.
     - Восстановление при разрыве связи с Chromecast (автоматический возврат на локальный вывод).
     - `last_stream` хранится исключительно в оперативной памяти (RAM) и никогда не пишется в NVS или логи.
   - `src/firmware/mod.rs` [MODIFIED]: зарегистрирован `mod cast_client;`, покрыты события Cast в фоллбек-цикле.

### Верификация
1. **Host Contract & Unit Tests**:
   - `tests/device_control_contract.rs`: добавлены тесты `re17_cast_proto_roundtrip_and_framing`, `re17_receiver_cache_ttl_and_sanitization`, `re17_full_state_snapshot_with_output_conforms_to_contract`, `re17_protocol_message_constructors_and_responses`.
   - Результат `cargo test --target x86_64-pc-windows-msvc`:
     - 12/12 lib unit-тестов успешно пройдены.
     - 21/21 contract-тестов успешно пройдены.
2. **ESP-IDF Firmware Build**:
   - Сборка целевого образа `idf.py build` (ESP32-P4 v1.3 + RISC-V `riscv32imafc-esp-espidf`):
     - Образ `build/rock_esp32.bin` (размер 0x2572c0 байт, 71% свободно в партиции приложения) успешно создан.
     - Все предупреждения компилятора устранены.
3. **Правило честности манифеста**:
   - `media.chromecast` в манифест регистрации (`src/device_control/protocol.rs`) пока НЕ добавлялся — добавление запланировано после живой приёмки на стенде с железом.
   - Репозитории `rockserver` и `rockcast` не изменялись. Файл `docs/plan-control.md` сохранён без изменений.

---

## 8. Инструкция для прошивки и живой приёмки на плате

1. Подключить плату Rock ESP32 (JC4880P443C_I_W, порт USB2 Native USB Serial/JTAG).
2. Определить текущий COM-порт (в диспетчере устройств или через `idf.py -p <PORT> flash`).
3. Запустить прошивку:
   ```powershell
   powershell -ExecutionPolicy Bypass -Command "& { `$env:PYTHONUTF8='1'; . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'; idf.py -p <PORT> flash monitor }"
   ```
4. Проверить по логам и экрану:
   - В UI нажать кнопку **Cast** в панели управления.
   - Убедиться, что mDNS-поиск находит ресиверы в локальной сети.
   - Выбрать ресивер из списка. Проверить, что бейдж переключился на `▶ <имя ресивера>`.
   - Выбрать станцию в каталоге: убедиться, что звук играет через Chromecast, а встроенный динамик молчит.
   - Регулировка громкости: ползунок громкости на радио меняет громкость на Chromecast.
   - Отключение Cast: возврат на локальный вывод.

---

## 9. Сбой релея на «Радио Ваня Туапсе» и его расследование (2026-09-18)

### 9.1 Воспроизведённый сбой и корневые причины ПО (исправлены)

Первая воспроизведённая ошибка: `rock_relay: Relay prebuffer timed out at
0 PCM bytes` **в ту же миллисекунду**, что и ответ апстрима — цикл выхода
не по таймауту, а по несброшенному `s_relay_stop` (флаг оставался `true`
после любого прошлого останова релея; `rock_relay_start` сбрасывал его
только ПОСЛЕ префилла). Далее последовательно вскрылись и устранены
(всё — сравнением с эталоном rockcast, READ ONLY→патч десктопа разрешён
владельцем только для `limit`):

| # | Дефект | Правка |
|---|--------|--------|
| 1 | Stale `s_relay_stop` — любой рестарт релея после первого стопа мгновенно фейлит префилл | Сброс флага в начале `rock_relay_start` |
| 2 | Преролл декодировался из соединения #1, затем соединение закрывалось и открывалось заново — пауза в секунды при запасе 1 c → underrun | Одно непрерывное upstream-соединение: префилл и стриминг из одного хендла |
| 3 | Нет реалтайм-пейсинга: decode-and-burst заливал CC, backpressure останавливал чтение апстрима | `pace_before_push` по медиачасу (эквивалент `fanout::pace_pcm_chunk`) |
| 4 | Нет PLAY-nudge для «молчащего» live-потока | `msg_play` + nudge через 2 c тишины после LOAD |
| 5 | Один клиент последовательно; второе соединение CC гибло от 2-сек таймаута чтения запроса | Fanout: кольцо 2 МиБ + задача на клиента, join `end−144 КБ`, clamp при переполнении, таймаут запроса 12 c |
| 6 | LOAD только `audio/wav` без ретрая | Лестница кандидатов как `candidate_content_types` (реляй отдаёт свой contentType первым) |
| 7 | 1-КБ куски send(): сотни syscall/с душили SDIO/C6 | Целыми кусками (MSS-сегментация lwip) |
| 8 | Порт 8080 + `TCP_SND_BUF=5760` (потолок ~28 КБ/с при RTT 200 мс) | Порт 47809, `TCP_SND_BUF_DEFAULT=57480`, `TCP_WND_DEFAULT=32400` |

Дополнительно реализован **passthrough-транспорт** (эквивалент
`RelayTransport::Passthrough`): MP3 отдаётся ресиверу как `audio/mpeg`
chunked без декодирования (32 КБ/с вместо 192 КБ/с WAV) — выбран автоматически
по content-type апстрима; AAC/неизвестные — как раньше, WAV-транскод.
`rock_relay_start` теперь возвращает URL **и** content type для LOAD.

### 9.2 Эталонная телеметрия (десктоп играет, плата — нет)

Запущен rockcast desktop (после патча `limit: 50→20` под новый контракт
RockServer `POST /api/v1/search`; изменение в rockcast/src/rockserver.rs,
не закоммичено) с той же станцией на том же JBL Bar 9.1:
BUFFERING→PLAYING за ~0.5 c; wire-захват (PC↔JBL, HTTP-релей открытый):
CC в первые 200 мс принимает 167 КБ, далее ровно 192 КБ/с, rwin CC
постоянно 3649 B. Заголовки ответа платы байт-в-байт совпадают с эталоном.

### 9.3 Доказанный блокер: радиолинк платы

- Во время стриминга плата теряет **87–100% ICMP** (ping PC→плата);
  при префилле латентность 300–800 мс; в простое ~47 мс.
- PC→плата TCP: SYN-ретрансмиты ×5, хендшейк редко, дата теряется;
  A/B TCP timestamps — не влияет; порт — не влияет.
- RSSI платы −75…−78 dBm на канале 6 при 10–13 соседних AP
  (docs/stability-notes.md, RF survey RE-16) — низкий PHY-rate, TX платы
  монополизирует эфир, даунлинк к плате коллапсирует, ACK'и гибнут →
  zero-window deadlock.
- Даже passthrough MP3 (32 КБ/с) CC вычитывает на ~2 КБ/с.
- `esp_wifi_set_ps(WIFI_PS_NONE)` подтверждён readback'ом (ps=0) — не сон.
- C6-прошивка и host-библиотека совпадают (3.0.7=3.0.7), SDIO-ошибок нет.

**Вывод: ПО релея эквивалентно эталону; остаточный блокер — качество
радиолинка платы (RSSI −77).** Следующий шаг — физический: переместить
плату/роутер или улучшить антенну; затем повторить `test_cast_hw.py`
(телеметрия и полный разбор — `telemetry/RE17-relay-20260918/README.md`).

### 9.4 Верификация после правок (2026-09-18)

- Host-тесты: 12/12 lib + 21/21 contract зелёные
  (`cargo test --target x86_64-pc-windows-msvc`).
- Сборка: `idf.py --no-ccache build` — Project build complete.
- `test_cast_hw.py` обновлён под оба транспорта
  (`content-type: audio/(wav|mpeg)`).
- Живая приёмка на стенде BLOCKED по RF (см. 9.3) — до перемещения платы.

---

## 10. Статус работ после доработки антенны владельцем (2026-10-10)

### 10.1 Вводные
1. Владелец подключил внешнюю антенну к модулю: аппаратный радиолинк восстановлен, встроенный динамик воспроизводит звук чисто и стабильно.
2. Проблема пользователя: при переключении на Chromecast звук обрывался, на экране появлялось `RockServer: OFF` и «ошибка воспроизведения потока», а также наблюдались артефакты в GUI («бред на экране»).

### 10.2 Диагностированные дефекты и внесённые исправления
1. **Шрифт и артефакты в GUI (`main/rock_gui.c`):**
   - Бейдж вывода Cast использовал строку `"▶ %s"` с глифом `▶` (U+25B6). Растровый шрифт `font_inter_14` не содержит этого символа, что приводило к сбою рендера строки.
   - Заменено на встроенный глиф FontAwesome `ROCK_ICON_PLAY` (`\xEF\x81\x8B`), корректно отображающийся в бейдже.
   - В `rock_gui_update_playback()` предотвращено затирание названия станции и метаданных при получении пустых строк статуса.

2. **Зависания SDIO RPC и падение WSS (`main/control_platform.c`, `src/firmware/platform.rs`):**
   - Функция `rock_wifi_rssi()` вызывала `esp_wifi_sta_get_ap_info(&ap_info)` каждые 10 с через SDIO RPC к ESP32-C6. В момент одновременного скачивания потока и раздачи релея C6 блокировался на 5000 мс (`eh_host_feat_rpc: request: no response uid=16 msg_id=294 (5000 ms)`). Из-за этого задерживались ответы WSS Heartbeat ACK, превышался 45-с таймаут, и плата отображала `RockServer: OFF`.
   - В `control_platform.c` добавлена `rock_wifi_is_connected()` на базе прямых битов FreeRTOS (без RPC).
   - В `rock_wifi_rssi()` введено кэширование: при активном релее (`rock_relay_is_active() == true`) SDIO RPC к C6 полностью запрещён, возвращается кэшированное значение RSSI.
   - В `src/firmware/transport.rs` таймаут ожидания Heartbeat ACK увеличен с 45 с до 75 с, и перед переключением на Cast принудительно отправляется свежий heartbeat.

3. **Оптимизация релея (`main/relay_bsp.c`):**
   - Подушка входа (`RELAY_JOIN_CUSHION_BYTES`): разделена на 144 КБ для WAV и 16 КБ для MP3 passthrough (ранее 144 КБ MP3 создавали залповый флуд очереди C6 и сокетное противодавление `errno 11`).
   - Убран `TCP_NODELAY` на сокете раздачи аудио, чтобы lwIP выполнял нормальную сегментацию пакетов по MSS.
   - Задачи `relay_task` и `relay_client_task` закреплены за Core 1 с выделением стеков из внутренней SRAM.
   - `RELAY_PASSTHROUGH_PREFILL_BYTES` уменьшен до 4 КБ.

### 10.3 Результаты и выявленная внешняя проблема апстрима
- После устранения зависаний RPC плата продемонстрировала непрерывный каст:
  `rock_relay: Relay streamed 73644 bytes to Chromecast` ... `368220 bytes to Chromecast` без дропов декодера (`underruns=0 decode_errors=0`), с запасом внутренней памяти >115 КБ и быстрыми ответами WSS (140 мс).
- Однако при тестах с текущей станцией по умолчанию `181-fm-classic-buzz` соединение с её сервером `listen.181fm.com` крайне нестабильно из сети пользователя:
  - Пинг/RTT до `216.235.84.2` составляет 1300+ мс.
  - TLS-хэндшейк с `listen.181fm.com` длится 5–10 секунд, а периодически зависает с `TimeoutError` даже при прямом обращении с ПК под управлением Windows.
  - В моменты таких сетевых просадок `listen.181fm.com` успевает вернуть HTTP 200, но затем закрывает соединение (`esp_transport_read returned:0 and errno:11`), не передав начальные аудиоданные, что приводит к таймауту префилла в `relay_prefill_ring`.
- В `relay_bsp.c` настроен таймаут подключения 25 с, ретраи в `upstream_connect()`, а сокетный таймаут чтения переводится на 5 с через `esp_http_client_set_timeout_ms()` после успешного приёма заголовков.

### 10.4 План и рекомендации для следующего агента
1. **Проверить стриминг на альтернативных станциях из каталога:**
   Тестировать не только `181-fm-classic-buzz` (сервер в США с RTT >1.3с), но и более стабильные станции из `c:\repos\rockserver\catalog\rockcatalog\stations.v1.json`, например:
   - `somafm-metal-detector` (`ice6.somafm.com`)
   - `rock-antenne-heavy-metal` (`stream.rockantenne.de`)
   - `radio-caprice-heavy-metal` (`79.111.14.76`)
2. **Либо использовать локальный прокси/серверный поток:**
   В `play_local_station` динамик играет поток через RockServer (`/api/v1/stations/{id}/stream`), тогда как Cast пытается напрямую связаться с исходным `stream_uri`. Если апстрим блокируется географически или имеет высокий RTT, релей может брать поток через RockServer или локальный тестовый HTTP-стрим.
3. **Запустить и верифицировать:**
   `python test_cast_hw.py` с выбранной станцией, убедиться в воспроизведении звука на Chromecast и проверить `screen.png`.

---

## 11. Единый источник аудио: архитектура и верификация (2026-10-10)

### 11.1 Решение владельца: единый сервис получения потока

Владелец отметил архитектурный дефект: в прошивке было два независимых
тракта выборки — `player_bsp.c` (динамик) и `relay_bsp.c` (Cast), каждый
со своим HTTP-клиентом, ICY-парсером и буферами. Динамик играл через
прокси RockServer, а Cast-релей лез на исходный апстрим напрямую — именно
поэтому проблема была «исключительно в Хромкасте». По решению владельца
реализован **единый источник** (вариант «Единый источник сейчас»):

```
RockServer-прокси ──(1 авторизованный коннект, Bearer)──▶ Сервис-источник
                                                            (кольцо 2 МиБ, фидер)
                                           ┌────────────────┴────────────────┐
                                  Потребитель «Динамик»            Потребитель «Cast»
                                  (rock_source_read →              (TCP :47809 → Chromecast,
                                   декодер → PCM → I2S)             MP3 passthrough / WAV)
```

Переключение выхода = смена потребителя; коннект к RockServer и кольцо
переживают переключение (нет переподключения, нет разрыва звука).

### 11.2 Реализация

1. **`main/relay_bsp.c` / `main/relay_bsp.h`** — сервис-источник:
   - `rock_source_start(uri, bearer)` — коннект к прокси с `Authorization:
     Bearer` (ретраи, ICY-фильтр как раньше), выбор транспорта
     (MP3 → passthrough / AAC → 48 кГц stereo PCM в кольце), префилл, фидер;
   - `rock_source_stop()` — полная остановка (фидер + клиенты);
   - `rock_source_read(out, cap)` — тап динамика: подключение у живого края
     (`end − cushion`), clamp при отставании;
   - `rock_relay_listen(out_url)` — подключение Cast-потребителя (TCP-сервер
     :47809), идемпотентно;
   - кольцо **персистентно**: выделяется один раз, при рестартах источника
     сбрасывается под мьютексом — потребитель не может коснуться освобождённой
     памяти;
   - `rock_relay_stop`/`rock_relay_is_active` — алиасы (RSSI-кэш в
     `control_platform.c` продолжает работать: при любом стриминге SDIO-RPC
     к C6 подавляется).
2. **`main/player_bsp.c`** — `stream_rx_task` больше не имеет своего HTTP:
   - passthrough-режим: читает сжатый поток из источника, декодирует
     локально (как раньше), динамическое определение формата сохранено;
   - PCM-режим (AAC): берёт из кольца готовые 48 кГц stereo PCM напрямую,
     без декодера;
   - HTTP-клиент, редиректы, SSRF-проверки и реконнекты из задачи убраны
     (ими владеет источник); SSRF-гейт на входе `rock_player_play_stream`
     сохранён.
3. **`src/firmware/platform.rs`** — FFI источника (`source_start/stop/
   is_active/content_type/update_bearer`, `relay_listen`).
4. **`src/firmware/transport.rs`** — жизненным циклом источника владеет Rust:
   - `ensure_source(station, token)`: идемпотентен по станции — если источник
     уже играет эту станцию, он НЕ перезапускается (быстрый путь, обновляется
     только bearer);
   - `play_local_station` = ensure_source + подключение динамика;
   - `cast_play_stream` = отключение тапа динамика + ensure_source +
     `relay_listen` + LOAD на ресивер;
   - CastDisconnect и смерть cast-сокета: динамик возвращается на ТОМ ЖЕ
     источнике — без переподключения к серверу;
   - полный STOP (команда/GUI) останавливает и источник;
   - периодическое обновление bearer (10 мин) продлевает и источник.
5. **`test_cast_hw.py`** — станция по умолчанию `somafm-metal-detector`
   (MP3 passthrough, стабильный апстрим; `listen.181fm.com` из этой сети
   имел RTT >1.3 c и обрывы TCP). Маркеры STEP 1 обновлены под единый
   источник (`Upstream HTTP status 200 OK`, `Pre-roll reached`).

Сырой `stream_uri` больше вообще не используется для воспроизведения:
платка ходит только на прокси RockServer. URI и bearer по-прежнему не
логируются и не пишутся в NVS (host-тест
`stream_uri_never_leaks_into_logs_or_state_or_errors` зелёный).

### 11.3 Найденный и исправленный сбой: паника при остановке источника

Симптом (воспроизведён владельцем): «звук был, но потом переключилось на
динамик и ребутнуло». Лог (`test_cast_run2.log`):

```
rock_relay: Stopping stream source...
rock_relay: Relay task exited
assert failed: prvTaskDeleteWithCaps idf_additions.c:133 (xResult == pdPASS)
rst:0xc (SW_CPU_RESET)
```

Корневая причина (латентный, существовал с первого дня релея): фидер
создавался обычным `xTaskCreatePinnedToCore`, а завершался
`vTaskDeleteWithCaps(NULL)`. В IDF 6.1 `prvTaskDeleteWithCaps` вызывает
`xTaskGetStaticBuffers` и ассертит на задачах, созданных не через
WithCaps-API. Путь самоудаления фидера раньше ни разу не выполнялся на
железе (реляй никогда не останавливали чисто). Фикс: парный
`vTaskDelete(NULL)` (`main/relay_bsp.c`, комментарий в коде).

### 11.4 Верификация на стенде (2026-10-10, прошивка 12:44/13:5x, COM6)

1. `cargo test --target x86_64-pc-windows-msvc`: 12/12 lib + 21/21
   contract — зелёные (включая тест невытекания stream_uri).
2. `idf.py build` — Project build complete (0x25ea50, 70% партиции свободно),
   прошит `idf.py -p COM6 app-flash`.
3. `python test_cast_hw.py` — **все 6 шагов PASS** (`test_cast_run3.log`):
   - STEP 1: `somafm-metal-detector` на динамике через единый источник
     (Upstream 200 → Pre-roll 524288 байт);
   - STEP 2-3: mDNS нашёл ресивер, TLS подключён;
   - STEP 4: LOAD → релей → `Cast LOAD ready!` → непрерывный стриминг,
     **подтверждено >300 КБ** (фактически ~975 КБ за прогон, без дропов);
   - STEP 5-6: метрики и скриншот. Полный лог прогона — локальный артефакт
     `test_cast_run3.log` (в git не входит, `*.log` игнорируется).
4. Регрессия ребута (бывший crash-путь), отдельная(serial)-проверка:
   - `CMD:UI:CAST:LOCAL` при активном касте (868 КБ отправлено ресиверу):
     динамик возобновился (**Pre-roll через 290 мс**, источник НЕ
     перезапускался — «Pre-roll reached» без «Stopping stream source»),
     Chromecast закрыл сокет (errno 104 — ожидаемо после STOP);
   - `CMD:UI:STOP`: «Stopping stream source… → Relay task exited →
     Stream source stopped», **без паники/ребута**;
   - живость после стопа: `CMD:METRICS` отвечает, `underruns=0
     decode_errors=0`, RSSI −63 dBm, внутренняя память 129 КБ свободно.
5. Скриншот во время каста (`docs/cast-verify-active.png`): артефактов
   рендера нет, кириллица и глифы корректны, бейдж ресивера
   («▶ Главная спальня») отображается на Now-Playing, спектрограмма живая.

### 11.5 Поведение при «умершем» ресивере (standby)

После часов тестов ресивер (JBL «Главная спальня») ушёл в standby: TLS-сокет
жив, DMR не отвечает на LOAD. Наблюдаемое поведение прошивки: каждая попытка
воспроизведения гоняет ограниченную лестницу content-type (LOAD 15 с ×
кандидаты), затем детерминированный отказ («Cast play_stream failed»),
плата жива, паник/ребутов нет (uptime непрерывен, проверено до 210 с).
`CMD:UI:CAST:LOCAL` корректно разрывает такую сессию и возвращает локальный
вывод. Замечание для будущей работы: лестница LOAD выполняется блокирующе
в сессионном цикле (до ~30 с на попытку) — GUI-события и heartbeat в это
время ждут; при необходимости сократить бюджиты/вынести в отдельную задачу.

Между сессиями тестирования наблюдался один ребут платы без захваченного
лога (вероятно, ручной сброс владельцем; до этого STOP-путь был проверен
чистым, после — плата работала стабильно).

### 11.6 Известные остаточные пункты (вне скопа этой задачи)

- Косметика GUI при переключениях выхода (транзиентные заголовок/бейдж
  в Now-Playing в момент «Starting…», устаревший бейдж после возврата на
  динамик) — владелец определил как отдельную задачу; GUI-код в этой
  работе не менялся.
- `media.chromecast` в манифест регистрации по-прежнему НЕ добавлен —
  только после живой приёмки владельцем (затем bump revision).
- Репозитории rockserver/rockcast не изменялись; сырые stream URI не
  логировались ни в логах платы, ни в артефактах тестов.

## 12. Исправление переключения станций на Chromecast (2026-10-23)

### Проблема
При переключении радиостанций во время активной трансляции на Chromecast (JBL Bar 9.1):
- На экране спектроанализатор и источник начинали работать на новой станции, но на саундбаре звук пропадал.
- Воспроизведение восстанавливалось только после ручного нажатия «Стоп» и затем «Старт».

### Причина
1. В `src/firmware/transport/media.rs`: при смене станции вызывался `ensure_source`, который обрывал HTTP-соединение реле. Но активная медиасессия Chromecast (`mediaSessionId`) не останавливалась (`stop_media` не вызывался). Приемник оставался в подвешенном состоянии, ожидая закрытый TCP-сокет старой сессии.
2. Идентичный URL потока: реле всегда отдавало `http://<ip>:47809/stream`. Для Chromecast новый `LOAD` с тем же URL часто воспринимался как повторный запрос или игнорировался плеером.
3. В `src/firmware/cast_client.rs`: в `wait_dmr_session` отсутствовало разделение между проверкой уже запущенного DMR (`query_dmr`) и ожиданием запуска нового DMR (`wait_launch_dmr`), а также не проверялся `requestId` при обработке ошибок запуска.

### Решение
1. В начале `cast_play_stream` добавлен обязательный вызов `cast_client.stop_media()` для гарантированного корректного завершения предыдущей медиасессии перед сменой источника.
2. В URL реле добавлен query-параметр станции (`?st={station_id}`), обеспечивающий уникальный `contentId` для Cast-приемника.
3. В `cast_client.rs` логика разделена на `query_dmr` и `wait_launch_dmr` с валидацией `requestId`.

### Верификация на оборудовании
Проведено сквозное тестирование переключений на плате `ESP32-P4`:
- Станция 0 (Russian Rock, MP3 passthrough) -> `PLAYING`, передано >1.9 МБ.
- Переключение на Станцию 1 (ГУСЬ.Рок, AAC -> WAV PCM) -> предыдущая сессия остановлена (`CANCELLED`), запущена новая сессия, `PLAYING`, передано >2.78 МБ.
- Переключение обратно на Станцию 0 -> сессия остановлена, `PLAYING`, передано >208 КБ.
Все переключения происходят автоматически и без задержек. Память: `int_free ≈ 210 КиБ`, `dma_largest = 57 344 байт`.


