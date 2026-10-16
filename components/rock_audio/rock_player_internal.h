#pragma once

#include "rock_player.h"
#include "pcm_snapshot.h"
#include "rock_ring.h"
#include "esp_audio_dec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCM_RING_CAPACITY (2 * 1024 * 1024) // PSRAM, ~12 s at 44.1 kHz stereo
#define PREROLL_MIN_BYTES (128 * 1024)      // ~0.73 s jitter buffer at 44.1 kHz stereo 16-bit
#define HTTP_CHUNK_SIZE   4096
#define DECODE_OUT_CAP    8192

// Format gate settings for HE-AAC SBR debounce (prevent I2S DMA storm)
#define FORMAT_STABLE_FRAMES 3
#define FORMAT_RECONFIG_MIN_INTERVAL_MS 1000
#define FORMAT_INITIAL_GRACE_MS 250

typedef struct {
    uint32_t pending_fs;
    uint8_t pending_ch;
    uint8_t stable_count;
    int64_t decode_started_us;
    int64_t last_reconfig_us;
} rock_format_gate_t;

// Ring buffer & Snapshot tap accessors
rock_pcm_ring_t *rock_player_get_pcm_ring(void);
rock_pcm_snapshot_t *rock_player_get_pcm_tap(void);

// Audio format accessors (atomic)
void rock_player_set_audio_format(uint32_t rate, uint8_t channels);
void rock_player_get_audio_format(uint32_t *rate, uint8_t *channels);

// State machine accessors & helpers
void rock_player_set_status(rock_player_status_t status);
rock_player_status_t rock_player_get_status(void);
void rock_player_notify_ui(const char *station_id, rock_player_ui_state_t state);
void rock_player_get_station(char *buf, size_t cap);
void rock_player_set_station(const char *station_id);
bool rock_player_check_preroll(const char *station_id);

// Volume, Mute & Power Amplifier
bool rock_player_is_muted(void);
void rock_player_pa_set(bool enable);
void rock_player_pa_update(void);

// Stream generation and cancellation
uint32_t rock_player_get_generation(void);
uint32_t rock_player_next_generation(void);
bool rock_player_is_stop_signaled(void);
void rock_player_set_stop_signal(bool stop);

// Telemetry counters
void rock_player_stat_inc_underruns(void);
uint32_t rock_player_get_underruns(void);
void rock_player_stat_inc_decode_errors(void);
void rock_player_stat_add_net_bytes(uint32_t bytes);
void rock_player_stat_add_pcm_bytes(uint32_t bytes);
void rock_player_stat_inc_zero_reads(void);
void rock_player_stat_reset_stream(void);

// Subsystem initializers & controllers
int rock_player_tx_init(void);

int rock_player_rx_start(const char *station_id);
int rock_player_rx_stop_and_wait(uint32_t timeout_ms);
bool rock_player_rx_is_running(void);

// Audio Decoder & Format Adaptation
esp_audio_type_t rock_decoder_detect_type(const char *content_type);
int rock_decoder_open(esp_audio_type_t type, esp_audio_dec_handle_t *out_dec);
void rock_decoder_close(esp_audio_dec_handle_t dec);
void rock_format_gate_init(rock_format_gate_t *gate);
int rock_decoder_step(esp_audio_dec_handle_t dec,
                      const char *station_id,
                      uint8_t *in_buf, size_t in_len,
                      uint8_t *out_buf,
                      uint32_t *current_fs, uint8_t *current_channels,
                      rock_format_gate_t *gate);
