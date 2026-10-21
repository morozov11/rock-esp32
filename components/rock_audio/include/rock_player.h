#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROCK_PLAYER_STATUS_IDLE = 0,
    ROCK_PLAYER_STATUS_BUFFERING,
    ROCK_PLAYER_STATUS_PLAYING,
    ROCK_PLAYER_STATUS_PAUSED,
    ROCK_PLAYER_STATUS_STOPPED,
    ROCK_PLAYER_STATUS_ERROR
} rock_player_status_t;

// Playback states the player reports upward to the UI. Which text/screen each
// state maps to is decided by the callback owner (composition root), not here.
typedef enum {
    ROCK_PLAYER_UI_BUFFERING,
    ROCK_PLAYER_UI_STALLED,
    ROCK_PLAYER_UI_PLAYING,
    ROCK_PLAYER_UI_PAUSED,
    ROCK_PLAYER_UI_ERROR
} rock_player_ui_state_t;

typedef struct {
    void (*on_playback_state)(const char *station_id, rock_player_ui_state_t state);
} rock_player_ui_cbs_t;

int rock_player_init(void);

void rock_player_set_ui_cbs(const rock_player_ui_cbs_t *cbs);

#if CONFIG_ROCK_BRINGUP_TESTS
/**
 * RE-5 hardware bring-up and verification task (Core 1).
 */
int rock_player_bringup_start(void);
#endif

/**
 * Initiates playback of an HTTP/HTTPS audio stream (MP3 or AAC).
 * Replaces any existing active stream (stopping and freeing old stream resources first).
 * Returns 0 on successful initiation, -1 on immediate rejection (e.g. SSRF violation).
 */
int rock_player_play_stream(const char *stream_uri, const char *station_id, const char *bearer);

int rock_player_play(void);
int rock_player_pause(void);
int rock_player_stop(void);
int rock_player_stop_and_wait(uint32_t timeout_ms);

int rock_player_set_volume(uint8_t volume);
int rock_player_set_mute(bool muted);

/**
 * Returns current status string ("idle", "buffering", "playing", "paused", "stopped", "error"),
 * active station_id, volume (0..100), and mute flag.
 */
int rock_player_get_state(char *status_buf, size_t status_cap,
                          char *station_buf, size_t station_cap,
                          uint8_t *vol, bool *muted);

/**
 * Current playback status as the rock_player_status_t value itself — the
 * cross-language contract for Rust (single source of truth instead of
 * parsing the text form above).
 */
int rock_player_status_enum(void);

/**
 * Telemetry metrics: underruns, decode errors, reconnect counts.
 */
void rock_player_get_stats(uint32_t *underruns, uint32_t *decode_errors, uint32_t *net_reconnects);
void rock_player_log_stream_metrics(void);

/**
 * Non-blocking read-only peek of recent decoded PCM samples from the ring buffer
 * for audio spectrum analysis. Never blocks or modifies the ring buffer.
 */
size_t rock_player_copy_pcm_snapshot(int16_t *out_samples, size_t max_samples,
                                     uint32_t *sample_rate, uint8_t *channels);

/**
 * Broadcast (Chromecast) output status controls for audio spectrum snapshot routing.
 */
void rock_player_set_cast_active(bool active);
bool rock_player_is_cast_active(void);

#ifdef __cplusplus
}
#endif
