# RockCast-class ESP32 implementation plan

Reworked on 2026-09-09 after the plan audit recorded in
[`plan-control.md`](plan-control.md). Execution and all cross-repository tasks
are coordinated from that control document; tasks are dispatched from the
control dialog to the repositories below.

## Repositories

| Repository | Responsibility |
| --- | --- |
| rock-esp32 | firmware, audio, GUI, voice client, OTA (this repo) |
| rockserver | contracts and enabling server implementation (Steps 1–2) |
| rockmobile | controller UI for station dispatch (Step 5) |
| rockcast | visual design reference only; design-token extraction (task RC-1) |

## Product decision

The owner decided on 2026-09-09 that the JC4880P443C_I_W device will be a
standalone RockCast-class radio rather than only a remote display. The finished
device must:

- browse and search the RockServer station catalog on its touch display;
- play stations through its local ES8311 codec and speaker;
- expose playback and volume controls to Rockmobile through the existing
  RockServer device-control plane;
- listen locally for the wake word `ESP`, then stream only the following
  bounded utterance to the existing RockServer voice path;
- execute touch, Rockmobile, and voice requests through the same typed
  RockServer intent/command boundary.

Two completion defaults were recorded by the 2026-09-09 rework; the owner may
flip either explicitly:

- OTA firmware update is in scope: a finished radio must be updatable without
  USB.
- End-user Wi-Fi onboarding is in scope: the device cannot ship with sdkconfig
  credentials.

DC-019 and DC-020 remain deferred because sensors are not available. They do
not block the radio, GUI, playback, or radio-only voice milestones in this
plan.

## Target architecture

```text
Rockmobile ---- device-control ----+
Touch GUI ----- station intent -----+--> RockServer --> device.command --> ESP32 player
"ESP" + voice - authenticated voice +                                |--> ES8311/speaker
                                                                  +--> LVGL display
```

The final truthful device manifest is expected to combine the `player`,
`controller`, `display_surface`, and `voice_endpoint` roles. Capabilities are
added only after their handlers and physical hardware paths pass their
milestone acceptance checks.

RockServer remains the owner of accounts, catalog search, station-to-stream
resolution, typed intents, permissions, command routing, and lifecycle. The
firmware does not create a second API, accept a direct Rockmobile protocol, or
trust a stream URL supplied by UI text or voice input.

## Verified dependency baseline (2026-09-09 audit)

The first draft of this plan assumed server behavior that is not implemented
yet. This baseline is the reason the two RockServer steps (2–3) exist; every
later step that needs one of these deltas names it.

- Catalog browse (`GET /api/v1/catalog/stations`) and search
  (`POST /api/v1/search`) exist but are anonymous and return `stream_url`;
  browse is not in `openapi.yaml` at all.
- `station.play_stream` resolution is only a planned OpenAPI contract: the
  command router forwards `station.play_station` unchanged; nothing resolves a
  catalog stream server-side today.
- Voice `/voice/stream` is anonymous station search from a transcript; the
  typed `UserIntent` model exists but is not wired to voice; the protocol has
  no cancel frame; the OpenAPI documents sample rates and session bounds the
  runtime does not honor.
- Rockmobile hard-validates `media.volume` (min 0, max 100, step 1..100 — any
  violation aborts applying the whole directory snapshot), sets a 10 s command
  deadline with client-side expiry, and today has no UI that sends
  `station.play_station`.
- DC-018 firmware seams are implemented, but golden presentations, power-cycle
  touch checks, and offline transitions are still open physical acceptance
  items.
- The partition table gives the app 3 MiB with no OTA slots; the image is
  1.8 MiB and about 13 MB of the 16 MB flash is unallocated.
- `esp_audio_codec` >= 2.6.0 requires ESP32-P4 chip >= 3.0; this board is
  P4 v1.3. `esp-sr` supports ESP-IDF 6.x only from v2.4.1 and experimentally.

## Step 1: De-risk components, freeze partitions, prove audio (rock-esp32)

This step runs first (owner decision, 2026-09-09): it depends on no other
repository and isolates the highest hardware risk before any contract or
server work is invested.

The verified board mapping is documented in
[`board-hardware.md`](board-hardware.md): ES8311 control shares I2C1 with
GT911; I2S uses BCLK GPIO12, MCLK GPIO13, WS GPIO10, speaker data GPIO9,
microphone data GPIO48, and amplifier enable GPIO11.

1. Migrate the partition table before player code lands: keep NVS at 0x9000,
   add otadata and two OTA app slots sized for LVGL plus codecs plus esp-sr
   out of the 16 MB flash. Committing this later forces a full reflash.
2. Spike `esp_audio_codec` on P4 v1.3: v2.6.0 requires chip >= 3.0, so pin
   the newest version that builds and decodes MP3 and AAC on v1.3.
3. Spike `esp-sr` (>= 2.4.1) on ESP-IDF 6.1: build the AFE and WakeNet only,
   no behavior; pin the version.
4. Introduce one shared I2C1 owner for GT911 and ES8311. Do not initialize a
   second bus over the display-owned pins.
5. Initialize ES8311 and I2S through `esp_codec_dev` (official codec
   component; ES8311 playback and record are supported).
6. Play a deterministic PCM test tone through the speaker.
7. Capture at least ten seconds of microphone PCM and measure effective
   sample format, level, noise floor, and clipping.
8. Run capture and playback concurrently while LVGL, touch, and ESP-Hosted
   are active.
9. Record internal RAM, PSRAM, task stacks, CPU load, I2S underruns, and
   watchdog behavior.

Acceptance:

- microphone and speaker work on the real board without corrupting touch or
  network operation (the speaker path is proven electrically; the audible
  acoustic confirmation is tracked as task RE-12 in
  [`plan-control.md`](plan-control.md) until a speaker is attached);
- concurrent capture/playback is stable and bounded;
- pinned component versions are recorded in `main/idf_component.yml`, the new
  partition table is committed, and spike results are recorded in docs;
- audio samples, Wi-Fi credentials, and tokens never enter normal logs.

## Step 2: Freeze contracts before implementation (rockserver)

Update RockServer OpenAPI, canonical device-control fixtures, roadmap, status,
and task log before implementing new public behavior.

1. Define two authenticated device-facing catalog paths: cursor-paginated
   browse (explicit cursor, at most 20 per page) and ranked search (query at
   most 128 characters, at most 20 results, one page, no cursor). Ranked
   search is not cursorable in the existing service, so a single cursor path
   would be new server design; two paths reuse what exists. Both return
   station metadata without stream URLs and reuse the existing
   catalog/search services.
2. Bound the firmware side: one retained page, deterministic rejection of
   unknown and oversized requests.
3. Promote `station.play_station` to server-resolved `station.play_stream`
   from a planned to an implemented contract: same `command_id`,
   accepted/result lifecycle, SSRF-validated `stream_uri` that is never
   logged.
4. Record `station_list`, `search`, `loading`, `offline`, and
   `playback_error` as local UI states. Decision: no new remote presentation
   types beyond `text`, `now_playing`, `sensor_grid`.
5. Extend the voice contract: device-session authentication,
   `source_device_id`, `voice.main` surface, locale, an explicit cancel
   frame, and deadlines. Fix the voice OpenAPI drift (16 kHz mono only,
   2 MiB / 60 s session).
6. Encode Rockmobile compatibility into fixtures: `media.volume` exactly
   min 0 / max 100 / step 1 / mute; command semantics "accept fast, result
   async" within the server's 30 s bound against Rockmobile's 10 s client
   expiry.
7. Update the ESP32 register fixture to the truthful final shape
   (player/controller/display_surface/voice_endpoint roles and the
   capabilities above). Firmware still advertises each capability only after
   its handler passes its board test.
8. Confirm Rockmobile needs no protocol change beyond the Step 5 UI work.

Acceptance:

- OpenAPI and every canonical fixture validate (`tests/openapi_contract.rs`);
- `x-rockserver-status: planned` markers are removed only for operations
  implemented in Step 3;
- station selection always carries an explicit player `device_id`;
- no contract exposes credentials, provider identifiers, or unvalidated
  stream URLs.

## Step 3: Implement the server enablement (rockserver)

Step 2 freezes the words; this step makes them true. Without it, Steps 4, 6,
and 7 of this plan are not executable.

1. Device-facing authenticated catalog endpoints (device-session bearer)
   returning station metadata without `stream_url`, reusing the existing
   search/catalog services and their rate limits.
2. Command-router resolution: on `station.play_station`, resolve the catalog
   stream server-side and dispatch a validated `station.play_stream` under
   the same `command_id` to the explicit player target; validate the target's
   `player` role and `media.station` capability first.
3. Voice: authenticate device sessions on `/voice/stream`; carry
   `source_device_id`, surface, locale, and cancellation through the session;
   route the recognized transcript through typed `UserIntent` resolution and
   the normal device command router for the radio intents (play radio or
   named station, stop, volume). No ESP-specific STT or command API.
4. Runtime tests and canonical fixtures for all three paths; update the
   roadmap, status, and task log in that repository.

Acceptance:

- every Step 2 contract is implemented and covered by RockServer contract
  tests, not fixture-only;
- Steps 4, 6, and 7 of this plan are unblocked.

## Step 4: Player MVP (rock-esp32) — needs Step 1 (audio) and Step 3 (RS-2, RS-3)

1. Add a bounded HTTPS stream client using the ESP certificate bundle,
   explicit timeouts, at most five redirects, and reconnect backoff.
2. Add only codecs proven against real catalog stations, initially MP3 and
   AAC (the versions pinned in Step 1). Reject unsupported content types
   without guessing.
3. Implement decoder to PCM ring buffer to I2S, placing bulk buffers in PSRAM
   while keeping DMA-critical buffers in appropriate memory.
4. Implement `station.play_station`, resolved stream playback, pause, stop,
   volume, mute, and replacement of an old stream by a new station.
5. Acknowledge commands immediately; publish `command.result` only after the
   hardware operation has actually succeeded, within the server's 30 s bound.
   Rockmobile expires at 10 s by design; that is expected, not an error.
6. Publish playback and volume state only after the hardware operation has
   actually succeeded.
7. Render bounded `buffering`, `now_playing`, stopped, and playback-error
   views.
8. Advertise `media.volume` exactly as the fixture: min 0, max 100, step 1,
   mute. Add `player`, `media.station`, `media.playback`, and `media.volume`
   to the manifest only after the corresponding handler passes its board
   test.
9. Advertise `next` and `previous` only after a real bounded history or queue
   exists.

Acceptance:

- `station.play_station` and the resolved stream path work through RockServer
  using canonical fixtures or an integration harness (the Rockmobile UI
  arrives in Step 5);
- command lifecycle remains idempotent across retries and reconnects (the
  server dedups by command fingerprint for 24 h);
- playback survives an extended soak without heap growth, underruns, or
  watchdog resets.

## Step 5: Rockmobile station control (rockmobile) — needs Step 4

1. Add "play on device" from the station catalog to a usable controller
   target (player role, online, fresh, `media.control` scope), constructing
   `station.play_station` through the existing lifecycle model
   (Pending...Succeeded/Failed/Expired) without auto-retry.
2. Keep the 10 s deadline and expiry semantics visible to the user instead of
   hiding them.
3. Verify the whole directory snapshot parses with the full ESP32 manifest
   present — a malformed `media.volume` aborts the entire snapshot in today's
   client.

Acceptance:

- on physical hardware, Rockmobile discovers the ESP32, starts a catalog
  station, pauses, stops, changes volume, and observes the resulting state
  through RockServer.

## Step 6: Native station GUI (rock-esp32) — needs DC-018 closure, Step 4, RC-1

Build on the LVGL v9 and GT911 seams of DC-018 — after closing its open
physical acceptance.

0. Close the open DC-018 physical acceptance first: golden presentations,
   power-cycle touch coordinate checks, and offline/reconnect presentation
   transitions, recorded in `dc-018-progress.md`.
1. Add a home/now-playing screen with station, playback, volume,
   connectivity, and microphone state.
2. Add a scrollable browse screen backed by one fresh bounded RockServer
   page.
3. Add text search with an LVGL on-screen keyboard; voice search is added in
   a later step.
4. Add play, pause, stop, volume, retry, and back interactions with
   touch-sized controls.
5. On station touch, submit `station.play_station` through RockServer with
   the ESP32's explicit player target. Show `Starting...` until command
   result or observed playback state arrives.
6. Represent `loading`, empty result, offline, retry, and server-error states
   explicitly (these are local UI states per Step 2; no new remote
   presentation types).
7. Discard or hide stale catalog results after the server becomes
   unavailable; never present an old page as current.
8. Match the RockCast palette, typography hierarchy, spacing, and composition
   using the design tokens extracted by task RC-1, without copying
   desktop-only interaction patterns.

Acceptance:

- browse, keyboard search, station selection, and playback controls work
  using touch on the physical display;
- no touch callback performs blocking network work inside the LVGL task;
- malformed and oversized catalog pages do not mutate the visible model.

## Step 7: Push-to-talk voice transport (rock-esp32) — needs Step 3 voice (RS-4), Step 6 screens

Prove the end-to-end voice path before enabling continuous wake-word
detection.

1. Register `voice.main` with truthful `pcm16_mono_16000` input capability.
2. Capture microphone PCM into a bounded ring buffer and pass it through
   local voice activity detection.
3. Stream a bounded utterance to the RockServer voice WebSocket with
   authenticated source device/surface, locale, cancellation, and timeout.
   Budget the second concurrent `esp_websocket_client` instance (stack, RAM)
   alongside the control WSS within the fixed transport limits.
4. Reuse RockServer's provider-neutral speech recognition and typed
   `UserIntent` resolution (implemented in Step 3); do not add an ESP-specific
   STT or command API.
5. Route the resolved intent through the normal device command router to the
   explicit ESP32 player target.
6. Display listening, processing, execution, success, ambiguity, timeout, and
   offline states.
7. Never persist raw audio or include audio/transcripts in ordinary logs.

Initial radio-only scenarios:

- play a requested genre or named station;
- stop playback;
- set volume;
- handle silence, cancellation, ambiguity, unsupported intent, STT failure,
  and network loss.

The later multi-domain sensor/actuator scenarios remain deferred with DC-019
and DC-020.

## Step 8: Continuous wake-word detection (rock-esp32)

Use the official ESP-SR Audio Front End on ESP32-P4, at the version pinned by
the Step 1 spike. Its input is interleaved signed 16-bit PCM at 16 kHz and it
supplies WakeNet, VAD, noise suppression, automatic gain control, and
acoustic echo cancellation.

References:

- [ESP-SR Audio Front End](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html)
- [WakeNet on ESP32-P4](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/wake_word_engine/README.html)
- [Wake-word customization](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/wake_word_engine/ESP_Wake_Words_Customization.html)
- [ESP-SR acoustic echo cancellation](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/acoustic_echo_cancellation/README.html)

Implement this state machine:

```text
idle_listening
  -> wake_detected
  -> acknowledgement + listening indicator
  -> capturing_command
  -> VAD end or hard timeout
  -> uploading
  -> executing
  -> idle_listening
```

1. Keep idle audio and a short pre-roll only in RAM. Send nothing before a
   local wake detection.
2. Validate the complete pipeline first with Espressif's available `Hi ESP`
   model, then test the required exact wake word `ESP`.
3. Measure false accepts and false rejects at different distances, speaker
   volumes, voices, and room-noise levels.
4. If no production-quality stock model recognizes exact `ESP`, obtain or
   train a custom WakeNet model; do not ship a string heuristic as a
   wake-word detector.
5. While radio is playing, feed both microphone audio and a playback
   reference into AFE acoustic echo cancellation so the device does not
   trigger on its own speaker.
6. Add a visible microphone mute control and persistent muted/listening/
   uploading indicators.
7. Play a short acknowledgement only after wake detection and prevent that
   acknowledgement from contaminating the captured command.

Acceptance:

- no pre-wake audio leaves the device;
- exact `ESP` meets an agreed false-accept/false-reject threshold;
- commands work while radio is playing at normal volume;
- silence or timeout clears all voice buffers and returns to idle listening.

## Step 9: OTA and Wi-Fi onboarding (rock-esp32) — scope defaults, owner may flip

1. Deliver OTA updates through `esp_https_ota` and the certificate bundle on
   the Step 1 partition layout, with rollback and truthful version reporting.
   This step may be pulled earlier once the partition layout from Step 1 is
   committed.
2. Deliver end-user Wi-Fi onboarding: an LVGL SoftAP/captive-portal flow that
   stores credentials in NVS; the sdkconfig development path stays available.
3. Both ride the existing flash-without-NVS-erase rule so a paired identity
   survives updates.

Acceptance:

- a firmware update over Wi-Fi completes, rolls back on failure, and keeps
  the paired identity;
- a user with no sdkconfig credentials can join the device to Wi-Fi and
  complete pairing from the display.

## Step 10: Integrate, harden, and soak

Run the complete scenarios on physical hardware:

1. Rockmobile to ESP32 play, pause, stop, and volume.
2. Touch browse to station playback.
3. Touch text search to station playback.
4. `ESP` plus a radio request to RockServer intent to local playback.
5. Wake-word recognition during loud local playback.
6. Station replacement while decoding another stream.
7. Wi-Fi loss, C6 recovery, RockServer restart, and control reconnect.
8. Slow, malformed, oversized, redirected, and unsupported station streams.
9. Power cycle with safe restoration of settings but no automatic replay
   unless explicitly chosen as product behavior.
10. Firmware update through OTA including a forced rollback.
11. The Wi-Fi onboarding flow end to end (if Step 9 stays in scope).
12. At least 24 hours of playback plus repeated wake-word commands while
    tracking heap, PSRAM, CPU, reconnects, decoder errors, underruns, and
    watchdog resets.

Before each milestone is marked complete, run the repository's host contract
tests and serialized ESP-IDF build. Flash only after a successful build,
re-detect the serial port first, and record the physical verification result
in the relevant progress document.

## Delivery order

The implementation order is deliberately:

1. partition migration, component spikes, and audio proof (rock-esp32);
2. contracts (rockserver);
3. server enablement (rockserver);
4. player MVP (rock-esp32);
5. Rockmobile station control (rockmobile);
6. DC-018 closure and the native GUI (rock-esp32, design tokens from
   rockcast);
7. push-to-talk voice (rock-esp32);
8. exact `ESP` wake word and echo cancellation (rock-esp32);
9. OTA and Wi-Fi onboarding (rock-esp32);
10. integration soak.

The board de-risk runs first because it depends on no other repository and
validates the highest hardware risk before contracts and server work are
invested. Each step leaves a separately testable product slice and isolates the
remaining hardware risks before the always-listening feature combines playback,
capture, DSP, network, and UI concurrency.
