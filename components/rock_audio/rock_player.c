#include "rock_player.h"
#include "pcm_snapshot.h"
#include "audio_bsp.h"
#include "rock_source.h"
#include "rock_ring.h"
#include "rock_ffi_exports.h"
#if CONFIG_ROCK_BRINGUP_TESTS
#include "test_mp3.h"
#endif

#include <assert.h>
#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "rock-player";

#define PCM_RING_CAPACITY (2 * 1024 * 1024) // PSRAM, ~12 s at 44.1 kHz stereo
#define PREROLL_MIN_BYTES (512 * 1024)      // ~3 s to absorb relay and Wi-Fi jitter
#define HTTP_CHUNK_SIZE   4096
#define DECODE_OUT_CAP    8192

// ---------------------------------------------------------------------------
// Thread-safe PSRAM PCM ring: implementation in rock_ring.c (phase 4); the
// player only owns the tap snapshot published on every speaker read.
// ---------------------------------------------------------------------------
static rock_pcm_ring_t s_pcm_ring;
static rock_pcm_snapshot_t s_pcm_tap;
static atomic_uint s_pcm_rate = 44100;
static atomic_uint s_pcm_channels = 2;
static atomic_bool s_spectrum_playing;

size_t rock_player_copy_pcm_snapshot(int16_t *out_samples, size_t max_samples,
                                     uint32_t *sample_rate, uint8_t *channels)
{
    if (!out_samples || !sample_rate || !channels || !atomic_load(&s_spectrum_playing)) return 0;
    return rock_pcm_copy(&s_pcm_tap, out_samples, max_samples, sample_rate, channels);
}

// ---------------------------------------------------------------------------
// Player State & Control
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_state_mutex;
static rock_player_status_t s_status = ROCK_PLAYER_STATUS_IDLE;
static char s_station_id[130];
static uint8_t s_volume = 35;
// Atomic: read by the audio TX task without s_state_mutex (B5 fix).
static atomic_bool s_muted = false;

static atomic_uint_fast32_t s_stream_generation = 0;
static atomic_bool s_stop_signal = false;

static atomic_uint_fast32_t s_stat_underruns = 0;
static atomic_uint_fast32_t s_stat_decode_errors = 0;
static atomic_uint_fast32_t s_stat_net_bytes = 0;
static atomic_uint_fast32_t s_stat_pcm_bytes = 0;
static atomic_uint_fast32_t s_stat_zero_reads = 0;

static TaskHandle_t s_rx_task = NULL;

// UI callbacks registered by the composition root; the player never calls
// rock_ui_* directly (dependency inversion, phase 3).
static rock_player_ui_cbs_t s_ui_cbs;

void rock_player_set_ui_cbs(const rock_player_ui_cbs_t *cbs)
{
    s_ui_cbs = cbs ? *cbs : (rock_player_ui_cbs_t){0};
}

static void notify_ui(const char *station_id, rock_player_ui_state_t state)
{
    if (s_ui_cbs.on_playback_state) {
        s_ui_cbs.on_playback_state(station_id, state);
    }
}

static void set_player_status(rock_player_status_t status)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_status = status;
    atomic_store(&s_spectrum_playing, status == ROCK_PLAYER_STATUS_PLAYING);
    xSemaphoreGive(s_state_mutex);
}

static rock_player_status_t get_player_status(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    rock_player_status_t st = s_status;
    xSemaphoreGive(s_state_mutex);
    return st;
}

// ---------------------------------------------------------------------------
// Audio Output Task (CPU 1, Priority 6)
// ---------------------------------------------------------------------------
static void audio_tx_task(void *arg)
{
    (void)arg;
    static uint8_t pcm_chunk[1024];

    while (1) {
        rock_player_status_t st = get_player_status();

        if (st == ROCK_PLAYER_STATUS_PLAYING) {
            size_t n = rock_pcm_ring_read(&s_pcm_ring, pcm_chunk, sizeof(pcm_chunk));
            rock_pcm_publish(&s_pcm_tap, pcm_chunk, n, atomic_load(&s_pcm_rate), atomic_load(&s_pcm_channels));
            if (n > 0) {
                esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
                if (codec) {
                    esp_codec_dev_write(codec, pcm_chunk, (int)n);
                    if (!atomic_load(&s_muted)) {
                        rock_audio_pa_set(true);
                    }
                }
            } else {
                // Pause output until the relay has refilled enough PCM to play smoothly.
                atomic_fetch_add(&s_stat_underruns, 1);
                set_player_status(ROCK_PLAYER_STATUS_BUFFERING);
                rock_audio_pa_set(false);
                notify_ui(s_station_id, ROCK_PLAYER_UI_STALLED);
                ESP_LOGW(TAG, "PCM ring underrun (total: %u)", (unsigned)atomic_load(&s_stat_underruns));
            }
        } else if (st == ROCK_PLAYER_STATUS_PAUSED) {
            rock_audio_pa_set(false);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            // STOPPED / IDLE / ERROR
            rock_audio_pa_set(false);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// One decode pass over the staged compressed input (phase-4 extraction from
// stream_rx_task): run esp_audio_dec frames, reconfigure the codec on format
// changes, push PCM into the speaker ring and flip BUFFERING→PLAYING once
// the pre-roll threshold is reached.  Decoder errors skip a byte to
// resynchronize.  Returns consumed bytes, or -1 when a codec reconfig failed
// and the stream must be dropped.
static int rock_decoder_step(esp_audio_dec_handle_t dec,
                             const char *station_id,
                             uint8_t *in_buf, size_t in_len,
                             uint8_t *out_buf,
                             uint32_t *current_fs, uint8_t *current_channels)
{
    size_t in_pos = 0;
    while (in_len > in_pos && !atomic_load(&s_stop_signal)) {
        esp_audio_dec_in_raw_t raw = {
            .buffer = in_buf + in_pos,
            .len = (uint32_t)(in_len - in_pos),
            .consumed = 0,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = out_buf,
            .len = DECODE_OUT_CAP,
            .needed_size = 0,
            .decoded_size = 0,
        };

        esp_audio_err_t dret = esp_audio_dec_process(dec, &raw, &out_frame);
        if (dret == ESP_AUDIO_ERR_OK || dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            in_pos += raw.consumed;
            if (out_frame.decoded_size > 0) {
                // Check sample info dynamically
                esp_audio_dec_info_t info;
                if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    if (info.sample_rate != *current_fs || info.channel != *current_channels) {
                        ESP_LOGI(TAG, "Decoder format: %lu Hz, %u ch (reconfiguring codec)",
                                 (unsigned long)info.sample_rate, info.channel);
                        if (rock_audio_reconfig(info.sample_rate, info.channel) != 0) {
                            ESP_LOGE(TAG, "rock_audio_reconfig failed, aborting local decode");
                            return -1;
                        }
                        atomic_store(&s_pcm_rate, info.sample_rate);
                        atomic_store(&s_pcm_channels, info.channel);
                        *current_fs = info.sample_rate;
                        *current_channels = info.channel;
                    }
                }

                atomic_fetch_add(&s_stat_pcm_bytes, (uint32_t)rock_pcm_ring_write(&s_pcm_ring, out_buf, out_frame.decoded_size));

                // Pre-roll check
                if (get_player_status() == ROCK_PLAYER_STATUS_BUFFERING &&
                    rock_pcm_ring_fill(&s_pcm_ring) >= PREROLL_MIN_BYTES) {
                    set_player_status(ROCK_PLAYER_STATUS_PLAYING);
                    notify_ui(station_id, ROCK_PLAYER_UI_PLAYING);
                    if (!atomic_load(&s_muted)) {
                        rock_audio_pa_set(true);
                    }
                    ESP_LOGI(TAG, "Pre-roll reached (%u bytes); playback started", (unsigned)PREROLL_MIN_BYTES);
                }
            }
        } else if (dret == ESP_AUDIO_ERR_DATA_LACK) {
            // Need more data from the source
            break;
        } else {
            atomic_fetch_add(&s_stat_decode_errors, 1);
            in_pos++; // skip byte to resynchronize
        }
    }
    return (int)in_pos;
}

// ---------------------------------------------------------------------------
// Source Tap & Decode Worker (CPU 1, Priority 5)
//
// The unified stream source (relay_bsp) owns the single authorized RockServer
// connection; this task is the speaker consumer: it reads the shared ring and
// feeds the local decoder / PCM ring.  Cast clients read the same ring over
// the relay listener, so switching outputs never reopens the upstream.
// ---------------------------------------------------------------------------
static void stream_rx_task(void *arg)
{
    (void)arg;
    uint32_t current_gen = atomic_load(&s_stream_generation);
    esp_audio_dec_handle_t dec = NULL;
    // Per-generation heap buffers: rock_player_play_stream joins the previous
    // task for only ~600 ms, so two generations can overlap and must never
    // share static buffers here.  Freed on every exit path (fail/cleanup).
    uint8_t *in_buf = malloc(HTTP_CHUNK_SIZE * 2);
    uint8_t *out_buf = malloc(DECODE_OUT_CAP);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    char current_station[130];
    strlcpy(current_station, s_station_id, sizeof(current_station));
    xSemaphoreGive(s_state_mutex);

    set_player_status(ROCK_PLAYER_STATUS_BUFFERING);
    notify_ui(current_station, ROCK_PLAYER_UI_BUFFERING);

    if (!in_buf || !out_buf) {
        ESP_LOGE(TAG, "Failed to allocate stream RX buffers");
        goto fail;
    }

    if (!rock_source_is_active()) {
        ESP_LOGE(TAG, "Stream source is not running");
        goto fail;
    }

    // PCM mode: the ring already holds 48 kHz stereo PCM (transcoded by the
    // source for Cast); the speaker consumes it directly without a decoder.
    // Compressed mode: decode locally like before, just from the ring.
    bool pcm_mode = rock_source_is_pcm();

    if (!pcm_mode) {
        const char *content_type = rock_source_content_type();
        esp_audio_type_t codec_type = ESP_AUDIO_TYPE_UNSUPPORT;
        ESP_LOGI(TAG, "Source content-type: %s", content_type);
        if (strcasestr(content_type, "audio/mpeg") || strcasestr(content_type, "audio/mp3")
            || strcasestr(content_type, "audio/x-mpeg")
            || strcasestr(content_type, "application/octet-stream")) {
            codec_type = ESP_AUDIO_TYPE_MP3;
        } else if (strcasestr(content_type, "audio/aac") || strcasestr(content_type, "audio/aacp")
            || strcasestr(content_type, "audio/x-aac") || strcasestr(content_type, "audio/mp4")) {
            codec_type = ESP_AUDIO_TYPE_AAC;
        } else {
            ESP_LOGE(TAG, "Unsupported Content-Type (not MP3/AAC): %s", content_type);
            goto fail;
        }
        esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        esp_audio_dec_cfg_t dec_cfg = {
            .type = codec_type,
            .cfg = NULL,
            .cfg_sz = 0,
        };
        if (codec_type == ESP_AUDIO_TYPE_AAC) {
            // HE-AAC (aacPlus) radio streams decode only with SBR/PS enabled.
            aac_cfg.aac_plus_enable = true;
            dec_cfg.cfg = &aac_cfg;
            dec_cfg.cfg_sz = sizeof(aac_cfg);
        }
        if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open audio decoder for type %d", codec_type);
            goto fail;
        }
    }

    size_t in_len = 0;
    uint32_t current_fs = 0;
    uint8_t current_channels = 0;

    if (pcm_mode) {
        current_fs = 48000;
        current_channels = 2;
        if (rock_audio_reconfig(current_fs, current_channels) != 0) {
            ESP_LOGE(TAG, "Audio reconfig to 48 kHz stereo failed");
            goto fail;
        }
        atomic_store(&s_pcm_rate, current_fs);
        atomic_store(&s_pcm_channels, current_channels);
    }

    while (!atomic_load(&s_stop_signal) && atomic_load(&s_stream_generation) == current_gen) {
        // Flow control: wait if PCM ring buffer is getting full (> 85%)
        if (rock_pcm_ring_fill(&s_pcm_ring) > (PCM_RING_CAPACITY * 85 / 100)) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        int space = (int)(HTTP_CHUNK_SIZE * 2 - in_len);
        if (!pcm_mode && space > HTTP_CHUNK_SIZE) space = HTTP_CHUNK_SIZE;
        int rlen = rock_source_read(in_buf + in_len, (size_t)space);
        if (rlen < 0) {
            ESP_LOGW(TAG, "Stream source ended (%d)", rlen);
            goto fail;
        }
        if (rlen == 0) {
            atomic_fetch_add(&s_stat_zero_reads, 1);
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        atomic_fetch_add(&s_stat_net_bytes, (uint32_t)rlen);
        in_len += (size_t)rlen;

        if (pcm_mode) {
            atomic_fetch_add(&s_stat_pcm_bytes, (uint32_t)rock_pcm_ring_write(&s_pcm_ring, in_buf, in_len));
            in_len = 0;
            if (get_player_status() == ROCK_PLAYER_STATUS_BUFFERING
                && rock_pcm_ring_fill(&s_pcm_ring) >= PREROLL_MIN_BYTES) {
                set_player_status(ROCK_PLAYER_STATUS_PLAYING);
                notify_ui(current_station, ROCK_PLAYER_UI_PLAYING);
                if (!atomic_load(&s_muted)) {
                    rock_audio_pa_set(true);
                }
                ESP_LOGI(TAG, "Pre-roll reached (%u bytes); playback started",
                    (unsigned)rock_pcm_ring_fill(&s_pcm_ring));
            }
            continue;
        }

        // Decode loop
        int consumed = rock_decoder_step(dec, current_station, in_buf, in_len, out_buf,
                                         &current_fs, &current_channels);
        if (consumed < 0) {
            goto fail;
        }
        size_t in_pos = (size_t)consumed;

        // Shift unconsumed bytes to beginning
        if (in_pos < in_len) {
            memmove(in_buf, in_buf + in_pos, in_len - in_pos);
            in_len -= in_pos;
        } else {
            in_len = 0;
        }
    }

    goto cleanup;

fail:
    // A concurrent stop (or a newer play_stream generation) may have torn the
    // source down and already installed its own status: never overwrite that
    // with ERROR, the stream did not fail on its own.
    if (!atomic_load(&s_stop_signal) && atomic_load(&s_stream_generation) == current_gen) {
        set_player_status(ROCK_PLAYER_STATUS_ERROR);
        notify_ui(current_station, ROCK_PLAYER_UI_ERROR);
    }
    rock_audio_pa_set(false);

cleanup:
    if (dec) {
        esp_audio_dec_close(dec);
        dec = NULL;
    }
    free(out_buf);
    free(in_buf);
    s_rx_task = NULL;
    vTaskDelete(NULL);
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
    if (xTaskCreatePinnedToCore(audio_tx_task, "rock_au_tx", 4096, NULL, 6, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio TX worker");
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

    // Terminate any existing streaming task and discard its output
    atomic_store(&s_stop_signal, true);
    atomic_fetch_add(&s_stream_generation, 1);

    // Wait briefly for previous task to finish
    int wait_cycles = 0;
    while (s_rx_task != NULL && ++wait_cycles < 30) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    rock_pcm_ring_clear(&s_pcm_ring);
    rock_audio_pa_set(false);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(s_station_id, station_id, sizeof(s_station_id));
    xSemaphoreGive(s_state_mutex);

    atomic_store(&s_stat_net_bytes, 0);
    atomic_store(&s_stat_pcm_bytes, 0);
    atomic_store(&s_stat_zero_reads, 0);

    atomic_store(&s_stop_signal, false);

    // Spawn stream RX worker on Core 1
    if (xTaskCreatePinnedToCore(stream_rx_task, "rock_st_rx", 8192, NULL, 5, &s_rx_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start stream RX task");
        set_player_status(ROCK_PLAYER_STATUS_ERROR);
        return -1;
    }

    return 0;
}

// Resume/pause return -1 when the command does not apply to the current
// status so callers can distinguish "nothing to resume" from success; a
// stopped player must be restarted via rock_player_play_stream.
int rock_player_play(void)
{
    rock_player_status_t st = get_player_status();
    if (st != ROCK_PLAYER_STATUS_PAUSED) {
        return -1;
    }
    set_player_status(ROCK_PLAYER_STATUS_PLAYING);
    if (!atomic_load(&s_muted)) rock_audio_pa_set(true);
    notify_ui(s_station_id, ROCK_PLAYER_UI_PLAYING);
    return 0;
}

int rock_player_pause(void)
{
    rock_player_status_t st = get_player_status();
    if (st != ROCK_PLAYER_STATUS_PLAYING && st != ROCK_PLAYER_STATUS_BUFFERING) {
        return -1;
    }
    set_player_status(ROCK_PLAYER_STATUS_PAUSED);
    rock_audio_pa_set(false);
    notify_ui(s_station_id, ROCK_PLAYER_UI_PAUSED);
    return 0;
}

int rock_player_stop(void)
{
    atomic_store(&s_stop_signal, true);
    atomic_fetch_add(&s_stream_generation, 1);
    rock_pcm_ring_clear(&s_pcm_ring);
    rock_audio_pa_set(false);
    set_player_status(ROCK_PLAYER_STATUS_STOPPED);
    return 0;
}

int rock_player_stop_and_wait(uint32_t timeout_ms)
{
    rock_player_stop();
    uint32_t waited_ms = 0;
    while (s_rx_task != NULL && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    return s_rx_task == NULL ? 0 : -1;
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
    if (muted) {
        rock_audio_pa_set(false);
    } else if (get_player_status() == ROCK_PLAYER_STATUS_PLAYING) {
        rock_audio_pa_set(true);
    }
    return 0;
}

int rock_player_get_state(char *status_buf, size_t status_cap,
                          char *station_buf, size_t station_cap,
                          uint8_t *vol, bool *muted)
{
    if (!status_buf || status_cap == 0) return -1;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
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
    return (int)get_player_status();
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

// ---------------------------------------------------------------------------
// Hardware Bring-up & Verification Task (Pinned to Core 1)
// ---------------------------------------------------------------------------
#if CONFIG_ROCK_BRINGUP_TESTS
static void player_bringup_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "RE-5 PLAYER HARDWARE VERIFICATION TASK STARTED");
    ESP_LOGI(TAG, "==================================================");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 1. SSRF Gate Verification
    ESP_LOGI(TAG, "[TEST 1/5] SSRF Gate validation...");
    assert(!rock_validate_stream_uri("http://127.0.0.1:8000/stream.mp3"));
    assert(!rock_validate_stream_uri("http://localhost:8000/stream.mp3"));
    assert(!rock_validate_stream_uri("http://10.0.0.1/stream.mp3"));
    assert(!rock_validate_stream_uri("http://192.168.1.100/stream.mp3"));
    assert(!rock_validate_stream_uri("http://169.254.169.254/latest/meta-data"));
    assert(!rock_validate_stream_uri("ftp://example.com/stream.mp3"));
    assert(rock_validate_stream_uri("https://stream.rockcast.live/classic.mp3"));
    assert(rock_validate_stream_uri("http://stream.example.org:8000/live.aac"));
    ESP_LOGI(TAG, "[TEST 1/5] SSRF Gate PASS (all 8 vectors verified)");

    // 2. Decoder & Audio Path verification
    ESP_LOGI(TAG, "[TEST 2/5] MP3 Decoder & Pipeline initialization...");
    set_player_status(ROCK_PLAYER_STATUS_BUFFERING);
    notify_ui("bringup-station", ROCK_PLAYER_UI_BUFFERING);

    esp_audio_dec_handle_t dec = NULL;
    esp_audio_dec_cfg_t dec_cfg = {
        .type = ESP_AUDIO_TYPE_MP3,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder");
        vTaskDelete(NULL);
        return;
    }

    static uint8_t out_buf[DECODE_OUT_CAP];
    size_t in_pos = 0;
    uint32_t current_fs = 16000;
    uint8_t current_channels = 1;
    bool playback_started = false;

    // Decode until pre-roll is reached and playback starts.  The embedded
    // clip decodes to only ~230 KB of PCM, under the 512 KB pre-roll
    // threshold, so loop the input like the soak below instead of a single
    // pass (a single pass never starts playback and trips the TEST 3
    // pause/play asserts).
    while (!playback_started) {
        if (in_pos >= s_test_mp3_len) in_pos = 0;
        esp_audio_dec_in_raw_t raw = {
            .buffer = (uint8_t *)(s_test_mp3_data + in_pos),
            .len = (uint32_t)(s_test_mp3_len - in_pos),
            .consumed = 0,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = out_buf,
            .len = sizeof(out_buf),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t dret = esp_audio_dec_process(dec, &raw, &out_frame);
        if (dret == ESP_AUDIO_ERR_OK || dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            in_pos += raw.consumed;
            if (out_frame.decoded_size > 0) {
                esp_audio_dec_info_t info;
                if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    if (info.sample_rate != current_fs || info.channel != current_channels) {
                        ESP_LOGI(TAG, "[TEST 2/5] Decoded stream format: %lu Hz, %u ch (reconfiguring codec)",
                                 (unsigned long)info.sample_rate, info.channel);
                        rock_audio_reconfig(info.sample_rate, info.channel);
                            atomic_store(&s_pcm_rate, info.sample_rate);
                            atomic_store(&s_pcm_channels, info.channel);
                        current_fs = info.sample_rate;
                        current_channels = info.channel;
                    }
                }
                rock_pcm_ring_write(&s_pcm_ring, out_buf, out_frame.decoded_size);
                if (!playback_started && rock_pcm_ring_fill(&s_pcm_ring) >= PREROLL_MIN_BYTES) {
                    playback_started = true;
                    set_player_status(ROCK_PLAYER_STATUS_PLAYING);
                    notify_ui("bringup-station", ROCK_PLAYER_UI_PLAYING);
                    if (!atomic_load(&s_muted)) rock_audio_pa_set(true);
                    ESP_LOGI(TAG, "[TEST 2/5] Pre-roll buffer reached (%u bytes), PA GPIO11 active, playback PASS",
                             (unsigned)PREROLL_MIN_BYTES);
                    break;
                }
            }
        } else {
            in_pos++;
        }
    }

    // 3. Hardware Controls Verification
    ESP_LOGI(TAG, "[TEST 3/5] Volume & Mute control verification...");
    rock_player_set_volume(40);
    vTaskDelay(pdMS_TO_TICKS(100));
    assert(s_volume == 40);
    rock_player_set_volume(70);
    vTaskDelay(pdMS_TO_TICKS(100));
    assert(s_volume == 70);

    rock_player_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    assert(atomic_load(&s_muted) == true);
    rock_player_set_mute(false);
    vTaskDelay(pdMS_TO_TICKS(200));
    assert(atomic_load(&s_muted) == false);

    rock_player_pause();
    assert(get_player_status() == ROCK_PLAYER_STATUS_PAUSED);
    vTaskDelay(pdMS_TO_TICKS(300));
    rock_player_play();
    assert(get_player_status() == ROCK_PLAYER_STATUS_PLAYING);
    ESP_LOGI(TAG, "[TEST 3/5] Volume/Mute/Pause/Play control PASS");

    // 4. Extended 30-Second Playback & Soak Loop
    ESP_LOGI(TAG, "[TEST 4/5] 30-second continuous playback & soak on Core 1...");
    int soak_seconds = 30;
    TickType_t start_tick = xTaskGetTickCount();
    size_t loop_pos = in_pos;
    size_t initial_internal = esp_get_free_internal_heap_size();
    size_t initial_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    int last_logged_sec = 0;
    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(soak_seconds * 1000)) {
        // Feed decoder
        if (rock_pcm_ring_fill(&s_pcm_ring) < (PCM_RING_CAPACITY * 70 / 100)) {
            if (loop_pos >= s_test_mp3_len) loop_pos = 0;
            esp_audio_dec_in_raw_t raw = {
                .buffer = (uint8_t *)(s_test_mp3_data + loop_pos),
                .len = (uint32_t)(s_test_mp3_len - loop_pos),
                .consumed = 0,
            };
            esp_audio_dec_out_frame_t out_frame = {
                .buffer = out_buf,
                .len = sizeof(out_buf),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_err_t dret = esp_audio_dec_process(dec, &raw, &out_frame);
            if (dret == ESP_AUDIO_ERR_OK || dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                loop_pos += raw.consumed;
                if (out_frame.decoded_size > 0) {
                    rock_pcm_ring_write(&s_pcm_ring, out_buf, out_frame.decoded_size);
                }
            } else {
                loop_pos++;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        int elapsed_sec = (int)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS / 1000);
        if (elapsed_sec > last_logged_sec && elapsed_sec % 5 == 0) {
            last_logged_sec = elapsed_sec;
            size_t cur_internal = esp_get_free_internal_heap_size();
            size_t cur_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            uint32_t underruns, dec_errors, net_reconnects;
            rock_player_get_stats(&underruns, &dec_errors, &net_reconnects);
            ESP_LOGI(TAG, "[SOAK t=%2d s] internal_ram=%u B, psram=%u B, underruns=%lu, dec_errors=%lu",
                     elapsed_sec, (unsigned)cur_internal, (unsigned)cur_psram,
                     (unsigned long)underruns, (unsigned long)dec_errors);
        }
    }

    size_t final_internal = esp_get_free_internal_heap_size();
    size_t final_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t total_underruns, total_dec_errors, total_reconnects;
    rock_player_get_stats(&total_underruns, &total_dec_errors, &total_reconnects);
    ESP_LOGI(TAG, "[TEST 4/5] 30s soak finished: internal delta=%d B, psram delta=%d B, underruns=%lu",
             (int)(final_internal - initial_internal), (int)(final_psram - initial_psram),
             (unsigned long)total_underruns);

    // 5. Stop & Error Resilience
    ESP_LOGI(TAG, "[TEST 5/5] Stop & decoder resilience...");
    rock_player_stop();
    assert(get_player_status() == ROCK_PLAYER_STATUS_STOPPED);
    assert(rock_pcm_ring_fill(&s_pcm_ring) == 0);
    ESP_LOGI(TAG, "[TEST 5/5] Stop verified");

    // Close bring-up decoder
    esp_audio_dec_close(dec);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "RE-5 PLAYER HARDWARE VERIFICATION: ALL PASSED!");
    ESP_LOGI(TAG, "==================================================");

    vTaskDelete(NULL);
}

int rock_player_bringup_start(void)
{
    if (xTaskCreatePinnedToCore(player_bringup_task, "rock_pl_test", 8192, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn player bringup task");
        return -1;
    }
    return 0;
}
#endif // CONFIG_ROCK_BRINGUP_TESTS
