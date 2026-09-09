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

int rock_player_init(void);

/**
 * RE-5 hardware bring-up and verification task (Core 1).
 */
int rock_player_bringup_start(void);

/**
 * Initiates playback of an HTTP/HTTPS audio stream (MP3 or AAC).
 * Replaces any existing active stream (stopping and freeing old stream resources first).
 * Returns 0 on successful initiation, -1 on immediate rejection (e.g. SSRF violation).
 */
int rock_player_play_stream(const char *stream_uri, const char *station_id);

int rock_player_play(void);
int rock_player_pause(void);
int rock_player_stop(void);

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
 * Telemetry metrics: underruns, decode errors, reconnect counts.
 */
void rock_player_get_stats(uint32_t *underruns, uint32_t *decode_errors, uint32_t *net_reconnects);

#ifdef __cplusplus
}
#endif
