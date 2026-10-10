#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOT a well-known proxy port: the LAN router inspects/filters cross-client
// traffic on 8080 (the desktop reference binds an ephemeral port and is
// unaffected).  Verified on the JBL Cast setup, 2026-09-18.
#define ROCK_RELAY_PORT 47809

/**
 * Unified stream source service.
 *
 * ONE authorized connection to the RockServer station proxy feeds ONE shared
 * ring; consumers attach to the ring instead of opening their own upstream
 * connections:
 *   - speaker consumer: player_bsp decode loop via rock_source_read();
 *   - Cast consumer:    TCP clients on ROCK_RELAY_PORT via rock_relay_listen().
 *
 * Output switching (speaker <-> Chromecast) keeps the source alive: only the
 * set of consumers changes.
 *
 * The ring holds compressed audio in passthrough mode (MP3 upstreams) or
 * 48 kHz stereo PCM in transcode mode (AAC upstreams).
 *
 * Confidentiality requirement: upstream_uri and bearer must NEVER be logged
 * or persisted.
 */

/**
 * Start (or restart) the source: connect upstream, prefill the ring, spawn
 * the feeder task.  Blocks until the prefill target is reached or fails.
 *
 * @param upstream_uri  RockServer station proxy URI.
 * @param bearer        Bearer token for the upstream Authorization header;
 *                      NULL or "" for an anonymous upstream.
 * @return ESP_OK on success, or error code.
 */
esp_err_t rock_source_start(const char *upstream_uri, const char *bearer);

/**
 * Refresh the stored bearer for future upstream reconnects.
 */
esp_err_t rock_source_update_bearer(const char *bearer);

/**
 * Stop the source: feeder, all Cast clients and the upstream connection.
 * The ring stays allocated and is reset for the next rock_source_start.
 */
void rock_source_stop(void);

/**
 * Returns true while the feeder task is alive.
 */
bool rock_source_is_active(void);

/**
 * Successful reconnects of a dropped upstream since the last
 * rock_source_start (the primary connect is not counted).
 */
uint32_t rock_source_reconnect_count(void);

/**
 * Returns true when the ring holds 48 kHz stereo PCM (transcode mode);
 * false when it holds compressed audio (passthrough mode).
 */
bool rock_source_is_pcm(void);

/**
 * Content type negotiated for the current source session
 * ("audio/mpeg", "audio/wav", or the raw upstream type).
 */
const char *rock_source_content_type(void);

/**
 * Same content type as a stable enum (phase 5): the string above feeds HTTP
 * headers and logs; the enum is the cross-language single source that the
 * Rust Cast LOAD logic maps to a MIME type. MPEG = passthrough ring; WAV is
 * also what the relay serves for AAC/unknown upstreams (transcode mode).
 */
typedef enum rock_source_content_type_t {
    ROCK_SOURCE_CT_UNKNOWN = 0,
    ROCK_SOURCE_CT_MPEG,
    ROCK_SOURCE_CT_WAV,
    ROCK_SOURCE_CT_AAC,
} rock_source_content_type_t;

rock_source_content_type_t rock_source_content_type_enum(void);

/**
 * Thread-safe copy of the content-type string for FFI callers: snapshots
 * under the source config lock into the caller's buffer, so Rust never
 * borrows the mutable static behind rock_source_content_type().
 *
 * @return string length written, or -1 on a buffer/format error.
 */
int rock_source_content_type_copy(char *out, size_t cap);

/**
 * Speaker consumer tap: read the next chunk at the live edge.
 * Joins the stream behind the live edge on the first call; clamps forward
 * if the reader falls behind the ring start.
 *
 * @param out Output buffer.
 * @param cap Capacity of out.
 * @return bytes read (0 = nothing available yet), or -1 when the source
 *         is not running.
 */
int rock_source_read(uint8_t *out, size_t cap);

/**
 * Attach the Cast consumer: listen on ROCK_RELAY_PORT and serve ring data
 * to Chromecast clients (MP3 passthrough or WAV PCM).  Idempotent: when
 * already listening, returns the current URL.
 *
 * @param out_relay_url Output buffer for the LAN URL (e.g. "http://192.168.31.115:47809/stream").
 * @param out_len       Capacity of out_relay_url buffer (>= 32).
 * @return ESP_OK on success, or error code.
 */
esp_err_t rock_relay_listen(char *out_relay_url, size_t out_len);

/**
 * Stop everything: source + Cast listeners.
 * \deprecated Alias of rock_source_stop(); removal in phase 6.
 */
__attribute__((deprecated("alias of rock_source_stop(); removal in phase 6")))
void rock_relay_stop(void);

/**
 * Returns true while the source feeder is running.
 * \deprecated Alias of rock_source_is_active(); removal in phase 6.
 */
__attribute__((deprecated("alias of rock_source_is_active(); removal in phase 6")))
bool rock_relay_is_active(void);

#ifdef __cplusplus
}
#endif
