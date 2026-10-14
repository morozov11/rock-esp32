# ESP32-P4 memory budget

Measured on JC4880P443C_I_W with ESP-IDF 6.1, RockServer proxy playback of
181.FM Classic Buzz (MP3, 44.1 kHz stereo). `CMD:METRICS` reports the current
heap and stream counters. The largest free DMA block matters more than total
free heap when an I2S format change reallocates descriptors.

| Subsystem | Placement | Reserved or measured size |
| --- | --- | ---: |
| LCD framebuffer, RGB565 480 × 800 | display driver / PSRAM | 750 KiB |
| LVGL draw buffer, RGB565 480 × 50 | internal RAM | 47 KiB |
| LVGL task stack | task heap | 16 KiB |
| GUI serial task stack | task heap | 32 KiB |
| Pairing QR canvas, RGB565 320 × 320 | PSRAM, when used | 200 KiB |
| Unified source ring (compressed or 48 kHz PCM) | PSRAM, persistent after first use | 2 MiB |
| Source feeder task stack and staging buffers | task heap / PSRAM | 6 KiB stack + about 40 KiB PSRAM |
| Local PCM ring | PSRAM | 2 MiB |
| Local stream task stack and static decode buffers | task heap / static RAM | 8 KiB stack + 16 KiB buffers |
| Audio output and spectrum task stacks | task heap | 4 KiB each |
| I2S TX/RX DMA, 6 × 256 stereo frames per channel | DMA-capable internal RAM | about 12 KiB plus descriptors |
| Unified station catalog (128 items PSRAM, UC-4) | PSRAM, allocated on first use | 48 KiB |
| Wi-Fi, ESP-Hosted, TLS, WebSocket, decoder, LVGL objects | mixed | variable; measure at runtime |

The I2S driver rounds the configured 240-frame DMA blocks to 256 frames. The
player reserves stereo I2S buffers during startup, before TLS and the MP3
decoder fragment internal RAM. Before that change, the format switch happened
with only 14,627 bytes of DMA-capable memory free and a 2,176-byte largest
block; allocation failed and the board rebooted.

Observed with the 2 MiB PCM ring: roughly 115 KiB internal free while idle,
33–44 KiB during playback, and 1.8–4 KiB as the largest free DMA-capable
block. Total free heap during playback was about 30.5 MiB, mostly PSRAM.
Large non-DMA buffers must stay in PSRAM.

Unified station catalog (UC-4..UC-6, hardware measurement on 2026-10-14):
The 128-station accumulated buffer is allocated strictly in PSRAM
(`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`), consuming 48 KiB (128 × 378 B).
During full catalog accumulation (60+ stations), search, load-more, and offline
stale-recovery cycles on ESP32-P4 v1.3 (COM6), internal free heap stayed stable
at ~164 KiB, with total free heap at ~30.58 MiB. Internal RAM did not degrade.
forces fresh DMA allocation remains a risk and should be tested before adding
more internal allocations. Current PSRAM allocation policy prefers internal
RAM for allocations up to 16 KiB and reserves 32 KiB of internal RAM, so new
network or UI buffers need explicit placement review.

For a 128 kb/s MP3 station, compressed input is about 16 KiB/s and decoded
44.1 kHz stereo PCM is about 172 KiB/s. Since the unified-source rework
(2026-10-10, `cast-progress.md` §11) one upstream connection to the
RockServer proxy feeds the shared source ring; the ring holds compressed
audio in MP3 passthrough mode (about 2 minutes) or 48 kHz stereo PCM for
AAC stations (about 10 seconds). The speaker consumes the same ring
through its own decoder (or directly for PCM), pacing itself on the local
2 MiB PCM ring: playback starts after 512 KiB (about 3 seconds) and a
prolonged upstream stall returns output to buffering rather than playing
short fragments. In passthrough mode the ring is not media-paced, so a
paused speaker consumer clamps to the live edge after the ring wraps.

The control task renews the 15-minute native device token every 10 minutes
and refreshes both the player's and the source's stored bearer; the source
reconnects with the newest token after an upstream failure, and the server
caps one relay connection at one hour.
