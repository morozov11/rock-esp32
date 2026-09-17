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

## 7. Промпт для следующего агента

См. раздел «Промпт-передача» в конце файла — копировать целиком.

---

## Промпт-передача

Ты исполнитель задачи «Каст на Chromecast с радио» в репозитории
C:\repos\rock-esp32 (прошивка RockCast-радио, ESP32-P4 + C6). Владелец
изменил приоритет 2026-09-17: радио должно выводить звук на Chromecast
уже сейчас (динамик не подключён), выбор выхода — в локальном UI «по
примеру rockcast», станция играет на выбранный выход. Голос и управление
с мобильника — НЕ в скопе. Серверные chromecast.*-команды не нужны и не
используются: радио продолжает получать stream_uri по существующему пути
station.play_station → server-resolved station.play_stream.

Сначала прочитай (порядок важен):
1. docs/cast-progress.md — этот файл: карта кода, дизайн-решения, риски,
   приёмка. Всё разведано, кода ещё не написано.
2. docs/plan-control.md — контекст (доска RS-7/RE-17/RM-3, запись
   владельца 2026-09-16 о шаге 7, записи центра 2026-09-17 RC-3/RC-4).
   Статусы на доске НЕ менять.
3. docs/rockcast-device-plan.md — шаг 7 (п. 2–5 описывают именно эту
   работу; п. 1 про сервер RS-7 — отложен).
4. Эталон (ТОЛЬКО чтение): C:\repos\rockcast\src\cast\* (proto.rs,
   channel/*, client/*, discovery/*) и src/device_control/output.rs —
   портируй по образцу.

Сделай:
1. Rust: src/device_control/cast.rs (+cast_proto.rs) — порт protobuf
   CastMessage, кэш ресиверов (UUID, TTL 60 с, ≤32), стейт-машина CASTV2
   (CONNECT → LAUNCH DMR CC1AD845 → LOAD LIVE → MEDIA_STATUS; heartbeat
   PING 5 с/PONG; send_stop-последовательность) поверх FFI-пайпа. Бюджеты:
   connect ≤5 с, GET_STATUS 3 с, LAUNCH 8 с, LOAD 10 с (heartbeat-watchdog
   сервера 45 с). Последний (station_id, stream_uri) держать только в RAM.
2. C: main/cast_bsp.c/.h — rock_cast_discover через espressif/mdns
   (добавить в main/idf_component.yml), rock_cast_tls_open/send/recv/close
   на mbedtls с MBEDTLS_SSL_VERIFY_NONE (свой контекст, глобальный
   CONFIG_ESP_TLS_INSECURE не включать; обоснование в комментарии).
   Подключить в main/CMakeLists.txt.
3. Интеграция src/firmware/transport.rs + platform.rs: события
   CAST_BROWSE/CAST_SELECT/CAST_DISCONNECT; PlayStream при касте → LOAD на
   ресивер; Stop/Volume → cast STOP/SET_VOLUME; disconnect → локальный
   перезапуск; смерть сокета → детерминированный возврат; публикация
   output {mode, receiver_id?} в full_state_snapshot. Манифест НЕ менять
   (media.chromecast — только после живой приёмки, затем bump revision).
4. GUI (main/rock_gui.c/.h): Cast-кнопка (место скрытой заглушки «Голос»),
   экран выбора выхода (локальный + ресиверы + поиск + пустое состояние),
   бейдж «играет на <имя>», подсветка кнопки при касте.
5. Тесты: host-тесты по паттерну tests/device_control_contract.rs (proto
   roundtrip, кэш, снапшот с output, роутинг выхода); сборка
   idf.py --no-ccache build зелёная (при OOM — ninja -C build -j 4).
   Прошивка и живая приёмка — с владельцем (COM-порт детектить заново,
   был COM6).

Ограничения: не трогать голос, сервер (rockserver), rockcast; не
отправлять/не принимать chromecast.* команды; stream URI — никогда в
логах/NVS/коммитах; незакоммиченные правки docs/plan-control.md в
rock-esp32 и docs в rockserver — чужая работа центра, не коммитить их;
статусы доски не менять; секретов нет.

Завершение: коммит в rock-esp32 (только свои файлы), записи в
docs/cast-progress.md (что сделано/проверено/осталось), отчёт владельцу
и центру управления: что работает, что проверено на хосте/плате,
открытые вопросы (главный — прошёл ли mDNS-мультикаст через C6; если нет
— нужен ли unicast-фоллбек по образцу rockcast).
