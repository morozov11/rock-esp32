// Unified stream source, phase-4 split: this file owns the SOURCE domain —
// the single authorized upstream connection, ICY filtering, MP3 sync,
// decode, resampling to 48 kHz stereo, pacing and the shared ring. The
// Cast-side listener and serve loops live in rock_relay_http.c; ring buffers
// in rock_ring.c. Public ABI: rock_source.h; shared plumbing: relay_internal.h.

#include "rock_source.h"
#include "control_platform.h"
#include "relay_internal.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_audio_dec.h"
#include "esp_aac_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

static const char *TAG = "rock_relay";

// Compressed-audio staging buffer between the upstream socket and the decoder.
#define RELAY_RAW_BUFFER_BYTES (8 * 1024)
// 1 s of 48 kHz stereo PCM pushed to the ring before the listener starts.
#define RELAY_PREFILL_BYTES (48 * 1000 * 2 * 2)
#define RELAY_PREFILL_WAIT_MS 25000
// Bytes to keep behind the live edge when a Cast client joins:
// 144 KB (~0.75 s) for 48 kHz stereo PCM; 16 KB (~1.0 s) for compressed MP3.
#define RELAY_JOIN_CUSHION_BYTES_PCM (144 * 1024)
#define RELAY_JOIN_CUSHION_BYTES_PASSTHROUGH (16 * 1024)
#define RELAY_PROGRESS_BYTES (64 * 1024)
#define RELAY_READ_CHUNK_BYTES 4096
#define RELAY_PCM_CHUNK_BYTES 32768
#define RELAY_WAV_CHUNK_BYTES 32768
#define RELAY_PCM_BYTES_PER_SEC (48u * 1000u * 2u * 2u)
// Pacing slice, mirrors the desktop relay's pace_pcm_chunk() granularity.
#define RELAY_PACE_SLICE_MS 20
// Extra lead the transcode feeder may bank ahead of the media clock
// (1.5 MiB ~= 8 s of 48 kHz stereo). Without it the feeder is pinned to
// realtime delivery, the speaker tap sits on the live edge and any upstream
// delivery dip longer than ~1.5 s of buffered PCM underruns the DAC. The
// ring (2 MiB, drop-oldest) absorbs the bank; consumers keep their own
// positions, so this only adds jitter headroom, not latency, for readers
// that pace themselves (speaker PCM ring, Cast clients).
#define RELAY_PACE_BANK_BYTES (1536 * 1024)
#define RELAY_STOP_JOIN_MS 12000
// Passthrough prefill before the listener starts (rockcast min_bytes for
// non-WAV transports).
#define RELAY_PASSTHROUGH_PREFILL_BYTES (4 * 1024)
// Sentinel for "no ring sample yet" in the feeder stats window.
#define ROCK_RING_UNUSED SIZE_MAX
// PCM speaker tap pacing burst: the media-clock token bucket may grant up to
// 1 s of PCM at once (fast preroll start, smooth bank drain afterwards).
#define RELAY_TAP_BURST_BYTES (RELAY_PCM_BYTES_PER_SEC)
#define RELAY_CONTENT_TYPE_MAX 96

static char s_upstream_uri[512] = {0};
// Bearer for the RockServer station proxy; empty string means an anonymous
// upstream. NEVER logged (confidentiality: same rule as s_upstream_uri).
static char s_upstream_bearer[600] = {0};
// Guards s_upstream_uri/s_upstream_bearer (B5 fix): written by the session
// task in rock_source_start/update_bearer/stop, read by the feeder task in
// upstream_connect when it forms the URL and Authorization header.
// Created once in rock_source_start, the single entry point that can run
// before any reader exists.
static SemaphoreHandle_t s_config_lock = NULL;
static atomic_bool s_relay_stop = false;
static atomic_bool s_relay_running = false;
// Successful reconnects of a dropped upstream; reset by rock_source_start
// (the primary connect is not counted). Exposed for player telemetry.
static atomic_uint s_upstream_reconnects = 0;
// Passthrough mode: forward compressed MP3 as-is instead of transcoding to
// PCM WAV.  The radio link (RSSI ~-77 dBm) cannot carry 192 KB/s of WAV plus
// upstream reliably; 32 KB/s of MP3 restores inbound ACK flow.  rockcast
// exposes the same RelayTransport::Passthrough path.
static bool s_passthrough = false;
static char s_content_type[RELAY_CONTENT_TYPE_MAX] = "audio/mpeg";
// Enum twin of s_content_type (phase 5): set by the same normalization below,
// read cross-language without string parsing.
static rock_source_content_type_t s_content_enum = ROCK_SOURCE_CT_MPEG;
static rock_source_pcm_tap_fn s_pcm_tap_fn = NULL;
static void *s_pcm_tap_ctx = NULL;
static esp_audio_dec_handle_t s_shadow_mp3_dec = NULL;

typedef struct {
    uint32_t source_rate;
    uint32_t phase;
    int16_t previous_left;
    int16_t previous_right;
    bool have_previous;
} pcm_resampler_t;

#define ICY_METADATA_BUF_SIZE 512

typedef struct {
    size_t metadata_interval;
    size_t until_metadata;
    size_t metadata_remaining;
    char meta_buf[ICY_METADATA_BUF_SIZE];
    size_t meta_len;
} icy_filter_t;

typedef struct {
    char content_type[96];
    size_t icy_metaint;
} upstream_headers_t;

// One live upstream session: decode once, push 48 kHz stereo PCM into the
// ring, exactly like the desktop relay feeder + fanout.
typedef struct {
    esp_http_client_config_t http_cfg;
    esp_http_client_handle_t http;
    upstream_headers_t headers;
    icy_filter_t icy;
    esp_audio_dec_handle_t dec;
    esp_audio_type_t dec_type;
    pcm_resampler_t resampler;
    uint8_t *raw;
    size_t raw_len;
    bool mp3_synced;
} relay_upstream_t;

// Transcode feeder performance window (AAC underrun diagnosis): accumulated
// wall time in the blocking stages plus ring occupancy, logged every 5 s.
typedef struct {
    int64_t window_start_us;
    uint32_t read_us;
    uint32_t decode_us;
    uint32_t decode_max_us;
    uint32_t pace_us;
    size_t in_bytes;
    size_t out_bytes;
    uint32_t frames;
    size_t ring_min;
    size_t ring_max;
    // AAC frame shape: ADTS header params of the window plus the decoded
    // frame size split around the 1024/2048-sample boundary (4096 B stereo).
    uint32_t frames_small;
    uint32_t frames_large;
    uint32_t dec_size_max;
    uint8_t adts_freq_idx;
    uint8_t adts_profile;
    // Decoder-reported format snapshot (may differ from the ADTS header
    // when the library applies SBR/PS internally).
    uint32_t dec_rate;
    uint32_t dec_bitrate;
    uint32_t dec_frame_size;
    // Monotonic session counters (never reset with the window).
    uint64_t session_frames;
    int64_t session_start_us;
} feeder_stats_t;
static feeder_stats_t s_fstats = {0};

static relay_upstream_t s_upstream = {0};
// Only relay_task touches this handle; it survives feeder task exits so its
// TLS ticket remains available to the next station on the same origin.
static esp_http_client_handle_t s_http_client = NULL;
static char s_http_url[sizeof(s_upstream_uri)] = {0};
static SemaphoreHandle_t s_source_started = NULL;
static bool s_source_start_ok = false;
static rock_ring_t s_ring = {0};
// Speaker consumer tap: ring position once it has joined the live edge.
// Ring itself is persistent (allocated once, reset on source restart) so a
// consumer can never touch a freed buffer.
static bool s_tap_joined = false;
static uint64_t s_tap_pos = 0;
// Media-clock token bucket for the PCM speaker tap (jitter buffer pacing).
static int64_t s_tap_budget = 0;
static int64_t s_tap_last_refill_us = 0;

bool relay_stopped(void)
{
    return atomic_load(&s_relay_stop);
}

bool relay_is_running(void)
{
    return atomic_load(&s_relay_running);
}

bool relay_is_passthrough(void)
{
    return s_passthrough;
}

const char *relay_content_type(void)
{
    return s_content_type;
}

size_t relay_cushion_bytes(void)
{
    return s_passthrough ? RELAY_JOIN_CUSHION_BYTES_PASSTHROUGH : RELAY_JOIN_CUSHION_BYTES_PCM;
}

rock_ring_t *relay_ring(void)
{
    return &s_ring;
}

// --------------------------------------------------------------- utils ---

bool relay_send_all(int sock, const void *buf, size_t len)
{
    const uint8_t *data = buf;
    bool backpressure_logged = false;
    uint32_t retries = 0;
    while (len > 0) {
        int sent = send(sock, data, len, 0);
        if (sent > 0) {
            data += sent;
            len -= (size_t)sent;
            retries = 0;
            continue;
        }

        // A Chromecast may briefly apply TCP backpressure while it starts the
        // decoder. Treat a timed-out send as transient, just like the desktop
        // relay, rather than tearing down a healthy live stream.
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK
                || errno == ETIMEDOUT || errno == EINTR)) {
            if (!backpressure_logged) {
                ESP_LOGW(TAG, "Relay socket backpressure while sending %u bytes: errno %d",
                    (unsigned)len, errno);
                backpressure_logged = true;
            }
            if (relay_stopped()) {
                return false;
            }
            if (++retries > 10) {
                ESP_LOGW(TAG, "Relay socket send timed out after %u retries", (unsigned)retries);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        ESP_LOGW(TAG, "Relay socket send failed: errno %d", errno);
        return false;
    }
    return true;
}

bool relay_send_audio(
    int sock,
    const uint8_t *data,
    size_t len,
    size_t *total_bytes,
    size_t *last_log_bytes
)
{
    // Whole-chunk sends only: 1 KB pieces meant ~200 send() calls per second
    // of audio, and that SDIO/Wi-Fi TX flood starved the C6's RX path - the
    // Chromecast's ACKs and window updates got dropped and the stream died in
    // a zero-window deadlock.  Big writes let lwip segment at full MSS.
    while (len > 0) {
        if (!relay_send_all(sock, data, len)) {
            return false;
        }
        *total_bytes += len;
        if (*total_bytes - *last_log_bytes >= RELAY_PROGRESS_BYTES) {
            *last_log_bytes = *total_bytes;
            ESP_LOGI(TAG, "Relay streamed %u bytes to Chromecast", (unsigned)*total_bytes);
        }
        break;
    }
    return true;
}

// Emit decoded PCM on the media clock (rockcast fanout::pace_pcm_chunk):
// decode-and-burst fills the ring, drops the oldest audio and bursts sockets.
static bool pace_before_push(uint64_t pushed, size_t chunk_len, int64_t start_us)
{
    while (true) {
        int64_t allowed = (int64_t)(RELAY_PREFILL_BYTES + RELAY_PACE_BANK_BYTES)
            + (esp_timer_get_time() - start_us) * (int64_t)RELAY_PCM_BYTES_PER_SEC / 1000000;
        int64_t overshoot = (int64_t)(pushed + chunk_len) - allowed;
        if (overshoot <= 0) {
            return true;
        }
        if (relay_stopped()) {
            return false;
        }
        int64_t sleep_us = overshoot * 1000000 / (int64_t)RELAY_PCM_BYTES_PER_SEC;
        if (sleep_us > RELAY_PACE_SLICE_MS * 1000) {
            sleep_us = RELAY_PACE_SLICE_MS * 1000;
        }
        vTaskDelay(pdMS_TO_TICKS((unsigned)((sleep_us + 999) / 1000)));
    }
}

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    put_le16(dst, (uint16_t)value);
    put_le16(dst + 2, (uint16_t)(value >> 16));
}

void relay_make_wav_header(uint8_t header[44])
{
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    put_le32(header + 4, UINT32_MAX);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le32(header + 16, 16);
    put_le16(header + 20, 1);
    put_le16(header + 22, 2);
    put_le32(header + 24, 48000);
    put_le32(header + 28, 48000 * 2 * 2);
    put_le16(header + 32, 2 * 2);
    put_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, UINT32_MAX);
}

static size_t resample_to_48k_stereo(
    pcm_resampler_t *resampler,
    const int16_t *input,
    size_t input_frames,
    uint16_t input_channels,
    int16_t *output,
    size_t output_capacity_frames
)
{
    size_t output_frames = 0;
    for (size_t i = 0; i < input_frames; i++) {
        int16_t left = input[i * input_channels];
        int16_t right = input_channels > 1 ? input[i * input_channels + 1] : left;
        if (!resampler->have_previous) {
            output[0] = left;
            output[1] = right;
            output_frames = 1;
            resampler->previous_left = left;
            resampler->previous_right = right;
            resampler->phase = resampler->source_rate;
            resampler->have_previous = true;
            continue;
        }
        while (resampler->phase <= 48000 && output_frames < output_capacity_frames) {
            int32_t fraction = (int32_t)resampler->phase;
            output[output_frames * 2] = resampler->previous_left
                + ((int32_t)(left - resampler->previous_left) * fraction / 48000);
            output[output_frames * 2 + 1] = resampler->previous_right
                + ((int32_t)(right - resampler->previous_right) * fraction / 48000);
            output_frames++;
            resampler->phase += resampler->source_rate;
        }
        resampler->phase -= 48000;
        resampler->previous_left = left;
        resampler->previous_right = right;
    }
    return output_frames * 4;
}

static size_t mpeg_frame_size(const uint8_t *data, size_t len)
{
    static const uint16_t sample_rates[] = {44100, 48000, 32000};
    static const uint16_t mpeg1_l1[] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448};
    static const uint16_t mpeg1_l2[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
    static const uint16_t mpeg1_l3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const uint16_t mpeg2_l1[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256};
    static const uint16_t mpeg2_l23[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};

    if (len < 4 || data[0] != 0xff || (data[1] & 0xe0) != 0xe0) {
        return 0;
    }
    unsigned version = (data[1] >> 3) & 0x03;
    unsigned layer = (data[1] >> 1) & 0x03;
    unsigned bitrate_index = (data[2] >> 4) & 0x0f;
    unsigned rate_index = (data[2] >> 2) & 0x03;
    unsigned padding = (data[2] >> 1) & 0x01;
    if (version == 1 || layer == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
        return 0;
    }

    unsigned sample_rate = sample_rates[rate_index];
    if (version == 2) {
        sample_rate /= 2;
    } else if (version == 0) {
        sample_rate /= 4;
    }
    unsigned bitrate = 0;
    if (version == 3) {
        bitrate = layer == 3 ? mpeg1_l1[bitrate_index]
            : layer == 2 ? mpeg1_l2[bitrate_index] : mpeg1_l3[bitrate_index];
    } else {
        bitrate = layer == 3 ? mpeg2_l1[bitrate_index] : mpeg2_l23[bitrate_index];
    }

    if (bitrate == 0 || sample_rate == 0) {
        return 0;
    }
    if (layer == 3) {
        return ((12 * bitrate * 1000 / sample_rate) + padding) * 4;
    }
    return ((version != 3 && layer == 1 ? 72 : 144) * bitrate * 1000 / sample_rate) + padding;
}

size_t relay_find_mpeg_frame(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 4 < len; ++i) {
        size_t frame_size = mpeg_frame_size(data + i, len - i);
        if (frame_size > 0 && frame_size + 4 <= len - i
            && mpeg_frame_size(data + i + frame_size, len - i - frame_size) > 0) {
            return i;
        }
    }
    return len;
}

// ----------------------------------------------------------- upstream ---

static esp_err_t on_upstream_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data
        || !event->header_key || !event->header_value) {
        return ESP_OK;
    }

    upstream_headers_t *headers = event->user_data;
    if (strcasecmp(event->header_key, "Content-Type") == 0) {
        snprintf(headers->content_type, sizeof(headers->content_type), "%s", event->header_value);
    } else if (strcasecmp(event->header_key, "icy-metaint") == 0
        || strcasecmp(event->header_key, "ice-metaint") == 0) {
        headers->icy_metaint = strtoul(event->header_value, NULL, 10);
    }
    return ESP_OK;
}

static char s_meta_title[512] = {0};
static char s_meta_pending[512] = {0};
static atomic_uint_least32_t s_meta_generation = 0;
static int64_t s_meta_last_update_us = 0;

static const char *find_stream_title_value(const char *buf, size_t len)
{
    const char *key = "StreamTitle='";
    size_t key_len = strlen(key);
    if (len < key_len) {
        return NULL;
    }
    for (size_t i = 0; i <= len - key_len; i++) {
        if (strncasecmp(buf + i, key, key_len) == 0) {
            return buf + i + key_len;
        }
    }
    return NULL;
}

static bool parse_stream_title(const char *raw, size_t raw_len, char *out, size_t out_cap)
{
    const char *p = find_stream_title_value(raw, raw_len);
    if (!p) {
        return false;
    }
    size_t out_pos = 0;
    while (*p && out_pos < out_cap - 1) {
        if (*p == '\\' && *(p + 1) == '\'') {
            out[out_pos++] = '\'';
            p += 2;
            continue;
        }
        if (*p == '\'') {
            if (*(p + 1) == ';' || *(p + 1) == '\0') {
                break;
            }
            out[out_pos++] = *p++;
            continue;
        }
        if (*p == ';') {
            break;
        }
        out[out_pos++] = *p++;
    }
    out[out_pos] = '\0';

    while (out_pos > 0 && ((unsigned char)out[out_pos - 1] <= ' ')) {
        out[--out_pos] = '\0';
    }
    char *start = out;
    while (*start && ((unsigned char)*start <= ' ')) {
        start++;
    }
    if (start != out) {
        memmove(out, start, strlen(start) + 1);
    }
    return true;
}

static void update_meta_title(const char *new_title)
{
    if (!new_title) return;
    if (!s_config_lock) {
        s_config_lock = xSemaphoreCreateMutex();
        if (!s_config_lock) return;
    }

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (strcmp(s_meta_title, new_title) != 0) {
        int64_t now_us = esp_timer_get_time();
        if (s_meta_last_update_us == 0 || (now_us - s_meta_last_update_us) >= 3000000LL) {
            strncpy(s_meta_title, new_title, sizeof(s_meta_title) - 1);
            s_meta_title[sizeof(s_meta_title) - 1] = '\0';
            s_meta_pending[0] = '\0';
            s_meta_last_update_us = now_us;
            atomic_fetch_add(&s_meta_generation, 1);
            ESP_LOGI(TAG, "ICY StreamTitle updated: \"%s\" (gen=%u)",
                     s_meta_title, (unsigned)atomic_load(&s_meta_generation));
        } else {
            strncpy(s_meta_pending, new_title, sizeof(s_meta_pending) - 1);
            s_meta_pending[sizeof(s_meta_pending) - 1] = '\0';
        }
    } else {
        s_meta_pending[0] = '\0';
    }
    xSemaphoreGive(s_config_lock);
}

static void check_meta_pending(void)
{
    if (s_meta_pending[0] == '\0') return;
    if (!s_config_lock) return;

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (s_meta_pending[0] != '\0') {
        int64_t now_us = esp_timer_get_time();
        if (s_meta_last_update_us == 0 || (now_us - s_meta_last_update_us) >= 3000000LL) {
            strncpy(s_meta_title, s_meta_pending, sizeof(s_meta_title) - 1);
            s_meta_title[sizeof(s_meta_title) - 1] = '\0';
            s_meta_pending[0] = '\0';
            s_meta_last_update_us = now_us;
            atomic_fetch_add(&s_meta_generation, 1);
            ESP_LOGI(TAG, "ICY StreamTitle updated from pending: \"%s\" (gen=%u)",
                     s_meta_title, (unsigned)atomic_load(&s_meta_generation));
        }
    }
    xSemaphoreGive(s_config_lock);
}

static void clear_meta_title(void)
{
    if (s_config_lock) {
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
    }
    bool had_title = (s_meta_title[0] != '\0' || s_meta_pending[0] != '\0');
    s_meta_title[0] = '\0';
    s_meta_pending[0] = '\0';
    s_meta_last_update_us = 0;
    if (had_title) {
        atomic_fetch_add(&s_meta_generation, 1);
    }
    if (s_config_lock) {
        xSemaphoreGive(s_config_lock);
    }
}

// RockCast requests ICY metadata and removes it before the Cast-side relay.
// The metadata bytes are not audio, so passing them through corrupts an MP3
// frame stream even when the upstream Content-Type says audio/mpeg.
static size_t filter_icy_audio(
    icy_filter_t *filter,
    const uint8_t *input,
    size_t input_len,
    uint8_t *output,
    size_t output_cap
)
{
    if (filter->metadata_interval == 0) {
        size_t n = input_len < output_cap ? input_len : output_cap;
        memcpy(output, input, n);
        return n;
    }

    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < input_len && out_pos < output_cap) {
        if (filter->metadata_remaining > 0) {
            size_t skip = filter->metadata_remaining;
            if (skip > input_len - in_pos) {
                skip = input_len - in_pos;
            }
            if (filter->meta_len < sizeof(filter->meta_buf) - 1) {
                size_t to_copy = skip;
                if (to_copy > (sizeof(filter->meta_buf) - 1) - filter->meta_len) {
                    to_copy = (sizeof(filter->meta_buf) - 1) - filter->meta_len;
                }
                memcpy(filter->meta_buf + filter->meta_len, input + in_pos, to_copy);
                filter->meta_len += to_copy;
                filter->meta_buf[filter->meta_len] = '\0';
            }
            in_pos += skip;
            filter->metadata_remaining -= skip;
            if (filter->metadata_remaining == 0) {
                filter->until_metadata = filter->metadata_interval;
                char parsed_title[512] = {0};
                if (parse_stream_title(filter->meta_buf, filter->meta_len, parsed_title, sizeof(parsed_title))) {
                    update_meta_title(parsed_title);
                }
            }
            continue;
        }

        if (filter->until_metadata == 0) {
            filter->metadata_remaining = (size_t)input[in_pos++] * 16;
            filter->meta_len = 0;
            filter->meta_buf[0] = '\0';
            if (filter->metadata_remaining == 0) {
                filter->until_metadata = filter->metadata_interval;
            }
            continue;
        }

        size_t take = filter->until_metadata;
        if (take > input_len - in_pos) {
            take = input_len - in_pos;
        }
        if (take > output_cap - out_pos) {
            take = output_cap - out_pos;
        }
        memcpy(output + out_pos, input + in_pos, take);
        in_pos += take;
        out_pos += take;
        filter->until_metadata -= take;
    }
    return out_pos;
}

static bool upstream_origin(const char *url, struct http_parser_url *parsed, unsigned *port)
{
    http_parser_url_init(parsed);
    if (http_parser_parse_url(url, strlen(url), 0, parsed) != 0
        || !(parsed->field_set & (1 << UF_SCHEMA))
        || !(parsed->field_set & (1 << UF_HOST))
        || (parsed->field_set & (1 << UF_USERINFO))) {
        return false;
    }
    const size_t len = parsed->field_data[UF_SCHEMA].len;
    if (len == 5 && strncasecmp(url + parsed->field_data[UF_SCHEMA].off, "https", 5) == 0) {
        *port = 443;
    } else if (len == 4 && strncasecmp(url + parsed->field_data[UF_SCHEMA].off, "http", 4) == 0) {
        *port = 80;
    } else {
        return false;
    }
    if (parsed->field_set & (1 << UF_PORT)) {
        if (parsed->port == 0) {
            return false;
        }
        *port = parsed->port;
    }
    return true;
}

static bool upstream_same_origin(const char *a, const char *b)
{
    struct http_parser_url x, y;
    unsigned x_port, y_port;
    return upstream_origin(a, &x, &x_port) && upstream_origin(b, &y, &y_port)
        && x.field_data[UF_SCHEMA].len == y.field_data[UF_SCHEMA].len
        && strncasecmp(a + x.field_data[UF_SCHEMA].off, b + y.field_data[UF_SCHEMA].off,
            x.field_data[UF_SCHEMA].len) == 0
        && x.field_data[UF_HOST].len == y.field_data[UF_HOST].len
        && strncasecmp(a + x.field_data[UF_HOST].off, b + y.field_data[UF_HOST].off,
            x.field_data[UF_HOST].len) == 0
        && x_port == y_port;
}

static void upstream_discard_http(relay_upstream_t *up)
{
    if (s_http_client) {
        esp_http_client_cleanup(s_http_client);
        s_http_client = NULL;
    }
    s_http_url[0] = '\0';
    up->http = NULL;
}

static void upstream_teardown(relay_upstream_t *up)
{
    if (up->http && esp_http_client_close(up->http) != ESP_OK) {
        upstream_discard_http(up);
    }
    if (up->dec) {
        esp_audio_dec_close(up->dec);
        up->dec = NULL;
    }
    free(up->raw);
    up->raw = NULL;
    up->raw_len = 0;
    memset(up, 0, sizeof(*up));
}

static bool upstream_connect(relay_upstream_t *up)
{
    for (int retry = 0; retry < 2; retry++) {
        if (relay_stopped()) {
            return false;
        }
        memset(&up->headers, 0, sizeof(up->headers));
        up->icy.metadata_interval = 0;
        up->icy.until_metadata = 0;
        up->icy.metadata_remaining = 0;
        up->icy.meta_len = 0;
        up->icy.meta_buf[0] = '\0';

        // Snapshot the upstream URL/bearer under the config lock: the session
        // task may refresh the bearer while the feeder reconnects.  The URL
        // copy only lives across set_url/init, which parse it synchronously.
        char uri[sizeof(s_upstream_uri)];
        char bearer[sizeof(s_upstream_bearer)] = {0};
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
        memcpy(uri, s_upstream_uri, sizeof(uri));
        memcpy(bearer, s_upstream_bearer, sizeof(bearer));
        xSemaphoreGive(s_config_lock);
        // A transport ticket is scoped to scheme, host and port.  IDF keeps
        // it in the handle even when set_url changes the host, so discard first.
        bool reused = s_http_client && upstream_same_origin(s_http_url, uri);
        if (s_http_client && !reused) {
            upstream_discard_http(up);
        }
        if (!s_http_client) {
            esp_http_client_config_t http_cfg = up->http_cfg;
            http_cfg.url = uri;
            s_http_client = esp_http_client_init(&http_cfg);
            if (s_http_client) {
                memcpy(s_http_url, uri, sizeof(s_http_url));
            }
        } else if (esp_http_client_clear_response_buffer(s_http_client) != ESP_OK
            || esp_http_client_set_url(s_http_client, uri) != ESP_OK) {
            upstream_discard_http(up);
            ESP_LOGW(TAG, "Failed to set upstream URL (attempt %d/2)", retry + 1);
            continue;
        } else {
            memcpy(s_http_url, uri, sizeof(s_http_url));
        }
        esp_http_client_handle_t http = s_http_client;
        if (!http) {
            ESP_LOGE(TAG, "Failed to initialize upstream HTTP client");
            return false;
        }
        esp_err_t auth_clear = esp_http_client_delete_header(http, "Authorization");
        bool headers_ok = (auth_clear == ESP_OK || auth_clear == ESP_ERR_NOT_FOUND)
            && esp_http_client_set_header(http, "User-Agent", "RockCast/0.1") == ESP_OK
            && esp_http_client_set_header(http, "Accept", "*/*") == ESP_OK
            && esp_http_client_set_header(http, "Icy-MetaData", "1") == ESP_OK;
        if (bearer[0]) {
            char authorization[sizeof(bearer) + 16];
            snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
            headers_ok = headers_ok
                && esp_http_client_set_header(http, "Authorization", authorization) == ESP_OK;
        }
        if (!headers_ok) {
            ESP_LOGW(TAG, "Failed to set upstream headers (attempt %d/2)", retry + 1);
            upstream_discard_http(up);
            continue;
        }
        if (esp_http_client_set_timeout_ms(http, up->http_cfg.timeout_ms) != ESP_OK) {
            upstream_discard_http(up);
            continue;
        }
        int64_t connect_start_us = esp_timer_get_time();
        esp_err_t open_err = esp_http_client_open(http, 0);
        int64_t connect_ms = (esp_timer_get_time() - connect_start_us) / 1000;
        ESP_LOGI(TAG, "Upstream connect: %lld ms, reused_handle=%d, result=%s",
            (long long)connect_ms, reused, esp_err_to_name(open_err));
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open upstream connection (attempt %d/2)", retry + 1);
            upstream_discard_http(up);
            if (retry + 1 < 2 && !relay_stopped()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        int64_t header_result = esp_http_client_fetch_headers(http);
        int status = esp_http_client_get_status_code(http);
        if (header_result < 0 || status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Upstream returned HTTP status %d (attempt %d/2)", status, retry + 1);
            upstream_discard_http(up);
            if (retry + 1 < 2 && !relay_stopped()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        up->http = http;
        up->icy.metadata_interval = up->headers.icy_metaint;
        up->icy.until_metadata = up->headers.icy_metaint;
        esp_http_client_set_timeout_ms(http, 5000);
        ESP_LOGI(TAG, "Upstream HTTP status %d OK (length=%lld chunked=%d type=%s icy-metaint=%u)",
            status,
            (long long)esp_http_client_get_content_length(http),
            esp_http_client_is_chunked_response(http),
            up->headers.content_type[0] ? up->headers.content_type : "?",
            (unsigned)up->headers.icy_metaint);
        return true;
    }
    ESP_LOGE(TAG, "Failed to open upstream connection after retries");
    return false;
}

static bool upstream_open_decoder(relay_upstream_t *up)
{
    if (up->dec) {
        return true;
    }
    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_audio_dec_cfg_t dec_cfg = {
        .type = ESP_AUDIO_TYPE_MP3,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    if (strcasestr(up->headers.content_type, "aac")) {
        // Radio "audio/aac" may contain HE-AAC (aacPlus).
        aac_cfg.aac_plus_enable = true;
        dec_cfg.type = ESP_AUDIO_TYPE_AAC;
        dec_cfg.cfg = &aac_cfg;
        dec_cfg.cfg_sz = sizeof(aac_cfg);
    }
    if (esp_audio_dec_open(&dec_cfg, &up->dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open upstream audio decoder (type=%d)", dec_cfg.type);
        return false;
    }
    up->dec_type = dec_cfg.type;
    return true;
}

// Pull more compressed audio from the live upstream into the staging buffer.
// Returns bytes appended (0 = nothing available right now) or -1 when the
// session cannot continue.
static int upstream_pump(relay_upstream_t *up, uint8_t *scratch, uint8_t *filtered)
{
    size_t room = RELAY_RAW_BUFFER_BYTES - up->raw_len;
    if (room == 0) {
        if (!up->mp3_synced && up->dec_type == ESP_AUDIO_TYPE_MP3) {
            // Staging full of non-audio bytes: slide one byte to keep searching.
            memmove(up->raw, up->raw + 1, --up->raw_len);
            room = 1;
        } else {
            return 0;
        }
    }
    size_t cap = room < RELAY_READ_CHUNK_BYTES ? room : RELAY_READ_CHUNK_BYTES;
    int64_t read_t0 = esp_timer_get_time();
    int rlen = esp_http_client_read(up->http, (char *)scratch, (int)cap);
    s_fstats.read_us += (uint32_t)(esp_timer_get_time() - read_t0);
    if (rlen > 0) {
        size_t audio_len = filter_icy_audio(&up->icy, scratch, (size_t)rlen, filtered, cap);
        memcpy(up->raw + up->raw_len, filtered, audio_len);
        up->raw_len += audio_len;
        s_fstats.in_bytes += audio_len;
        return (int)audio_len;
    }
    if ((rlen == 0 && esp_http_client_is_complete_data_received(up->http)) ||
        (rlen < 0 && rlen != -ESP_ERR_HTTP_EAGAIN && rlen != -ESP_ERR_HTTP_READ_TIMEOUT)) {
        ESP_LOGW(TAG, "Upstream stream ended or read error %d; reconnecting", rlen);
        if (rlen < 0 || esp_http_client_close(up->http) != ESP_OK) {
            upstream_discard_http(up);
        }
        for (int retry = 0; retry < 3 && !relay_stopped(); retry++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (upstream_connect(up)) {
                atomic_fetch_add(&s_upstream_reconnects, 1);
                up->mp3_synced = false;
                ESP_LOGI(TAG, "Upstream stream reconnected");
                return 0;
            }
        }
        return -1;
    }
    if (rlen == 0 || rlen == -ESP_ERR_HTTP_EAGAIN || rlen == -ESP_ERR_HTTP_READ_TIMEOUT) {
        return 0;
    }
    return -1;
}

// Decode one chunk of staged audio into 48 kHz stereo PCM.  Returns PCM bytes
// produced (0 = need more input) or -1 on a fatal decoder state.
static int upstream_decode_chunk(
    relay_upstream_t *up,
    uint8_t *pcm,
    uint8_t *wav
)
{
    if (!up->mp3_synced && up->dec_type == ESP_AUDIO_TYPE_MP3 && up->raw_len >= 4) {
        size_t offset = relay_find_mpeg_frame(up->raw, up->raw_len);
        if (offset < up->raw_len) {
            if (offset > 0) {
                ESP_LOGI(TAG, "Skipped %u non-audio bytes before first frame", (unsigned)offset);
                memmove(up->raw, up->raw + offset, up->raw_len - offset);
                up->raw_len -= offset;
            }
            up->mp3_synced = true;
        }
    }

    size_t consumed = 0;
    size_t produced = 0;
    while (consumed < up->raw_len) {
        size_t input_len = up->raw_len - consumed;
        if (up->dec_type == ESP_AUDIO_TYPE_AAC) {
            const uint8_t *adts = up->raw + consumed;
            if (input_len < 7) {
                break;
            }
            // Never pass a partial or malformed ADTS frame to the AAC library.
            // In particular, channel_config=0 selects a program configuration
            // element parser that has crashed on this board with a bad pointer.
            size_t frame_len = ((size_t)(adts[3] & 3) << 11)
                | ((size_t)adts[4] << 3) | (adts[5] >> 5);
            size_t header_len = (adts[1] & 1) ? 7 : 9;
            unsigned channel_config = ((adts[2] & 1) << 2) | (adts[3] >> 6);
            if (adts[0] != 0xff || (adts[1] & 0xf6) != 0xf0
                || (adts[2] & 0x3c) == 0x3c
                || channel_config == 0 || channel_config > 2
                || frame_len < header_len || frame_len > RELAY_RAW_BUFFER_BYTES
                || (adts[6] & 3) != 0) {
                consumed++;
                continue;
            }
            if (input_len < frame_len) {
                break;
            }
            input_len = frame_len;
            // ADTS fixed header: profile and sampling index live in byte 2
            // (b2[7:6]=profile, b2[5:2]=freq index), NOT byte 1.
            s_fstats.adts_profile = (adts[2] >> 6) & 0x3;
            s_fstats.adts_freq_idx = (adts[2] >> 2) & 0xF;
        }
        esp_audio_dec_in_raw_t in = {
            .buffer = up->raw + consumed,
            .len = (uint32_t)input_len,
            .consumed = 0,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = pcm,
            .len = RELAY_PCM_CHUNK_BYTES,
            .needed_size = 0,
            .decoded_size = 0,
        };
        int64_t dec_t0 = esp_timer_get_time();
        esp_audio_err_t decoded = esp_audio_dec_process(up->dec, &in, &frame);
        int64_t dec_us = esp_timer_get_time() - dec_t0;
        s_fstats.decode_us += (uint32_t)dec_us;
        if ((uint32_t)dec_us > s_fstats.decode_max_us) {
            s_fstats.decode_max_us = (uint32_t)dec_us;
        }
        if (decoded == ESP_AUDIO_ERR_DATA_LACK) {
            if (up->dec_type == ESP_AUDIO_TYPE_AAC) {
                consumed += input_len;
                continue;
            }
            break;
        }
        if (decoded == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGE(TAG, "Upstream PCM frame needs %lu bytes (capacity %u)",
                (unsigned long)frame.needed_size, (unsigned)RELAY_PCM_CHUNK_BYTES);
            return -1;
        }
        if (decoded == ESP_AUDIO_ERR_OK) {
            if (frame.decoded_size > 0) {
                s_fstats.frames++;
                s_fstats.session_frames++;
                if (frame.decoded_size > 4096) {
                    s_fstats.frames_large++;
                } else {
                    s_fstats.frames_small++;
                }
                if (frame.decoded_size > s_fstats.dec_size_max) {
                    s_fstats.dec_size_max = frame.decoded_size;
                }
                esp_audio_dec_info_t info = {0};
                if (esp_audio_dec_get_info(up->dec, &info) != ESP_AUDIO_ERR_OK
                    || info.sample_rate == 0 || info.channel == 0 || info.channel > 2) {
                    ESP_LOGE(TAG, "Decoder did not provide a supported PCM format");
                    return -1;
                }
                s_fstats.dec_rate = info.sample_rate;
                s_fstats.dec_bitrate = info.bitrate;
                s_fstats.dec_frame_size = info.frame_size;
                if (up->resampler.source_rate == 0) {
                    up->resampler.source_rate = info.sample_rate;
                    ESP_LOGI(TAG, "Upstream PCM format: %u Hz, %u ch",
                        (unsigned)info.sample_rate, (unsigned)info.channel);
                }
                produced = resample_to_48k_stereo(&up->resampler,
                    (const int16_t *)pcm, frame.decoded_size / (2 * info.channel), info.channel,
                    (int16_t *)wav, RELAY_WAV_CHUNK_BYTES / 4);
            }
            consumed += up->dec_type == ESP_AUDIO_TYPE_AAC
                ? input_len : (in.consumed ? in.consumed : 1);
            if (produced > 0) {
                break;
            }
        } else {
            if (up->dec_type == ESP_AUDIO_TYPE_AAC) {
                consumed += input_len;
                continue;
            }
            // Decode error: jump to the next frame sync (ADTS/MPEG 0xFFFx)
            // instead of crawling one byte per decoder call, which floods
            // the log and stalls the feeder on garbage input.
            size_t skip = consumed + 1;
            while (skip + 1 < up->raw_len &&
                   !(up->raw[skip] == 0xFF && (up->raw[skip + 1] & 0xF0) == 0xF0)) {
                skip++;
            }
            consumed = (skip + 1 < up->raw_len) ? skip : up->raw_len;
        }
    }
    if (consumed > 0) {
        memmove(up->raw, up->raw + consumed, up->raw_len - consumed);
        up->raw_len -= consumed;
    }
    return (int)produced;
}

// --------------------------------------------------------------- tasks ---

// Feeder: keep the ring filled from the single upstream connection at media
// pace and poll for new Cast clients when the relay listener is attached
// (one task per client, like rockcast).
static bool relay_prepare(uint8_t *scratch, uint8_t *filtered, uint8_t *pcm, uint8_t *wav);

static void relay_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Source feeder task started");

    uint8_t *scratch = heap_caps_malloc(RELAY_READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *filtered = heap_caps_malloc(RELAY_READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm = heap_caps_malloc(RELAY_PCM_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *wav = heap_caps_malloc(RELAY_WAV_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch || !filtered || !pcm || !wav) {
        ESP_LOGE(TAG, "Failed to allocate relay feeder buffers");
        goto finish;
    }
    if (!relay_prepare(scratch, filtered, pcm, wav)) {
        upstream_discard_http(&s_upstream);
        goto finish;
    }
    if (s_passthrough) {
        esp_audio_dec_cfg_t dec_cfg = {
            .type = ESP_AUDIO_TYPE_MP3,
            .cfg = NULL,
            .cfg_sz = 0,
        };
        if (esp_audio_dec_open(&dec_cfg, &s_shadow_mp3_dec) != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Failed to open shadow MP3 decoder for spectrum tap");
            s_shadow_mp3_dec = NULL;
        } else {
            ESP_LOGI(TAG, "Shadow MP3 decoder opened for spectrum tap");
        }
    }
    atomic_store(&s_relay_running, true);
    s_source_start_ok = true;
    xSemaphoreGive(s_source_started);

    int64_t feed_start_us = esp_timer_get_time();
    uint64_t pushed = s_ring.end;
    size_t shadow_len = 0;
    // Start the perf window clean: prefill-stage counters are not part of it.
    memset(&s_fstats, 0, sizeof(s_fstats));
    s_fstats.window_start_us = feed_start_us;
    s_fstats.ring_min = ROCK_RING_UNUSED;
    s_fstats.session_start_us = feed_start_us;
    while (!relay_stopped()) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_fstats.window_start_us >= 5000000) {
            if (!s_passthrough) {
                size_t tap_behind = s_tap_joined ? (size_t)(s_ring.end - s_tap_pos) : 0;
                int64_t session_ms = (now_us - s_fstats.session_start_us) / 1000;
                uint32_t fps_tenths = session_ms > 0
                    ? (uint32_t)(s_fstats.session_frames * 10000 / (uint64_t)session_ms)
                    : 0;
                ESP_LOGI(TAG,
                    "feeder %lld ms: read=%lu dec=%lu(max %lu us) pace=%lu in=%u out=%u frames=%u(s%u/l%u max %u B) adts p%u f%u dec=%luHz %lukbps fs=%u ring=%u..%u tap_behind=%u fps=%u.%u",
                    (long long)(now_us - s_fstats.window_start_us) / 1000,
                    (unsigned long)s_fstats.read_us / 1000,
                    (unsigned long)s_fstats.decode_us / 1000,
                    (unsigned long)s_fstats.decode_max_us,
                    (unsigned long)s_fstats.pace_us / 1000,
                    (unsigned)s_fstats.in_bytes, (unsigned)s_fstats.out_bytes,
                    (unsigned)s_fstats.frames,
                    (unsigned)s_fstats.frames_small, (unsigned)s_fstats.frames_large,
                    (unsigned)s_fstats.dec_size_max,
                    (unsigned)s_fstats.adts_profile, (unsigned)s_fstats.adts_freq_idx,
                    (unsigned long)s_fstats.dec_rate,
                    (unsigned long)(s_fstats.dec_bitrate / 1000),
                    (unsigned)s_fstats.dec_frame_size,
                    (unsigned)(s_fstats.ring_min == ROCK_RING_UNUSED ? 0 : s_fstats.ring_min) / 1024,
                    (unsigned)s_fstats.ring_max / 1024, (unsigned)tap_behind / 1024,
                    (unsigned)(fps_tenths / 10), (unsigned)(fps_tenths % 10));
            }
            uint64_t session_frames = s_fstats.session_frames;
            int64_t session_start_us = s_fstats.session_start_us;
            memset(&s_fstats, 0, sizeof(s_fstats));
            s_fstats.window_start_us = now_us;
            s_fstats.ring_min = ROCK_RING_UNUSED;
            s_fstats.session_frames = session_frames;
            s_fstats.session_start_us = session_start_us;
        }
        // Poll for new Cast clients without ever blocking the media pace.
        // Speaker-only sessions never attach a listener.
        relay_listener_poll();
        check_meta_pending();

        if (s_passthrough) {
            // Upstream sets the pace; the ring absorbs bursts (no transcode
            // clock to follow).  Mirrors run_feeder_passthrough.
            int pumped = upstream_pump(&s_upstream, scratch, filtered);
            if (pumped < 0) {
                ESP_LOGW(TAG, "Relay upstream failed; stopping feeder");
                break;
            }
            if (pumped > 0) {
                rock_ring_push(&s_ring, s_upstream.raw, s_upstream.raw_len);
                if (s_shadow_mp3_dec != NULL && s_pcm_tap_fn != NULL) {
                    if (shadow_len + s_upstream.raw_len > RELAY_WAV_CHUNK_BYTES) shadow_len = 0;
                    memcpy(wav + shadow_len, s_upstream.raw, s_upstream.raw_len);
                    shadow_len += s_upstream.raw_len;
                    size_t consumed = 0;
                    // MP3 decoder treats a truncated tail as a bad frame; keep lookahead for the next read.
                    while (shadow_len - consumed >= RELAY_READ_CHUNK_BYTES) {
                        esp_audio_dec_in_raw_t in = {
                            .buffer = wav + consumed,
                            .len = (uint32_t)(shadow_len - consumed),
                        };
                        esp_audio_dec_out_frame_t frame = {
                            .buffer = pcm,
                            .len = RELAY_PCM_CHUNK_BYTES,
                        };
                        esp_audio_err_t res = esp_audio_dec_process(s_shadow_mp3_dec, &in, &frame);
                        if (in.consumed > in.len || frame.decoded_size > frame.len) {
                            ESP_LOGW(TAG, "Shadow MP3 decoder returned an invalid frame");
                            consumed = shadow_len;
                            break;
                        }
                        if (res == ESP_AUDIO_ERR_DATA_LACK) {
                            consumed += in.consumed;
                            break;
                        }
                        if (res == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
                            esp_audio_dec_info_t info = {0};
                            if (esp_audio_dec_get_info(s_shadow_mp3_dec, &info) == ESP_AUDIO_ERR_OK
                                && info.sample_rate > 0 && (info.channel == 1 || info.channel == 2)) {
                                s_pcm_tap_fn(pcm, frame.decoded_size, info.sample_rate,
                                             (uint8_t)info.channel, s_pcm_tap_ctx);
                            }
                        }
                        if (in.consumed > 0) consumed += in.consumed;
                        else if (res == ESP_AUDIO_ERR_OK) break;
                        else consumed++;
                    }
                    memmove(wav, wav + consumed, shadow_len - consumed);
                    shadow_len -= consumed;
                }
                s_upstream.raw_len = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(15));
            }
            continue;
        }

        int produced = upstream_decode_chunk(&s_upstream, pcm, wav);
        if (produced < 0) {
            ESP_LOGW(TAG, "Relay decode failed; stopping feeder");
            break;
        }
        if (produced > 0) {
            int64_t pace_t0 = esp_timer_get_time();
            bool paced_ok = pace_before_push(pushed, (size_t)produced, feed_start_us);
            s_fstats.pace_us += (uint32_t)(esp_timer_get_time() - pace_t0);
            if (!paced_ok) {
                break;
            }
            if (s_pcm_tap_fn && produced > 0) {
                s_pcm_tap_fn(wav, (size_t)produced, 48000, 2, s_pcm_tap_ctx);
            }
            rock_ring_push(&s_ring, wav, (size_t)produced);
            pushed += (size_t)produced;
            s_fstats.out_bytes += (size_t)produced;
            size_t buffered = rock_ring_buffered(&s_ring);
            if (buffered < s_fstats.ring_min) {
                s_fstats.ring_min = buffered;
            }
            if (buffered > s_fstats.ring_max) {
                s_fstats.ring_max = buffered;
            }
            continue;
        }

        int pumped = upstream_pump(&s_upstream, scratch, filtered);
        if (pumped < 0) {
            ESP_LOGW(TAG, "Relay upstream failed; stopping feeder");
            break;
        }
        if (pumped == 0) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

finish:
    if (s_shadow_mp3_dec) {
        esp_audio_dec_close(s_shadow_mp3_dec);
        s_shadow_mp3_dec = NULL;
    }
    free(scratch);
    free(filtered);
    free(pcm);
    free(wav);

    relay_listener_close();
    upstream_teardown(&s_upstream);
    atomic_store(&s_relay_running, false);
    if (!s_source_start_ok) {
        xSemaphoreGive(s_source_started);
    }
    ESP_LOGI(TAG, "Relay task exited");
    // This task was created with plain xTaskCreatePinnedToCore (dynamically
    // allocated stack/TCB): it must self-delete with plain vTaskDelete.
    // vTaskDeleteWithCaps asserts (xTaskGetStaticBuffers) on non-WithCaps
    // tasks and panicked the board on the first clean source stop.
    vTaskDelete(NULL);
}

// Fill the ring before the listener starts.  PCM path prebuffers 1 s of
// 48 kHz stereo (rockcast wait_for_pcm_format + wait_for_data); passthrough
// prebuffers a small compressed cushion (rockcast min_bytes = 8 KiB).
static bool relay_prefill_ring(
    relay_upstream_t *up,
    uint8_t *scratch,
    uint8_t *filtered,
    uint8_t *pcm,
    uint8_t *wav
)
{
    size_t want = s_passthrough ? RELAY_PASSTHROUGH_PREFILL_BYTES : RELAY_PREFILL_BYTES;
    int64_t start_us = esp_timer_get_time();
    uint64_t deadline_ms = (uint64_t)start_us / 1000 + RELAY_PREFILL_WAIT_MS;
    while (rock_ring_buffered(&s_ring) < want && !relay_stopped()
        && (uint64_t)esp_timer_get_time() / 1000 < deadline_ms) {
        if (s_passthrough) {
            int pumped = upstream_pump(up, scratch, filtered);
            if (pumped < 0) {
                return false;
            }
            if (pumped > 0) {
                rock_ring_push(&s_ring, up->raw, up->raw_len);
                up->raw_len = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(15));
            }
            continue;
        }
        int produced = upstream_decode_chunk(up, pcm, wav);
        if (produced < 0) {
            return false;
        }
        if (produced > 0) {
            rock_ring_push(&s_ring, wav, (size_t)produced);
            continue;
        }
        int pumped = upstream_pump(up, scratch, filtered);
        if (pumped < 0) {
            return false;
        }
        if (pumped == 0) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    size_t buffered = rock_ring_buffered(&s_ring);
    if (buffered == 0) {
        ESP_LOGE(TAG, "Relay prefill failed: no audio within %d ms", RELAY_PREFILL_WAIT_MS);
        return false;
    }
    if (buffered < want) {
        ESP_LOGW(TAG, "Relay prefill partial: %u of %u bytes; continuing on live data",
            (unsigned)buffered, (unsigned)want);
    } else {
        ESP_LOGI(TAG, "Relay prefill ready: %u bytes (mode=%s)",
            (unsigned)buffered, s_passthrough ? "passthrough" : "wav");
    }
    return true;
}

static bool relay_prepare(uint8_t *scratch, uint8_t *filtered, uint8_t *pcm, uint8_t *wav)
{
    // Confidentiality: NEVER log s_upstream_uri or s_upstream_bearer.
    if (!upstream_connect(&s_upstream)) {
        return false;
    }

    const char *ct = s_upstream.headers.content_type;
    const char *url = s_upstream_uri;
    bool mp3 = strcasestr(ct, "mpeg") || strcasestr(ct, "mp3")
        || strcasestr(url, ".mp3") || strcasestr(url, "/mp3");
    bool aac = strcasestr(ct, "aac") != NULL;
    if (!mp3 && !aac) {
        ESP_LOGE(TAG, "Unsupported upstream Content-Type: %s", ct[0] ? ct : "(missing)");
        return false;
    }
    s_passthrough = mp3;
    if (mp3) {
        snprintf(s_content_type, sizeof(s_content_type), "%s", ct[0] ? ct : "audio/mpeg");
        if (!ct[0] || strcasecmp(ct, "application/octet-stream") == 0) {
            snprintf(s_content_type, sizeof(s_content_type), "audio/mpeg");
        }
        s_content_enum = ROCK_SOURCE_CT_MPEG;
    } else {
        snprintf(s_content_type, sizeof(s_content_type), "audio/wav");
        s_content_enum = ROCK_SOURCE_CT_AAC;
    }
    ESP_LOGI(TAG, "Source transport: %s (%s)", s_passthrough ? "passthrough" : "wav-pcm",
        s_content_type);
    return (s_passthrough || upstream_open_decoder(&s_upstream))
        && relay_prefill_ring(&s_upstream, scratch, filtered, pcm, wav);
}

esp_err_t rock_source_start(const char *upstream_uri, const char *bearer)
{
    if (!upstream_uri || !upstream_uri[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (atomic_load(&s_relay_running)) {
        rock_source_stop();
        if (atomic_load(&s_relay_running)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    // A previous source cycle leaves the stop flag asserted; without this
    // reset the prefill loop below would bail out instantly at 0 bytes.
    atomic_store(&s_relay_stop, false);
    atomic_store(&s_upstream_reconnects, 0);
    s_tap_joined = false;
    s_tap_pos = 0;
    clear_meta_title();

    if (!s_config_lock) {
        s_config_lock = xSemaphoreCreateMutex();
        if (!s_config_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    strncpy(s_upstream_uri, upstream_uri, sizeof(s_upstream_uri) - 1);
    s_upstream_uri[sizeof(s_upstream_uri) - 1] = '\0';
    if (bearer && bearer[0]) {
        strncpy(s_upstream_bearer, bearer, sizeof(s_upstream_bearer) - 1);
        s_upstream_bearer[sizeof(s_upstream_bearer) - 1] = '\0';
    } else {
        s_upstream_bearer[0] = '\0';
    }
    xSemaphoreGive(s_config_lock);

    if (!rock_ring_init(&s_ring)) {
        ESP_LOGE(TAG, "Failed to allocate source ring");
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
        s_upstream_uri[0] = '\0';
        s_upstream_bearer[0] = '\0';
        xSemaphoreGive(s_config_lock);
        return ESP_ERR_NO_MEM;
    }

    memset(&s_upstream, 0, sizeof(s_upstream));
    s_upstream.http_cfg = (esp_http_client_config_t){
        .url = s_upstream_uri,
        .timeout_ms = 25000,
        .disable_auto_redirect = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .event_handler = on_upstream_http_event,
        .user_data = &s_upstream.headers,
        .save_client_session = true,
    };
    s_upstream.raw = heap_caps_malloc(RELAY_RAW_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_upstream.raw) {
        ESP_LOGE(TAG, "Failed to allocate source staging buffer");
        goto fail;
    }

    relay_clients_reset();
    if (!s_source_started) {
        s_source_started = xSemaphoreCreateBinary();
        if (!s_source_started) {
            ESP_LOGE(TAG, "Failed to create source start signal");
            goto fail;
        }
    }
    s_source_start_ok = false;
    BaseType_t ret = xTaskCreatePinnedToCore(relay_task, "rock_relay", 12288, NULL, 5,
        NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn source feeder task");
        goto fail;
    }
    xSemaphoreTake(s_source_started, portMAX_DELAY);
    if (!s_source_start_ok) {
        goto fail_started;
    }

    ESP_LOGI(TAG, "Stream source started (%s)",
        s_passthrough ? "compressed passthrough" : "48 kHz stereo PCM");
    return ESP_OK;

fail:
    free(s_upstream.raw);
    s_upstream.raw = NULL;
fail_started:
    rock_ring_reset(&s_ring);
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_upstream_uri[0] = '\0';
    s_upstream_bearer[0] = '\0';
    xSemaphoreGive(s_config_lock);
    return ESP_FAIL;
}

esp_err_t rock_source_update_bearer(const char *bearer)
{
    if (!bearer || !bearer[0] || strpbrk(bearer, "\r\n")
        || strlen(bearer) >= sizeof(s_upstream_bearer)) {
        return ESP_ERR_INVALID_ARG;
    }
    // No source session has ever started: the stored bearer is dead config
    // that rock_source_start overwrites anyway, so there is nothing to refresh.
    if (!s_config_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    strncpy(s_upstream_bearer, bearer, sizeof(s_upstream_bearer) - 1);
    s_upstream_bearer[sizeof(s_upstream_bearer) - 1] = '\0';
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

bool rock_source_is_active(void)
{
    return atomic_load(&s_relay_running);
}

uint32_t rock_source_reconnect_count(void)
{
    return (uint32_t)atomic_load(&s_upstream_reconnects);
}

bool rock_source_is_pcm(void)
{
    return atomic_load(&s_relay_running) && !s_passthrough;
}

const char *rock_source_content_type(void)
{
    return s_content_type;
}

rock_source_content_type_t rock_source_content_type_enum(void)
{
    return s_content_enum;
}

int rock_source_content_type_copy(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return -1;
    }
    // Snapshot under the config lock: the Rust session task may call this
    // while a new source session is rewriting the string.
    if (s_config_lock) {
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
    }
    int n = snprintf(out, cap, "%s", s_content_type);
    if (s_config_lock) {
        xSemaphoreGive(s_config_lock);
    }
    return (n >= 0 && (size_t)n < cap) ? n : -1;
}

int rock_source_read(uint8_t *out, size_t cap)
{
    if (!out || cap == 0) {
        return -1;
    }
    if (!atomic_load(&s_relay_running) || !s_ring.lock) {
        return -1;
    }
    if (!s_tap_joined) {
        s_tap_pos = rock_ring_join_position(&s_ring, relay_cushion_bytes());
        // Full burst credit at join: the initial cushion is readable
        // immediately so the PCM preroll fills without a pacing delay.
        s_tap_budget = RELAY_TAP_BURST_BYTES;
        s_tap_last_refill_us = esp_timer_get_time();
        s_tap_joined = true;
        ESP_LOGI(TAG, "Speaker tap joined the %s ring behind the live edge",
            s_passthrough ? "compressed" : "PCM");
    }
    size_t n = 0;
    size_t read_cap = cap;
    if (!s_passthrough) {
        // Speaker jitter buffer: rate-limit the tap to the media clock
        // (token bucket at RELAY_PCM_BYTES_PER_SEC with a 1 s burst).  A
        // greedy tap pins to the live edge, the cushion never regrows after
        // a delivery dip and the DAC underruns within ~1.5 s of buffered
        // PCM.  With pacing, upstream bursts bank playable audio in the
        // ring (the feeder may lead the media clock by
        // RELAY_PACE_BANK_BYTES) and dips drain that bank smoothly.
        int64_t now_us = esp_timer_get_time();
        s_tap_budget += (now_us - s_tap_last_refill_us)
            * (int64_t)RELAY_PCM_BYTES_PER_SEC / 1000000;
        s_tap_last_refill_us = now_us;
        if (s_tap_budget > RELAY_TAP_BURST_BYTES) {
            s_tap_budget = RELAY_TAP_BURST_BYTES;
        }
        if (s_tap_budget < 0) {
            s_tap_budget = 0;
        }
        uint64_t ahead = s_ring.end - s_tap_pos;
        uint64_t paced = ahead < (uint64_t)s_tap_budget ? ahead : (uint64_t)s_tap_budget;
        // 48 kHz stereo 16-bit PCM: the grant must stay a whole-sample-frame
        // (4-byte) multiple or the player's PCM ring byte-shifts and the DAC
        // emits white noise until the stream restarts. Ring positions and
        // pushes are 4-byte aligned; only the token grant is arbitrary.
        paced &= ~3ULL;
        if (paced < read_cap) {
            read_cap = (size_t)paced;
        }
        if (read_cap == 0) {
            // Distinguish "nothing yet" from "source died between checks".
            if (!atomic_load(&s_relay_running)) {
                return -1;
            }
            return 0;
        }
    }
    rock_ring_read_result_t r = rock_ring_read(&s_ring, s_tap_pos, out, read_cap, &n);
    if (s_tap_budget > 0 && !s_passthrough) {
        s_tap_budget -= (int64_t)n;
    }
    if (r == ROCK_RING_READ_OVERRUN) {
        s_tap_pos = rock_ring_clamp_position(&s_ring, s_tap_pos, relay_cushion_bytes());
        return 0;
    }
    if (r == ROCK_RING_READ_EMPTY) {
        // Distinguish "nothing yet" from "source died between checks".
        if (!atomic_load(&s_relay_running)) {
            return -1;
        }
        return 0;
    }
    s_tap_pos += n;
    return (int)n;
}

void rock_source_stop(void)
{
    if (!atomic_load(&s_relay_running)) {
        return;
    }

    ESP_LOGI(TAG, "Stopping stream source...");
    atomic_store(&s_relay_stop, true);

    // The feeder may sit in a blocking upstream read (up to ~10 s), so join
    // longer than one heartbeat interval but keep the common case fast.
    int wait_ms = 0;
    while (atomic_load(&s_relay_running) && wait_ms < RELAY_STOP_JOIN_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }

    // Client tasks stream from the ring: they must be gone before it is
    // reset.  They exit within ~2 s of the stop flag (send timeout bound).
    int client_wait_ms = 0;
    while (relay_client_count() > 0 && client_wait_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        client_wait_ms += 50;
    }

    // Wake a lingering accept() without closing the fd: the feeder owns it
    // and closes it on exit (observing s_relay_running == false implies the
    // close already happened), which avoids fd-reuse races.
    relay_listener_wake();

    // Persistent ring: reset contents, keep the allocation for the next
    // source session so late consumers can never touch freed memory.
    rock_ring_reset(&s_ring);
    s_tap_joined = false;
    s_tap_pos = 0;
    // Clear the upstream config only when the feeder has exited: on a join
    // timeout it may still read the buffers on a reconnect attempt.  A missed
    // clear is harmless — rock_source_start overwrites both buffers before
    // any new session can use them.
    if (!atomic_load(&s_relay_running)) {
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
        s_upstream_uri[0] = '\0';
        s_upstream_bearer[0] = '\0';
        xSemaphoreGive(s_config_lock);
    }
    clear_meta_title();
    ESP_LOGI(TAG, "Stream source stopped");
}

uint32_t rock_source_get_meta_title(char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return 0;
    }
    check_meta_pending();
    if (s_config_lock) {
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
    }
    strncpy(buf, s_meta_title, cap - 1);
    buf[cap - 1] = '\0';
    uint32_t gen = (uint32_t)atomic_load(&s_meta_generation);
    if (s_config_lock) {
        xSemaphoreGive(s_config_lock);
    }
    return gen;
}

uint32_t rock_source_meta_generation(void)
{
    check_meta_pending();
    return (uint32_t)atomic_load(&s_meta_generation);
}

void rock_source_clear_meta_title(void)
{
    clear_meta_title();
}

void rock_source_set_pcm_tap(rock_source_pcm_tap_fn tap_fn, void *ctx)
{
    s_pcm_tap_fn = tap_fn;
    s_pcm_tap_ctx = ctx;
}
