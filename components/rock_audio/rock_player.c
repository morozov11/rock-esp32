#include "rock_player.h"
#include "rock_player_internal.h"
#include "audio_bsp.h"
#include "rock_source.h"
#include "rock_ring.h"
#include "pcm_snapshot.h"
#include "rock_ffi_exports.h"

#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-player";

// ---------------------------------------------------------------------------
// Thread-safe PSRAM PCM ring & snapshot tap (read-only for spectrum analysis)
// ---------------------------------------------------------------------------
static rock_pcm_ring_t s_pcm_ring;
static rock_pcm_snapshot_t s_pcm_tap;
static atomic_uint s_pcm_rate = 44100;
static atomic_uint s_pcm_channels = 2;
static atomic_bool s_spectrum_playing = false;

rock_pcm_ring_t *rock_player_get_pcm_ring(void)
{
    return &s_pcm_ring;
}

rock_pcm_snapshot_t *rock_player_get_pcm_tap(void)
{
    return &s_pcm_tap;
}

void rock_player_set_audio_format(uint32_t rate, uint8_t channels)
{
    atomic_store(&s_pcm_rate, rate);
    atomic_store(&s_pcm_channels, channels);
}

void rock_player_get_audio_format(uint32_t *rate, uint8_t *channels)
{
    if (rate) *rate = atomic_load(&s_pcm_rate);
    if (channels) *channels = (uint8_t)atomic_load(&s_pcm_channels);
}

size_t rock_player_copy_pcm_snapshot(int16_t *out_samples, size_t max_samples,
                                     uint32_t *sample_rate, uint8_t *channels)
{
    if (!out_samples || !sample_rate || !channels || !atomic_load(&s_spectrum_playing)) return 0;
    return rock_pcm_copy(&s_pcm_tap, out_samples, max_samples, sample_rate, channels);
}

// ---------------------------------------------------------------------------
// Player State, Volume, Mute & Concurrency Controls
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_state_mutex;
static rock_player_status_t s_status = ROCK_PLAYER_STATUS_IDLE;
static char s_station_id[130];
static uint8_t s_volume = 35;
static atomic_bool s_muted = false;

static atomic_uint_fast32_t s_stream_generation = 0;
static atomic_bool s_stop_signal = false;

static atomic_uint_fast32_t s_stat_underruns = 0;
static atomic_uint_fast32_t s_stat_decode_errors = 0;
static atomic_uint_fast32_t s_stat_net_bytes = 0;
static atomic_uint_fast32_t s_stat_pcm_bytes = 0;
static atomic_uint_fast32_t s_stat_zero_reads = 0;

// Preroll hysteresis state (TX and RX tasks; benign 32-bit races only move
// the target between power-of-two steps).
static uint32_t s_preroll_target = PREROLL_MIN_BYTES;
static int64_t s_preroll_play_start_us = 0;

static rock_player_ui_cbs_t s_ui_cbs;

void rock_player_set_ui_cbs(const rock_player_ui_cbs_t *cbs)
{
    s_ui_cbs = cbs ? *cbs : (rock_player_ui_cbs_t){0};
}

void rock_player_notify_ui(const char *station_id, rock_player_ui_state_t state)
{
    if (!s_ui_cbs.on_playback_state) return;
    char buf[sizeof(s_station_id)];
    if (!station_id) {
        rock_player_get_station(buf, sizeof(buf));
        station_id = buf;
    }
    s_ui_cbs.on_playback_state(station_id, state);
}

void rock_player_set_status(rock_player_status_t status)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_status = status;
    atomic_store(&s_spectrum_playing, status == ROCK_PLAYER_STATUS_PLAYING);
    xSemaphoreGive(s_state_mutex);
}

rock_player_status_t rock_player_get_status(void)
{
    if (!s_state_mutex) return ROCK_PLAYER_STATUS_IDLE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    rock_player_status_t st = s_status;
    xSemaphoreGive(s_state_mutex);
    return st;
}

void rock_player_get_station(char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(buf, s_station_id, cap);
    xSemaphoreGive(s_state_mutex);
}

void rock_player_set_station(const char *station_id)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(s_station_id, station_id ? station_id : "", sizeof(s_station_id));
    xSemaphoreGive(s_state_mutex);
}

bool rock_player_check_preroll(const char *station_id)
{
    if (rock_player_get_status() == ROCK_PLAYER_STATUS_BUFFERING &&
        rock_pcm_ring_fill(&s_pcm_ring) >= s_preroll_target) {
        rock_player_set_status(ROCK_PLAYER_STATUS_PLAYING);
        rock_player_notify_ui(station_id, ROCK_PLAYER_UI_PLAYING);
        rock_player_pa_set(true);
        s_preroll_play_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Pre-roll reached (%u bytes); playback started", (unsigned)s_preroll_target);
        return true;
    }
    return false;
}

// Underrun hysteresis: each consecutive underrun doubles the preroll target
// (up to PREROLL_MAX_BYTES) so a stream delivered below realtime yields
// longer stable stretches instead of flap-stall cycles every few seconds.
// Ten seconds of uninterrupted playback resets the target to the base value.
uint32_t rock_player_note_underrun(void)
{
    if (s_preroll_target < PREROLL_MAX_BYTES) {
        s_preroll_target *= 2;
        if (s_preroll_target > PREROLL_MAX_BYTES) {
            s_preroll_target = PREROLL_MAX_BYTES;
        }
    }
    s_preroll_play_start_us = esp_timer_get_time();
    rock_player_stat_inc_underruns();
    return s_preroll_target;
}

void rock_player_note_playback_stable(void)
{
    if (s_preroll_target == PREROLL_MIN_BYTES) {
        return;
    }
    if (esp_timer_get_time() - s_preroll_play_start_us >= PREROLL_STABLE_RESET_US) {
        s_preroll_target = PREROLL_MIN_BYTES;
        ESP_LOGI(TAG, "Playback stable; preroll target reset to %u bytes",
            (unsigned)s_preroll_target);
    }
}

bool rock_player_is_muted(void)
{
    return atomic_load(&s_muted);
}

void rock_player_pa_set(bool enable)
{
    if (enable && !atomic_load(&s_muted)) {
        rock_audio_pa_set(true);
    } else {
        rock_audio_pa_set(false);
    }
}

void rock_player_pa_update(void)
{
    rock_player_pa_set(rock_player_get_status() == ROCK_PLAYER_STATUS_PLAYING);
}

uint32_t rock_player_get_generation(void)
{
    return (uint32_t)atomic_load(&s_stream_generation);
}

uint32_t rock_player_next_generation(void)
{
    return (uint32_t)(atomic_fetch_add(&s_stream_generation, 1) + 1);
}

bool rock_player_is_stop_signaled(void)
{
    return atomic_load(&s_stop_signal);
}

void rock_player_set_stop_signal(bool stop)
{
    atomic_store(&s_stop_signal, stop);
}

void rock_player_stat_inc_underruns(void)
{
    atomic_fetch_add(&s_stat_underruns, 1);
}

uint32_t rock_player_get_underruns(void)
{
    return (uint32_t)atomic_load(&s_stat_underruns);
}

void rock_player_stat_inc_decode_errors(void)
{
    atomic_fetch_add(&s_stat_decode_errors, 1);
}

void rock_player_stat_add_net_bytes(uint32_t bytes)
{
    atomic_fetch_add(&s_stat_net_bytes, bytes);
}

void rock_player_stat_add_pcm_bytes(uint32_t bytes)
{
    atomic_fetch_add(&s_stat_pcm_bytes, bytes);
}

void rock_player_stat_inc_zero_reads(void)
{
    atomic_fetch_add(&s_stat_zero_reads, 1);
}

void rock_player_stat_reset_stream(void)
{
    atomic_store(&s_stat_net_bytes, 0);
    atomic_store(&s_stat_pcm_bytes, 0);
    atomic_store(&s_stat_zero_reads, 0);
    // New stream: fresh hysteresis. The base preroll starts the stream fast;
    // it only grows if this stream actually underruns.
    s_preroll_target = PREROLL_MIN_BYTES;
    s_preroll_play_start_us = 0;
}

// ---------------------------------------------------------------------------
// Public Player API
// ---------------------------------------------------------------------------
int rock_player_init(void)
{
    if (s_state_mutex) return 0;
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) return -1;

    // Initialize low-level audio hardware (I2S, ES8311, PA)
    if (rock_audio_init() != 0) {
        ESP_LOGE(TAG, "Audio hardware initialization failed");
        goto fail;
    }
    // Reserve stereo DMA buffers before TLS and decoder allocations fragment internal RAM.
    if (rock_audio_reconfig(44100, 2) != 0) {
        ESP_LOGE(TAG, "Audio output initialization at 44.1 kHz stereo failed");
        goto fail;
    }

    // Register official esp_audio_codec decoders
    esp_mp3_dec_register();
    esp_aac_dec_register();

    // Allocate PSRAM PCM ring buffer
    if (rock_pcm_ring_init(&s_pcm_ring, PCM_RING_CAPACITY) != 0) {
        ESP_LOGE(TAG, "PCM ring buffer initialization failed");
        goto fail;
    }

    // Set initial volume & mute
    esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
    if (codec) {
        esp_codec_dev_set_out_vol(codec, s_volume);
        esp_codec_dev_set_out_mute(codec, atomic_load(&s_muted));
    }
    rock_audio_pa_set(false);

    // Create audio TX worker pinned to Core 1
    if (rock_player_tx_init() != 0) {
        goto fail;
    }

    ESP_LOGI(TAG, "Player initialized: PSRAM ring %u KiB, PA GPIO11, decoders: MP3+AAC",
             PCM_RING_CAPACITY / 1024);
    return 0;

fail:
    // Full teardown: leaving s_state_mutex alive would make every retry of
    // rock_player_init "succeed" with a half-initialized player.
    vSemaphoreDelete(s_state_mutex);
    s_state_mutex = NULL;
    return -1;
}

int rock_player_play_stream(const char *stream_uri, const char *station_id, const char *bearer)
{
    if (!stream_uri || !station_id || !bearer || !*bearer || strpbrk(bearer, "\r\n")) return -1;

    // SSRF gate on admission
    if (!rock_validate_stream_uri(stream_uri)) {
        ESP_LOGE(TAG, "stream_uri rejected by SSRF gate");
        return -1;
    }

    return rock_player_rx_start(station_id);
}

// Resume/pause return -1 when the command does not apply to the current
// status so callers can distinguish "nothing to resume" from success; a
// stopped player must be restarted via rock_player_play_stream.
int rock_player_play(void)
{
    rock_player_status_t st = rock_player_get_status();
    if (st != ROCK_PLAYER_STATUS_PAUSED) {
        return -1;
    }
    rock_player_set_status(ROCK_PLAYER_STATUS_PLAYING);
    rock_player_pa_set(true);
    rock_player_notify_ui(NULL, ROCK_PLAYER_UI_PLAYING);
    return 0;
}

int rock_player_pause(void)
{
    rock_player_status_t st = rock_player_get_status();
    if (st != ROCK_PLAYER_STATUS_PLAYING && st != ROCK_PLAYER_STATUS_BUFFERING) {
        return -1;
    }
    rock_player_set_status(ROCK_PLAYER_STATUS_PAUSED);
    rock_player_pa_set(false);
    rock_player_notify_ui(NULL, ROCK_PLAYER_UI_PAUSED);
    return 0;
}

int rock_player_stop(void)
{
    rock_player_set_stop_signal(true);
    rock_player_next_generation();
    rock_pcm_ring_clear(&s_pcm_ring);
    rock_player_pa_set(false);
    rock_player_set_status(ROCK_PLAYER_STATUS_STOPPED);
    return 0;
}

int rock_player_stop_and_wait(uint32_t timeout_ms)
{
    rock_player_stop();
    return rock_player_rx_stop_and_wait(timeout_ms);
}

int rock_player_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_volume = volume;
    xSemaphoreGive(s_state_mutex);

    esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
    if (codec) {
        esp_codec_dev_set_out_vol(codec, volume);
    }
    return 0;
}

int rock_player_set_mute(bool muted)
{
    atomic_store(&s_muted, muted);

    esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
    if (codec) {
        esp_codec_dev_set_out_mute(codec, muted);
    }
    rock_player_pa_update();
    return 0;
}

int rock_player_get_state(char *status_buf, size_t status_cap,
                          char *station_buf, size_t station_cap,
                          uint8_t *vol, bool *muted)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (status_buf && status_cap > 0) {
        const char *st_str = "idle";
        switch (s_status) {
            case ROCK_PLAYER_STATUS_IDLE: st_str = "idle"; break;
            case ROCK_PLAYER_STATUS_BUFFERING: st_str = "buffering"; break;
            case ROCK_PLAYER_STATUS_PLAYING: st_str = "playing"; break;
            case ROCK_PLAYER_STATUS_PAUSED: st_str = "paused"; break;
            case ROCK_PLAYER_STATUS_STOPPED: st_str = "stopped"; break;
            case ROCK_PLAYER_STATUS_ERROR: st_str = "error"; break;
        }
        strlcpy(status_buf, st_str, status_cap);
    }
    if (station_buf && station_cap > 0) {
        strlcpy(station_buf, s_station_id, station_cap);
    }
    if (vol) *vol = s_volume;
    if (muted) *muted = atomic_load(&s_muted);
    xSemaphoreGive(s_state_mutex);
    return 0;
}

int rock_player_status_enum(void)
{
    return (int)rock_player_get_status();
}

void rock_player_get_stats(uint32_t *underruns, uint32_t *decode_errors, uint32_t *net_reconnects)
{
    if (underruns) *underruns = (uint32_t)atomic_load(&s_stat_underruns);
    if (decode_errors) *decode_errors = (uint32_t)atomic_load(&s_stat_decode_errors);
    if (net_reconnects) *net_reconnects = rock_source_reconnect_count();
}

void rock_player_log_stream_metrics(void)
{
    ESP_LOGI(TAG, "stream metrics net=%lu pcm=%lu ring=%u zero_reads=%lu dma_largest=%u",
             (unsigned long)atomic_load(&s_stat_net_bytes),
             (unsigned long)atomic_load(&s_stat_pcm_bytes),
             (unsigned)rock_pcm_ring_fill(&s_pcm_ring),
             (unsigned long)atomic_load(&s_stat_zero_reads),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}
