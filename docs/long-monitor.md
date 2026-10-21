# Длительный мониторинг ESP32-P4

Запускать после установки сборки с EQ-5 на плату. Скрипт `tools/long_monitor.py` не меняет прошивку и при открытии USB Serial/JTAG держит DTR/RTS выключенными. Одновременно с ним нельзя открывать `idf.py monitor` или прошивать через тот же COM-порт.

## Запуск на этом ПК

```powershell
Set-Location C:\repos\rock-esp32
$run = Join-Path (Get-Location) ('telemetry\long-monitor\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
$process = Start-Process -FilePath 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' `
  -ArgumentList @('tools\long_monitor.py', '--port', 'auto', '--output', $run, '--gateway', '192.168.31.1', '--board-ip', '192.168.31.115', '--metrics-interval', '0') `
  -WorkingDirectory 'C:\repos\rock-esp32' -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $run 'stdout.log') `
  -RedirectStandardError (Join-Path $run 'stderr.log')
$process.Id | Set-Content (Join-Path $run 'pid.txt')
Write-Output "PID=$($process.Id) LOG=$run"
```

Если DHCP сменил адрес платы, замените значение `--board-ip`; после следующей загрузки скрипт обновит его по строке `sta ip` в консоли. Проверка: `Get-Content "$run\status.json"`, затем `Get-Content "$run\events-*.log" -Tail 10`. `status.json` обновляется каждые 30 секунд, пока открыт порт. При необходимости остановить: `Stop-Process -Id (Get-Content "$run\pid.txt")`.

## Что записывается

- `serial-YYYY-MM-DD.log`: все строки консоли платы с временем ПК и часовым поясом; в том числе загрузка, Wi-Fi/SDIO, WSS, REST, аудио и состояние UI.
- `events-YYYY-MM-DD.log`: открытие и потеря COM-порта, остановка. Пример выше запускает пассивный режим без команд плате; без `--metrics-interval 0` отправляется `CMD:METRICS` каждые 30 секунд.
- `network-YYYY-MM-DD.log`: каждые 30 секунд ping роутера, ping платы после появления её IP в консоли и HTTP `/health/ready` с ПК.
- `session.json` и `status.json`: параметры запуска и последнее состояние монитора.

Скрипт пытается открыть порт снова после USB-обрыва. Каталог `telemetry` исключён из Git; логи могут содержать имя Wi-Fi и сведения об устройстве. ПК должен оставаться включённым, с USB2-кабелем и питанием платы. Проверки сети с ПК помогают сопоставить моменты сбоев, но сами по себе не доказывают качество Wi-Fi радиоканала платы.

Если экран затормозил или пропал звук, записать местное время события и текущую станцию/режим вывода. В логе сопоставить этот момент с `rssi`, `underruns`, `decode_errors`, `reconnects`, `internal`, `WSS`, `REST`, STA disconnect и ответами `/health/ready`.
