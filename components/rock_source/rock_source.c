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
#define RELAY_PCM_CHUNK_BYTES 8192
#define RELAY_WAV_CHUNK_BYTES 16384
#define RELAY_PCM_BYTES_PER_SEC (48u * 1000u * 2u * 2u)
// Pacing slice, mirrors the desktop relay's pace_pcm_chunk() granularity.
#define RELAY_PACE_SLICE_MS 20
#define RELAY_STOP_JOIN_MS 12000
// Passthrough prefill before the listener starts (rockcast min_bytes for
// non-WAV transports).
#define RELAY_PASSTHROUGH_PREFILL_BYTES (4 * 1024)
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

static relay_upstream_t s_upstream = {0};
static rock_ring_t s_ring = {0};
// Speaker consumer tap: ring position once it has joined the live edge.
// Ring itself is persistent (allocated once, reset on source restart) so a
// consumer can never touch a freed buffer.
static bool s_tap_joined = false;
static uint64_t s_tap_pos = 0;

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
        int64_t allowed = (int64_t)RELAY_PREFILL_BYTES
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

static void upstream_teardown(relay_upstream_t *up)
{
    esp_http_client_handle_t http = up->http;
    up->http = NULL;
    if (http) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
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
        // copy only lives across esp_http_client_init, which parses it
        // synchronously into its own storage.
        char uri[sizeof(s_upstream_uri)];
        char bearer[sizeof(s_upstream_bearer)] = {0};
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
        memcpy(uri, s_upstream_uri, sizeof(uri));
        memcpy(bearer, s_upstream_bearer, sizeof(bearer));
        xSemaphoreGive(s_config_lock);
        esp_http_client_config_t http_cfg = up->http_cfg;
        http_cfg.url = uri;

        esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
        if (!http) {
            ESP_LOGE(TAG, "Failed to initialize upstream HTTP client");
            return false;
        }
        esp_http_client_set_header(http, "User-Agent", "RockCast/0.1");
        esp_http_client_set_header(http, "Accept", "*/*");
        esp_http_client_set_header(http, "Icy-MetaData", "1");
        if (bearer[0]) {
            char authorization[sizeof(bearer) + 16];
            snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
            esp_http_client_set_header(http, "Authorization", authorization);
        }
        if (esp_http_client_open(http, 0) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open upstream connection (attempt %d/2)", retry + 1);
            esp_http_client_cleanup(http);
            if (retry + 1 < 2 && !relay_stopped()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        esp_http_client_fetch_headers(http);
        int status = esp_http_client_get_status_code(http);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Upstream returned HTTP status %d (attempt %d/2)", status, retry + 1);
            esp_http_client_close(http);
            esp_http_client_cleanup(http);
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
        // Radio "audio/aac" streams are typically HE-AAC (aacPlus): without
        // SBR/PS enabled the decoder rejects every frame (error 30) and the
        // feeder starves with an error storm.
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
    int rlen = esp_http_client_read(up->http, (char *)scratch, (int)cap);
    if (rlen > 0) {
        size_t audio_len = filter_icy_audio(&up->icy, scratch, (size_t)rlen, filtered, cap);
        memcpy(up->raw + up->raw_len, filtered, audio_len);
        up->raw_len += audio_len;
        return (int)audio_len;
    }
    if ((rlen == 0 && esp_http_client_is_complete_data_received(up->http)) ||
        (rlen < 0 && rlen != -ESP_ERR_HTTP_EAGAIN && rlen != -ESP_ERR_HTTP_READ_TIMEOUT)) {
        ESP_LOGW(TAG, "Upstream stream ended or read error %d; reconnecting", rlen);
        esp_http_client_close(up->http);
        esp_http_client_cleanup(up->http);
        up->http = NULL;
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
        esp_audio_dec_in_raw_t in = {
            .buffer = up->raw + consumed,
            .len = (uint32_t)(up->raw_len - consumed),
            .consumed = 0,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = pcm,
            .len = RELAY_PCM_CHUNK_BYTES,
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t decoded = esp_audio_dec_process(up->dec, &in, &frame);
        if (decoded == ESP_AUDIO_ERR_DATA_LACK) {
            break;
        }
        if (decoded == ESP_AUDIO_ERR_OK || decoded == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (frame.decoded_size > 0) {
                esp_audio_dec_info_t info = {0};
                if (esp_audio_dec_get_info(up->dec, &info) != ESP_AUDIO_ERR_OK
                    || info.sample_rate == 0 || info.channel == 0 || info.channel > 2) {
                    ESP_LOGE(TAG, "Decoder did not provide a supported PCM format");
                    return -1;
                }
                if (up->resampler.source_rate == 0) {
                    up->resampler.source_rate = info.sample_rate;
                    ESP_LOGI(TAG, "Upstream PCM format: %u Hz, %u ch",
                        (unsigned)info.sample_rate, (unsigned)info.channel);
                }
                produced = resample_to_48k_stereo(&up->resampler,
                    (const int16_t *)pcm, frame.decoded_size / (2 * info.channel), info.channel,
                    (int16_t *)wav, RELAY_WAV_CHUNK_BYTES / 4);
            }
            consumed += in.consumed ? in.consumed : 1;
            if (produced > 0) {
                break;
            }
        } else {
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
        free(scratch);
        free(filtered);
        free(pcm);
        free(wav);
        goto exit;
    }

    int64_t feed_start_us = esp_timer_get_time();
    uint64_t pushed = s_ring.end;
    while (!relay_stopped()) {
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
            if (!pace_before_push(pushed, (size_t)produced, feed_start_us)) {
                break;
            }
            rock_ring_push(&s_ring, wav, (size_t)produced);
            pushed += (size_t)produced;
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

    free(scratch);
    free(filtered);
    free(pcm);
    free(wav);

exit:
    relay_listener_close();
    upstream_teardown(&s_upstream);
    atomic_store(&s_relay_running, false);
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

    uint8_t *scratch = heap_caps_malloc(RELAY_READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *filtered = heap_caps_malloc(RELAY_READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm = heap_caps_malloc(RELAY_PCM_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *wav = heap_caps_malloc(RELAY_WAV_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch || !filtered || !pcm || !wav) {
        ESP_LOGE(TAG, "Failed to allocate source prefill buffers");
        goto fail;
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
    };
    s_upstream.raw = heap_caps_malloc(RELAY_RAW_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_upstream.raw) {
        ESP_LOGE(TAG, "Failed to allocate source staging buffer");
        goto fail;
    }

    // Confidentiality: NEVER log s_upstream_uri or s_upstream_bearer.
    if (!upstream_connect(&s_upstream)) {
        goto fail;
    }

    // Transport choice mirrors rockcast choose_transport: MP3 goes through
    // as audio/mpeg (the weak radio link cannot carry a 6x heavier WAV
    // transcode), anything else (AAC/unknown) still transcodes to WAV PCM.
    // Speaker consumers read PCM directly in transcode mode and decode the
    // compressed ring themselves in passthrough mode.
    {
        const char *ct = s_upstream.headers.content_type;
        const char *url = s_upstream_uri;
        bool mp3 = strcasestr(ct, "mpeg") || strcasestr(ct, "mp3")
            || strcasestr(url, ".mp3") || strcasestr(url, "/mp3");
        s_passthrough = mp3;
        if (mp3) {
            snprintf(s_content_type, sizeof(s_content_type), "%s",
                ct[0] ? ct : "audio/mpeg");
            // normalize_content_type: unknown/octet-stream defaults to mpeg
            if (!ct[0] || strcasecmp(ct, "application/octet-stream") == 0) {
                snprintf(s_content_type, sizeof(s_content_type), "audio/mpeg");
            }
            s_content_enum = ROCK_SOURCE_CT_MPEG;
        } else {
            snprintf(s_content_type, sizeof(s_content_type), "audio/wav");
            // Transcode mode always serves WAV; keep the upstream family in
            // the enum for logs/diagnostics.
            if (strcasestr(ct, "wav")) {
                s_content_enum = ROCK_SOURCE_CT_WAV;
            } else if (strcasestr(ct, "aac")) {
                s_content_enum = ROCK_SOURCE_CT_AAC;
            } else {
                s_content_enum = ROCK_SOURCE_CT_UNKNOWN;
            }
        }
    }
    ESP_LOGI(TAG, "Source transport: %s (%s)", s_passthrough ? "passthrough" : "wav-pcm",
        s_content_type);

    if ((!s_passthrough && !upstream_open_decoder(&s_upstream))
        || !relay_prefill_ring(&s_upstream, scratch, filtered, pcm, wav)) {
        goto fail;
    }

    free(scratch);
    free(filtered);
    free(pcm);
    free(wav);

    relay_clients_reset();
    atomic_store(&s_relay_running, true);

    BaseType_t ret = xTaskCreatePinnedToCore(relay_task, "rock_relay", 6144, NULL, 5,
        NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn source feeder task");
        atomic_store(&s_relay_running, false);
        goto fail_no_buffers;
    }

    ESP_LOGI(TAG, "Stream source started (%s)",
        s_passthrough ? "compressed passthrough" : "48 kHz stereo PCM");
    return ESP_OK;

fail_no_buffers:
    atomic_store(&s_relay_running, false);
    upstream_teardown(&s_upstream);
    rock_ring_reset(&s_ring);
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_upstream_uri[0] = '\0';
    s_upstream_bearer[0] = '\0';
    xSemaphoreGive(s_config_lock);
    return ESP_FAIL;

fail:
    free(scratch);
    free(filtered);
    free(pcm);
    free(wav);
    upstream_teardown(&s_upstream);
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
        s_tap_joined = true;
        ESP_LOGI(TAG, "Speaker tap joined the %s ring behind the live edge",
            s_passthrough ? "compressed" : "PCM");
    }
    size_t n = 0;
    rock_ring_read_result_t r = rock_ring_read(&s_ring, s_tap_pos, out, cap, &n);
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
