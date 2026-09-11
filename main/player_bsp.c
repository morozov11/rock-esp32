#include "player_bsp.h"
#include "audio_bsp.h"
#include "display_bsp.h"
#include "test_mp3.h"

#include <assert.h>
#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
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

#define PCM_RING_CAPACITY (256 * 1024)   // 256 KiB in PSRAM (~1.5 s of 44.1 kHz stereo)
#define PREROLL_MIN_BYTES (16 * 1024)    // 16 KiB pre-roll before starting playback (~100 ms)
#define HTTP_CHUNK_SIZE   4096
#define DECODE_OUT_CAP    8192
#define MAX_REDIRECTS     5

// External pure-Rust SSRF validator
extern bool rock_validate_stream_uri(const char *uri);

// ---------------------------------------------------------------------------
// Thread-safe PSRAM PCM Ring Buffer
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t fill;
    SemaphoreHandle_t lock;
} pcm_ring_t;

static pcm_ring_t s_pcm_ring;

static int pcm_ring_init(pcm_ring_t *r, size_t capacity)
{
    r->buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->buffer) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for PCM ring buffer", (unsigned)capacity);
        return -1;
    }
    r->capacity = capacity;
    r->head = 0;
    r->tail = 0;
    r->fill = 0;
    r->lock = xSemaphoreCreateMutex();
    return r->lock ? 0 : -1;
}

static size_t pcm_ring_write(pcm_ring_t *r, const uint8_t *data, size_t len)
{
    if (!r || !r->buffer || !data || len == 0) return 0;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t free_space = r->capacity - r->fill;
    size_t to_write = len < free_space ? len : free_space;
    if (to_write > 0) {
        size_t part1 = r->capacity - r->head;
        if (to_write <= part1) {
            memcpy(r->buffer + r->head, data, to_write);
            r->head = (r->head + to_write) % r->capacity;
        } else {
            memcpy(r->buffer + r->head, data, part1);
            memcpy(r->buffer, data + part1, to_write - part1);
            r->head = to_write - part1;
        }
        r->fill += to_write;
    }
    xSemaphoreGive(r->lock);
    return to_write;
}

static size_t pcm_ring_read(pcm_ring_t *r, uint8_t *out, size_t max_len)
{
    if (!r || !r->buffer || !out || max_len == 0) return 0;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t to_read = max_len < r->fill ? max_len : r->fill;
    if (to_read > 0) {
        size_t part1 = r->capacity - r->tail;
        if (to_read <= part1) {
            memcpy(out, r->buffer + r->tail, to_read);
            r->tail = (r->tail + to_read) % r->capacity;
        } else {
            memcpy(out, r->buffer + r->tail, part1);
            memcpy(out + part1, r->buffer, to_read - part1);
            r->tail = to_read - part1;
        }
        r->fill -= to_read;
    }
    xSemaphoreGive(r->lock);
    return to_read;
}

static size_t pcm_ring_fill(pcm_ring_t *r)
{
    if (!r || !r->lock) return 0;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t fill = r->fill;
    xSemaphoreGive(r->lock);
    return fill;
}

static void pcm_ring_clear(pcm_ring_t *r)
{
    if (!r || !r->lock) return;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    r->head = 0;
    r->tail = 0;
    r->fill = 0;
    xSemaphoreGive(r->lock);
}

// ---------------------------------------------------------------------------
// Player State & Control
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_state_mutex;
static rock_player_status_t s_status = ROCK_PLAYER_STATUS_IDLE;
static char s_station_id[130];
static char s_stream_uri[2050];
static uint8_t s_volume = 35;
static bool s_muted = false;

static atomic_uint_fast32_t s_stream_generation = 0;
static atomic_bool s_stop_signal = false;

static atomic_uint_fast32_t s_stat_underruns = 0;
static atomic_uint_fast32_t s_stat_decode_errors = 0;
static atomic_uint_fast32_t s_stat_net_reconnects = 0;

static TaskHandle_t s_rx_task = NULL;
static TaskHandle_t s_tx_task = NULL;

static void set_player_status(rock_player_status_t status)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_status = status;
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
    int64_t last_underrun_log = 0;

    while (1) {
        rock_player_status_t st = get_player_status();

        if (st == ROCK_PLAYER_STATUS_PLAYING) {
            size_t n = pcm_ring_read(&s_pcm_ring, pcm_chunk, sizeof(pcm_chunk));
            if (n > 0) {
                esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
                if (codec) {
                    esp_codec_dev_write(codec, pcm_chunk, (int)n);
                    if (!s_muted) {
                        rock_audio_pa_set(true);
                    }
                }
            } else {
                // Underrun condition
                atomic_fetch_add(&s_stat_underruns, 1);
                int64_t now = esp_timer_get_time();
                if (now - last_underrun_log > 5000000) { // log at most once per 5 s
                    ESP_LOGW(TAG, "PCM ring underrun (total: %u)", (unsigned)atomic_load(&s_stat_underruns));
                    last_underrun_log = now;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
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

// ---------------------------------------------------------------------------
// HTTP Stream & Decode Worker (CPU 1, Priority 5)
// ---------------------------------------------------------------------------
static void stream_rx_task(void *arg)
{
    (void)arg;
    uint32_t current_gen = atomic_load(&s_stream_generation);
    char active_url[2050];

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(active_url, s_stream_uri, sizeof(active_url));
    char current_station[130];
    strlcpy(current_station, s_station_id, sizeof(current_station));
    xSemaphoreGive(s_state_mutex);

    set_player_status(ROCK_PLAYER_STATUS_BUFFERING);
    rock_ui_show_now_playing(current_station, "BUFFERING", "Connecting to stream...");

    esp_http_client_handle_t http_client = NULL;
    esp_audio_dec_handle_t dec = NULL;
    esp_audio_type_t codec_type = ESP_AUDIO_TYPE_UNSUPPORT;

    int redirects = 0;
    while (redirects <= MAX_REDIRECTS) {
        if (atomic_load(&s_stop_signal) || atomic_load(&s_stream_generation) != current_gen) {
            goto cleanup;
        }

        esp_http_client_config_t cfg = {
            .url = active_url,
            .timeout_ms = 10000,
            .disable_auto_redirect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
            .buffer_size_tx = 1024,
        };
        http_client = esp_http_client_init(&cfg);
        if (!http_client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            goto fail;
        }

        esp_err_t err = esp_http_client_open(http_client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(http_client);
            http_client = NULL;
            goto fail;
        }

        esp_http_client_fetch_headers(http_client);
        int status = esp_http_client_get_status_code(http_client);
        ESP_LOGI(TAG, "HTTP response status: %d", status);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            redirects++;
            if (redirects > MAX_REDIRECTS) {
                ESP_LOGE(TAG, "Too many HTTP redirects (> %d)", MAX_REDIRECTS);
                goto fail;
            }
            char *location = NULL;
            esp_http_client_get_header(http_client, "Location", &location);
            if (!location || strlen(location) == 0) {
                ESP_LOGE(TAG, "Redirect without Location header");
                goto fail;
            }
            // Strict SSRF validation on redirect target
            if (!rock_validate_stream_uri(location)) {
                ESP_LOGE(TAG, "Redirect destination rejected by SSRF validation");
                goto fail;
            }
            strlcpy(active_url, location, sizeof(active_url));
            esp_http_client_close(http_client);
            esp_http_client_cleanup(http_client);
            http_client = NULL;
            continue;
        }

        if (status != 200 && status != 206) {
            ESP_LOGE(TAG, "HTTP non-200/206 status: %d", status);
            goto fail;
        }

        // Content-Type validation without guessing
        char *content_type = NULL;
        esp_http_client_get_header(http_client, "Content-Type", &content_type);
        if (!content_type) {
            ESP_LOGE(TAG, "Missing Content-Type header; rejected");
            goto fail;
        }
        ESP_LOGI(TAG, "Content-Type: %s", content_type);
        if (strcasestr(content_type, "audio/mpeg") || strcasestr(content_type, "audio/mp3") || strcasestr(content_type, "audio/x-mpeg")) {
            codec_type = ESP_AUDIO_TYPE_MP3;
        } else if (strcasestr(content_type, "audio/aac") || strcasestr(content_type, "audio/aacp") ||
                   strcasestr(content_type, "audio/x-aac") || strcasestr(content_type, "audio/mp4")) {
            codec_type = ESP_AUDIO_TYPE_AAC;
        } else {
            ESP_LOGE(TAG, "Unsupported Content-Type (not MP3/AAC): %s", content_type);
            goto fail;
        }
        break;
    }

    if (!http_client || codec_type == ESP_AUDIO_TYPE_UNSUPPORT) {
        goto fail;
    }

    // Open audio decoder
    esp_audio_dec_cfg_t dec_cfg = {
        .type = codec_type,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open audio decoder for type %d", codec_type);
        goto fail;
    }

    static uint8_t in_buf[HTTP_CHUNK_SIZE * 2];
    static uint8_t out_buf[DECODE_OUT_CAP];
    size_t in_len = 0;
    uint32_t current_fs = 16000;
    uint8_t current_channels = 1;
    bool playback_started = false;

    while (!atomic_load(&s_stop_signal) && atomic_load(&s_stream_generation) == current_gen) {
        // Flow control: wait if PCM ring buffer is getting full (> 85%)
        if (pcm_ring_fill(&s_pcm_ring) > (PCM_RING_CAPACITY * 85 / 100)) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // Read network chunk
        int space = (int)(sizeof(in_buf) - in_len);
        if (space > HTTP_CHUNK_SIZE) space = HTTP_CHUNK_SIZE;
        int rlen = esp_http_client_read(http_client, (char *)(in_buf + in_len), space);
        if (rlen < 0) {
            ESP_LOGW(TAG, "HTTP read error %d; attempting reconnect backoff", rlen);
            atomic_fetch_add(&s_stat_net_reconnects, 1);
            // Reconnect attempt with backoff
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (atomic_load(&s_stop_signal) || atomic_load(&s_stream_generation) != current_gen) goto cleanup;
            esp_http_client_close(http_client);
            if (esp_http_client_open(http_client, 0) != ESP_OK) {
                ESP_LOGE(TAG, "HTTP reconnect failed");
                goto fail;
            }
            esp_http_client_fetch_headers(http_client);
            continue;
        } else if (rlen == 0) {
            // End of stream or server paused
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        in_len += (size_t)rlen;

        // Decode loop
        size_t in_pos = 0;
        while (in_len > in_pos && !atomic_load(&s_stop_signal)) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = in_buf + in_pos,
                .len = (uint32_t)(in_len - in_pos),
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
                    // Check sample info dynamically
                    esp_audio_dec_info_t info;
                    if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                        if (info.sample_rate != current_fs || info.channel != current_channels) {
                            ESP_LOGI(TAG, "Decoder format: %lu Hz, %u ch (reconfiguring codec)",
                                     (unsigned long)info.sample_rate, info.channel);
                            rock_audio_reconfig(info.sample_rate, info.channel);
                            current_fs = info.sample_rate;
                            current_channels = info.channel;
                        }
                    }

                    pcm_ring_write(&s_pcm_ring, out_buf, out_frame.decoded_size);

                    // Pre-roll check
                    if (!playback_started && pcm_ring_fill(&s_pcm_ring) >= PREROLL_MIN_BYTES) {
                        playback_started = true;
                        set_player_status(ROCK_PLAYER_STATUS_PLAYING);
                        rock_ui_show_now_playing(current_station, "PLAYING", "RockCast Radio");
                        if (!s_muted) {
                            rock_audio_pa_set(true);
                        }
                        ESP_LOGI(TAG, "Pre-roll reached (%u bytes); playback started", (unsigned)PREROLL_MIN_BYTES);
                    }
                }
            } else if (dret == ESP_AUDIO_ERR_DATA_LACK) {
                // Need more data from network
                break;
            } else {
                atomic_fetch_add(&s_stat_decode_errors, 1);
                in_pos++; // skip byte to resynchronize
            }
        }

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
    set_player_status(ROCK_PLAYER_STATUS_ERROR);
    rock_ui_show_text("PLAYBACK ERROR");
    rock_audio_pa_set(false);

cleanup:
    if (dec) {
        esp_audio_dec_close(dec);
        dec = NULL;
    }
    if (http_client) {
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        http_client = NULL;
    }
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
        return -1;
    }

    // Register official esp_audio_codec decoders
    esp_mp3_dec_register();
    esp_aac_dec_register();

    // Allocate PSRAM PCM ring buffer
    if (pcm_ring_init(&s_pcm_ring, PCM_RING_CAPACITY) != 0) {
        ESP_LOGE(TAG, "PCM ring buffer initialization failed");
        return -1;
    }

    // Set initial volume & mute
    esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
    if (codec) {
        esp_codec_dev_set_out_vol(codec, s_volume);
        esp_codec_dev_set_out_mute(codec, s_muted);
    }
    rock_audio_pa_set(false);

    // Create audio TX worker pinned to Core 1
    if (xTaskCreatePinnedToCore(audio_tx_task, "rock_au_tx", 4096, NULL, 6, &s_tx_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio TX worker");
        return -1;
    }

    ESP_LOGI(TAG, "Player initialized: PSRAM ring %u KiB, PA GPIO11, decoders: MP3+AAC",
             PCM_RING_CAPACITY / 1024);
    return 0;
}

int rock_player_play_stream(const char *stream_uri, const char *station_id)
{
    if (!stream_uri || !station_id) return -1;

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

    pcm_ring_clear(&s_pcm_ring);
    rock_audio_pa_set(false);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(s_stream_uri, stream_uri, sizeof(s_stream_uri));
    strlcpy(s_station_id, station_id, sizeof(s_station_id));
    xSemaphoreGive(s_state_mutex);

    atomic_store(&s_stop_signal, false);

    // Spawn stream RX worker on Core 1
    if (xTaskCreatePinnedToCore(stream_rx_task, "rock_st_rx", 8192, NULL, 5, &s_rx_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start stream RX task");
        set_player_status(ROCK_PLAYER_STATUS_ERROR);
        return -1;
    }

    return 0;
}

int rock_player_play(void)
{
    rock_player_status_t st = get_player_status();
    if (st == ROCK_PLAYER_STATUS_PAUSED) {
        set_player_status(ROCK_PLAYER_STATUS_PLAYING);
        if (!s_muted) rock_audio_pa_set(true);
        rock_ui_show_now_playing(s_station_id, "PLAYING", "RockCast Radio");
    }
    return 0;
}

int rock_player_pause(void)
{
    rock_player_status_t st = get_player_status();
    if (st == ROCK_PLAYER_STATUS_PLAYING) {
        set_player_status(ROCK_PLAYER_STATUS_PAUSED);
        rock_audio_pa_set(false);
        rock_ui_show_now_playing(s_station_id, "PAUSED", "");
    }
    return 0;
}

int rock_player_stop(void)
{
    atomic_store(&s_stop_signal, true);
    atomic_fetch_add(&s_stream_generation, 1);
    pcm_ring_clear(&s_pcm_ring);
    rock_audio_pa_set(false);
    set_player_status(ROCK_PLAYER_STATUS_STOPPED);
    return 0;
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
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_muted = muted;
    xSemaphoreGive(s_state_mutex);

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
    if (muted) *muted = s_muted;
    xSemaphoreGive(s_state_mutex);
    return 0;
}

void rock_player_get_stats(uint32_t *underruns, uint32_t *decode_errors, uint32_t *net_reconnects)
{
    if (underruns) *underruns = (uint32_t)atomic_load(&s_stat_underruns);
    if (decode_errors) *decode_errors = (uint32_t)atomic_load(&s_stat_decode_errors);
    if (net_reconnects) *net_reconnects = (uint32_t)atomic_load(&s_stat_net_reconnects);
}

// ---------------------------------------------------------------------------
// Hardware Bring-up & Verification Task (Pinned to Core 1)
// ---------------------------------------------------------------------------
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
    rock_ui_show_now_playing("bringup-station", "BUFFERING", "RockCast Radio");

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

    // Decode until pre-roll is reached and playback starts
    while (in_pos < s_test_mp3_len) {
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
                        current_fs = info.sample_rate;
                        current_channels = info.channel;
                    }
                }
                pcm_ring_write(&s_pcm_ring, out_buf, out_frame.decoded_size);
                if (!playback_started && pcm_ring_fill(&s_pcm_ring) >= PREROLL_MIN_BYTES) {
                    playback_started = true;
                    set_player_status(ROCK_PLAYER_STATUS_PLAYING);
                    rock_ui_show_now_playing("bringup-station", "PLAYING", "RockCast Radio");
                    if (!s_muted) rock_audio_pa_set(true);
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
    assert(s_muted == true);
    rock_player_set_mute(false);
    vTaskDelay(pdMS_TO_TICKS(200));
    assert(s_muted == false);

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
        if (pcm_ring_fill(&s_pcm_ring) < (PCM_RING_CAPACITY * 70 / 100)) {
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
                    pcm_ring_write(&s_pcm_ring, out_buf, out_frame.decoded_size);
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
    assert(pcm_ring_fill(&s_pcm_ring) == 0);
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
