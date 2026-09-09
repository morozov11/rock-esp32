# DC-017 device control

## Security boundary

The paired `device_id` and long-lived `device_secret` are stored in the ESP-IDF default NVS
partition under the `rock_auth` namespace. NVS is intentionally not encrypted: the owner accepted
on 2026-09-08 that physical flash extraction is outside DC-017's threat model. The firmware must
still never log credentials, pairing proofs, Wi-Fi credentials, stream URIs, or access tokens.
Short-lived device-session access tokens live only in RAM and are renewed after every reboot or
authentication failure.

## Network and transport

The P4 uses the on-board C6 through official ESP-Hosted over four-bit SDIO (CLK 18, CMD 19, D0 14,
D1 15, D2 16, D3 17, reset 54). The host and companion are pinned to ESP-Hosted 3.0.7 because
3.0.3 is the first 3.x release carrying the ESP-IDF 6.1 host fix. The owner explicitly approved
updating the companion firmware on 2026-09-08.

The original vendor C6 recovery images remain untouched:

- `D:\work\JC4880P443C_I_W\8-Burn operation\Burn files\JC-C6-slave_v2.3.2.bin`
  (`SHA-256 F9EC1205C5FA1E497032DEDCEE33D2AB9560BD2768FC1EA06ACB6D8E2773F80C`)
- `D:\work\JC4880P443C_I_W\8-Burn operation\Burn files\C6-JC4880P443-2.1.10.bin`
  (`SHA-256 7B68C4CFF6E3CF288896A89B4A09379396F62C6BFBFBDC3984B9E142CFB4DB0C`)

These two files are the recovery backup and must not be moved, renamed, overwritten, or used as
build output. Updating the connected companion is authorized only with an official matching
ESP-Hosted 3.x image after its serial port and target have been positively identified.

Hardware progress and the C6 update safety hold are recorded in `docs/dc-017-progress.md`.

WSS uses the official `esp_websocket_client` component rather than a second Rust TLS stack. The
fixed operating limits are:

| Resource | Limit |
| --- | ---: |
| JSON WebSocket frame | 65,536 bytes |
| JSON payload | 61,440 bytes |
| queued inbound frames | 1 (newest complete frame replaces stale queued data) |
| WebSocket task stack | 8 KiB |
| control task stack | 12 KiB |
| heartbeat | 20 seconds |
| missed heartbeat/offline TTL | 60 seconds |
| reconnect delay | 1–30 seconds, full jitter |
| blocking network operation | 10 seconds |

TLS validation uses the ESP certificate bundle and starts only after Wi-Fi, SNTP synchronization,
and a successful HTTPS readiness request. Automatic reconnect is disabled in the component so the
firmware owns bounded exponential backoff and can reset a silent C6 on GPIO54. Task watchdogs stay
enabled; all waits are bounded and the control loop yields on every iteration.

Firmware version reporting uses only the compile-time Cargo package version after checking the
protocol's 64-byte printable-ASCII bound. It never includes a branch name, build path, environment
variable, credential, or arbitrary Git metadata.

## Configuration

`sdkconfig` is ignored by Git. It contains the local Wi-Fi SSID/password, RockServer HTTPS base URL,
and display name. Pairing identity is never supplied through source code, build arguments, or
environment variables.

The tracked partition table reserves 24 KiB for ordinary NVS and 3 MiB for the factory app. A
normal `flash` updates the bootloader, partition table, and application without erasing NVS, so an
already paired identity survives firmware updates.

The browser handoff uses the existing RockServer/RockCast form
`/?code=<SHORT>#secret=<APPROVAL_SECRET>`. The approval proof stays in the URL fragment and is not
sent in the browser's HTTP request. Firmware configuration is Kconfig-backed, not process
environment variables; local values are entered in `sdkconfig` through `menuconfig`.

## Scope

DC-017 provides pairing, persistent identity, device-session renewal, bounded WSS transport,
heartbeat/reconnect, manifests/state serialization, and generic command dispatch. It does not add
DC-018 presentation handlers, sensors, voice/audio, playback, catalog browsing, or OTA. The
firmware advertises only the capabilities backed by handlers in this milestone.
