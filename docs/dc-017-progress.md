# DC-017: Статус задачи — Выполнена

Обновлено: 2026-09-09. Задача DC-017 ("ESP32 provisioning and transport core") полностью выполнена и верифицирована на целевом оборудовании JC4880P443C_I_W (ESP32-P4 + ESP32-C6).


## Реализовано и проверено

- Выбран и зафиксирован режим обычного, незашифрованного NVS: `device_id` и
  `device_secret` хранятся в namespace `rock_auth`; короткоживущий access token остаётся
  только в RAM. Это согласовано для модели угроз, в которой физическое извлечение flash вне
  области DC-017.
- Реализованы pairing, QR/deep-link, обновление device session, ограниченный WSS transport,
  heartbeat/reconnect, generic manifest/state serialization и generic command dispatch.
- Контрактные тесты с fixture RockServer успешно прошли: 6 из 6 (`cargo test --target
  x86_64-pc-windows-msvc`, с `ROCKSERVER_ROOT=C:\repos\rockserver`).
- Полная ESP-IDF 6.1 сборка для ESP32-P4 успешна. Образ
  `cmake-build-debug\rock_esp32.bin` имеет размер `0x1aea00`; в factory-разделе 3 МиБ
  свободно 44%.
- На P4 прошиты bootloader, partition table и DC-017 application через COM6. Проверка хеша
  записанных данных прошла успешно. Обычный `flash` не стирал NVS (`0x9000` не был целью
  прошивки).
- Монитор подтверждает ESP32-P4 v1.3, 16 МиБ flash, 32 МиБ PSRAM, дисплей/LVGL и SDIO-подключение
  C6 на GPIO CLK=18, CMD=19, D0..D3=14..17, RESET=54.

## C6: обновление до ESP-Hosted 3.0.7 выполнено успешно

Через безопасный временный OTA-загрузчик P4 с сохранённой структурой NVS (`0x9000..0xf000`) образ `eh_cp_ota_coprocessor_ota.bin` (v3.0.7) был передан в C6 по шине SDIO 4-bit.
При перезагрузке согласование версий подтвердило полный match:

```text
eh_init_evt: slave chip id: 0x0d (esp32c6)
eh_init_evt: esp-hosted fw versions: host=3.0.7 coprocessor=3.0.7 (match)
eh_init_evt: SDIO SW_AGGR negotiated (e2h=15872B h2e=15872B)
eh_init_evt: SDIO mode: slave=streaming host=streaming
```

## Вывод сетей Wi-Fi на экран

Реализован интерактивный сканер Wi-Fi с отображением списка найденных сетей на встроенном 4.3" дисплее ST7701 (800x480) через LVGL:
- Тёмная тема интерфейса (`0x0F1115`), карточка списка (`0x14171F`) со скруглением и рамкой.
- Для каждой найденной сети выводятся:
  - RSSI в dBm с цветовой индикацией (зеленый >= -60 dBm, желтый >= -75 dBm, красный < -75 dBm);
  - SSID сети (или `<Hidden Network>`);
  - Номер радиоканала (`CH x`);
  - Тип аутентификации (`OPEN`, `WPA2-PSK`, `WPA/WPA2`, `WPA3-PSK` и др.).
- При отсутствии сконфигурированной в `sdkconfig` сети (`CONFIG_ROCK_WIFI_SSID`) устройство переходит в режим периодического сканирования с обновлением экрана каждые 10 секунд.
- Проверено вживую на плате `JC4880P443C_I_W`: сканирование стабильно находит доступные сети (Stranger_n5, TP-Link_4A46, Beeline_2G, RT-WiFi, MTSRouter и др.) и корректно обновляет графический экран.

## Сохранность NVS и резервные образы

- Раздел NVS (`0x9000..0xf000`) не перезаписывался и не стирался.
- Резервные копии вендора сохранены без изменений:
  - `D:\work\JC4880P443C_I_W\8-Burn operation\Burn files\JC-C6-slave_v2.3.2.bin`
  - `D:\work\JC4880P443C_I_W\8-Burn operation\Burn files\C6-JC4880P443-2.1.10.bin`

