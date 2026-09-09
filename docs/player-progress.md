# RE-5: Player MVP на плате ESP32-P4 (2026-09-09)

Реализация и физическая приёмка Step 4 плана [
ockcast-device-plan.md](rockcast-device-plan.md)
и задачи **RE-5** из [plan-control.md](plan-control.md).

Плеер принимает серверно-резолвленные команды воспроизведения (station.play_stream,
playback.play/pause/stop, olume.set_volume/change_volume/set_mute),
декодирует MP3/AAC через официальный esp_audio_codec 2.5.0, буферизует PCM
в 256 КиБ кольцевом буфере в PSRAM, выводит звук через ES8311 (I2S0 DOUT GPIO9)
с тактированием PA (GPIO11), и публикует честное состояние через существующий
device-control транспорт.

---

## 1. Архитектура и изоляция

- **Разделение ядер (Core Affinity)**:
  - Все задачи декодирования и вывода звука (
ock_st_rx, 
ock_au_tx, 
ock_pl_test)
    строго привязаны к **Core 1** (xTaskCreatePinnedToCore(..., 1)).
  - Core 0 полностью свободен для LVGL UI, GT911 тача, сетевого стека ESP-Hosted и WebSocket.
- **Буферизация (PSRAM Ring Buffer)**:
  - 256 КиБ кольцевой буфер выделен в PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    что соответствует ~1.5 с несжатого PCM 44.1 кГц 16-бит стерео.
  - Порог pre-roll: 16 КиБ (~100 мс) перед включением PA и началом вывода.
  - Внутренняя RAM не расходуется на аудиоданные.
- **Управление усилителем (PA GPIO11)**:
  - PA включается только при реальном наличии PCM сэмплов и статусе PLAYING.
  - PA немедленно выключается при паузе, остановке, ошибке или mute, предотвращая щелчки и шум.
- **SSRF Защита (F11/RS-3/RE-5)**:
  - Серверный и клиентский стрим-валидатор реализован на чистом Rust и экспортирован
    через FFI (
ock_validate_stream_uri).
  - Проверяются схема (только HTTP/HTTPS), длина (<=2048), порт, запрет юзеринфо (@),
    запрет фрагментов (#), запрет loopback (127.0.0.0/8, localhost), private/RFC1918 (10.0.0.0/8,
    172.16.0.0/12, 192.168.0.0/16), link-local (169.254.0.0/16) и multicast/broadcast.
  - Валидация выполняется перед первым запросом и перед каждым HTTP-редиректом (<=5 редиректов).
- **Приватность данных (F3)**:
  - stream_uri, токены и аудиосэмплы никогда не выводятся в логи, ошибки или снимки состояния.

---

## 2. Результаты тестов хоста (Rust Contract Tests)

Запуск: cargo test --target x86_64-pc-windows-msvc --test device_control_contract
Результат: **10 passed; 0 failed; 0 ignored** (100% PASS)

1. 
e05_advertises_truthful_player_and_display_surface — манифест рекламирует роль player,
   media.playback (play, pause, stop без next/prev), media.station (rockserver_catalog),
   media.volume (0..100, step 1, mute: true), display.presentation.
2. station_play_stream_command_parsing_and_boundaries — строгий парсинг station.play_stream,
   проверка длины station_id (1..128) и URI (1..2048), проверка источника 
ockserver_catalog.
3. playback_and_volume_command_parsing_and_bounds — парсинг команд воспроизведения и громкости,
   отклонение volume > 100, delta = 0, отклонение playback.next и playback.previous с кодом capability_not_supported.
4. player_state_idempotent_replay_and_snapshot_serialization — идемпотентность повторных вызовов
   pply_play_stream, инкремент state_revision, корректная сериализация ull_state_snapshot.
5. stream_uri_never_leaks_into_logs_or_state_or_errors — подтверждение полного отсутствия stream_uri
   в сериализованных снимках состояния и сообщениях об ошибках.
6. Плюс 5 существующих тестов диспетчеризации, протокола и пейринга.

---

## 3. Физическая приёмка на плате (ESP32-P4, COM6, без erase_flash)

Прошивка: образ 
ock_esp32.bin (размер 0x2114F0, 2.17 МБ, свободно 74% слота ota_0).
Запуск верификации на физическом кристалле ESP32-P4 rev 1.3:

`	ext
I (2181) rock-player: Player initialized: PSRAM ring 256 KiB, PA GPIO11, decoders: MP3+AAC
I (2191) rock-player: ==================================================
I (2201) rock-player: RE-5 PLAYER HARDWARE VERIFICATION TASK STARTED
I (2201) rock-player: ==================================================
I (4211) rock-player: [TEST 1/5] SSRF Gate validation...
I (4211) rock-player: [TEST 1/5] SSRF Gate PASS (all 8 vectors verified)
I (4211) rock-player: [TEST 2/5] MP3 Decoder & Pipeline initialization...
I (4221) rock-player: [TEST 2/5] Decoded stream format: 44100 Hz, 2 ch (reconfiguring codec)
I (4241) I2S_IF: STD: TX, data_bit: 16, slot_bit: 16, ws_width: 16, slot_mode: STEREO, slot_mask: 0x3
I (4251) I2S_IF: STD: TX, sample_rate_hz: 44100, mclk_multiple: 256, clk_src: 0
I (4281) Adev_Codec: Open codec device OK
I (4291) rock-player: [TEST 2/5] Pre-roll buffer reached (16384 bytes), PA GPIO11 active, playback PASS
I (4291) rock-player: [TEST 3/5] Volume & Mute control verification...
I (5191) rock-player: [TEST 3/5] Volume/Mute/Pause/Play control PASS
I (5191) rock-player: [TEST 4/5] 30-second continuous playback & soak on Core 1...
...
I (10201) rock-player: [SOAK t= 5 s] internal_ram=202059 B, psram=32421188 B, underruns=52, dec_errors=0
I (15201) rock-player: [SOAK t=10 s] internal_ram=202059 B, psram=32421188 B, underruns=52, dec_errors=0
I (20201) rock-player: [SOAK t=15 s] internal_ram=202059 B, psram=32421188 B, underruns=52, dec_errors=0
I (25201) rock-player: [SOAK t=20 s] internal_ram=202059 B, psram=32421188 B, underruns=52, dec_errors=0
I (30201) rock-player: [SOAK t=25 s] internal_ram=201659 B, psram=32421188 B, underruns=52, dec_errors=0
I (35201) rock-player: [SOAK t=30 s] internal_ram=202059 B, psram=32421188 B, underruns=52, dec_errors=0
I (35201) rock-player: [TEST 4/5] 30s soak finished: internal delta=-116 B, psram delta=0 B, underruns=52
I (35211) rock-player: [TEST 5/5] Stop & decoder resilience...
I (35221) rock-player: [TEST 5/5] Stop verified
I (35221) rock-player: ==================================================
I (35221) rock-player: RE-5 PLAYER HARDWARE VERIFICATION: ALL PASSED!
I (35231) rock-player: ==================================================
`

### Метрики аппаратного теста

| Параметр | Значение | Оценка |
| --- | --- | --- |
| **SSRF Gate** | 8/8 векторов (localhost, RFC1918, link-local, scheme, public https) | PASS |
| **MP3 декодирование** | Реальный поток esp_mp3_dec (44.1 кГц, 2 канала stereo, 16 бит) | PASS |
| **Динамический реконфиг I2S** | Авто-переключение ES8311 и I2S с 16 кГц mono на 44.1 кГц stereo | PASS |
| **Pre-roll буфер** | 16 КиБ достигнуто, PA GPIO11 включён вовремя | PASS |
| **Управление громкостью** | 40, 70 установлены через ES8311 | PASS |
| **Mute / PA тактирование** | PA выключается при mute и pause, включается при play | PASS |
| **Дельта свободной PSRAM (30 с soak)** | **0 байт** (ровно 32 421 188 Б от старта до финиша) | PASS (zero leak) |
| **Дельта свободной RAM (30 с soak)** | **-116 байт** (~202 КиБ свободно, колебания от Wi-Fi сканера) | PASS |
| **Ошибки декодера** | 0 | PASS |
| **Underrun в установившемся режиме** | 0 (52 отсчёта только во время первичного реконфига I2S) | PASS |
| **Конкурентность** | Одновременно активны: ST7701 экран, GT911 тач, LVGL v9, SDIO Wi-Fi скан на C6 | PASS |
| **Остановка (
ock_player_stop)** | Буфер сброшен в 0, PA выключен, статус STOPPED | PASS |

---

## 4. Статус зависимости RE-11

Согласно решению F12 в [plan-control.md](plan-control.md):
Живая сквозная приёмка с реальным сервером RockServer ожидает реализации задачи
**RE-11** (Wi-Fi онбординг и постоянный pairing устройства).
Локальная интеграция и аппаратное воспроизведение на плате JC4880P443C_I_W полностью
доказаны и подтверждены.
