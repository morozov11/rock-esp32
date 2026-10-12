#pragma once

// Private plumbing shared between relay_bsp.c (source domain: upstream,
// decode, ring ownership) and rock_relay_http.c (listener + client serving)
// after the phase-4 split. NOT a public header: public ABI is relay_bsp.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rock_ring.h"

// --- relay_bsp.c (source domain owner) ---

bool relay_stopped(void);
bool relay_is_running(void);
bool relay_is_passthrough(void);
const char *relay_content_type(void);

// Cushion a fresh/clamped reader keeps behind the live edge (depends on the
// negotiated transport).
size_t relay_cushion_bytes(void);

// The shared fanout ring (owned and reset by the source lifecycle).
rock_ring_t *relay_ring(void);

// Offset of the next plausible MP3 frame sync in data[0..len).
size_t relay_find_mpeg_frame(const uint8_t *data, size_t len);

// Fixed 48 kHz stereo WAV header for the transcode serve mode.
void relay_make_wav_header(uint8_t header[44]);

// Socket send helpers (backpressure-tolerant, stop-flag aware).
// relay_send_audio sends whole chunks only and drives the progress log.
bool relay_send_all(int sock, const void *buf, size_t len);
bool relay_send_audio(int sock, const uint8_t *data, size_t len,
                      size_t *total_bytes, size_t *last_log_bytes);

// --- rock_relay_http.c (listener + client serving) ---

// Non-blocking accept poll for one pending client; called from the feeder
// loop between decode iterations so connects never stall the media pace.
void relay_listener_poll(void);

// Close the listener socket on feeder exit.
void relay_listener_close(void);

// Wake a lingering accept() without closing the fd (the feeder owns it and
// closes it on exit, avoiding fd-reuse races).
void relay_listener_wake(void);

// Live client tasks (they drain the ring and must be gone before a reset).
int relay_client_count(void);

// Zero the client count at source (re)start, mirroring the listener reset.
void relay_clients_reset(void);
