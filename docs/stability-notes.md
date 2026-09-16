# RE-16 stability findings (2026-09-16)

## Evidence and causes

- The pre-fix COM6 monitor ran for 10 minutes (20:25–20:35 local time). It recorded 24 `websocket_client: Started` lines and eight warnings of `Connection terminated while waiting for clean TCP close`. After STA obtained IP, it recorded no further STA disconnects in that interval. RockServer-side stability and the earlier 101 counts are facts supplied by the plan center; this work did not recheck production server logs.
- A controlled reboot during that monitor recorded three transient STA disconnects (`reason=201`, `201`, `7`) before `Wi-Fi connection established` at uptime 24.6 s. This shows why clearing NVS after a handful of attempts could discard working credentials. The historical `rock_wifi` erase was not present in the captured interval; the pre-fix call site was `rock_platform_init()` after its 30 s boot wait.
- The diagnostic build recorded `WSS connected` at 20:37:43, `protocol.welcome` at 20:37:52, then a local stop at 20:38:01. Another connection at 20:38:14 stopped at 20:38:24 with no welcome. The previous 10 s welcome wait was too close to the observed 9 s delivery. It is now 30 s.
- The ESP WebSocket client dispatches PING and PONG as `WEBSOCKET_EVENT_DATA`. Our event callback treated every opcode other than text (1) as a fatal frame and queued a negative receive result. The Rust session then called `rock_ws_stop()`. The callback now ignores PING/PONG, which the client library handles, and reports CLOSE separately. This explains local closes after successful upgrade without a TLS or Wi-Fi error.
- The firmware did not send `device.heartbeat` despite the server's 20 s interval and 60 s offline TTL. It now sends sequence-numbered heartbeats every 20 s after the initial full state.
- A transient HTTPS readiness timeout at 20:37:00 returned from platform initialization before the WebSocket queue and event group were created. The next device-session returned 200, then `rock_ws_start()` asserted in `xEventGroupClearBits` on a null event group. The queue and event group are now created before the health request, and a failed readiness check leaves the connection loop to retry.
- The old HTTP diagnostic line included the `device-session` response body, including its access token. Logging now records only status, ESP error name, and response length. Raw local diagnostic logs are excluded from the commit and removed after analysis.

## Wi-Fi and onboarding changes

- Saved `rock_wifi` credentials survive STA failures. Startup retries use 1–30 s backoff; after 10 minutes of continuous failure, onboarding opens while the saved values remain in NVS. A PIN-protected portal action can explicitly forget the network. `POST /connect` still replaces credentials and reboots into STA.
- Onboarding scans in STA mode before starting SoftAP and caches up to 30 records. `/api/scan` first tries APSTA, then uses that cached list if APSTA scan fails or returns empty. Logs report scan return codes, counts, and elapsed time without SSIDs or passwords.

## Verification

- `cargo test --target x86_64-pc-windows-msvc --test device_control_contract`: 14 passed, 0 failed.
- `ninja -C build -j 4`: passed; application image fits the existing app partition.
- P4 application flash on re-detected COM6: hash verified. No partition table or C6 flash change.
- Post-fix reboot: `rock_wifi` loaded from NVS; no onboarding QR. STA later reassociated when the 2.4 GHz AP became reachable, and the acceptance run below started from that state.
- 30-minute continuous WSS/heartbeat acceptance run: completed (see below).
- APSTA `/api/scan` response on the live portal: still pending board verification (deferred, see Acceptance).

## Acceptance run (2026-09-16, post-fix build)

`idf.py -p COM6 monitor --no-reset --timestamps` captured 21:08:08–21:39:55 local time (~31.8 min). Findings:

- Two `websocket_client: Started` lines in the whole window, versus 24 in 10 minutes before the fix.
- Session 1 (21:08:17–21:12:35): `protocol.welcome` arrived 0.4 s after connect, then `device.registered`, then `device.heartbeat` every 20 s with `device.heartbeat_ack` (worst observed ACK latency ~13 s). PONG frames (`opcode=10`) were handled by the client library instead of killing the session.
- Watchdog scenario occurred live: from 21:11:57 heartbeats were still sent with local success, but no ACK or PONG returned (last ACK 21:11:50). The client detected `WSS heartbeat ACK timeout age_ms=45033` at 21:12:35 — 45.0 s of ACK silence, before the server's 60 s offline TTL — and closed the session locally (`WSS local stop`), instead of waiting for a server-side 1008.
- Reconnect: the first `device-session` attempt at 21:12:48 failed transiently (`ESP_ERR_HTTP_CONNECT`, TLS connect timeout — the same upstream hiccup that stopped the ACKs) and was retried automatically; session 2 connected at 21:13:08, 33 s after the local stop. No server-initiated close was observed at any point.
- Session 2 ran from 21:13:08 until the monitor ended (26 min 43 s) with stable 20 s heartbeat/ACK cycles.
- Totals: 92 heartbeats sent, 90 ACKs, 88 PONGs; the two unACKed heartbeats are exactly the pair that triggered the watchdog.
- No STA disconnects, no asserts, no panics, and no task watchdog resets in the window.

Deferred: the live onboarding `/api/scan` check. It requires the owner to hide the 2.4 GHz AP for ~11 minutes and to join `RockCast-Setup` with a phone; the owner was not available during this autonomous acceptance session. Per the new semantics the device keeps its NVS credentials, retries STA with 1–30 s backoff for 10 minutes, and only then opens the portal, so the check remains a safe manual follow-up. No unsafe workaround (NVS erase, `/connect`, `/forget`) was attempted.

## Open questions

- Whether APSTA scanning on this specific ESP-Hosted 3.0.7 C6 reports records while SoftAP has a connected phone. The pre-AP cache provides a fallback, but the portal response still needs a live test — deferred until the owner can hide the 2.4 GHz AP and join the portal (see Acceptance).
- Production-side confirmation that one WebSocket upgrade stays active after the radio network is available again is reserved for the plan center. Device-side, the acceptance run shows one upgrade staying healthy for 26+ minutes after a watchdog reconnect.
