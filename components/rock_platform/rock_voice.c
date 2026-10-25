// One bounded push-to-talk session beside the long-lived device control WSS.
#include "control_platform.h"
#include "audio_bsp.h"
#include "rock_player.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VOICE_MAX_SAMPLES (16000 * 13)
#define VOICE_WINDOW_SAMPLES 1600
#define VOICE_FRAME_CAP 65536

#define VOICE_CONNECTING 1
#define VOICE_LISTENING 2
#define VOICE_PROCESSING 3
#define VOICE_SUCCESS 4
#define VOICE_SILENCE 5
#define VOICE_AMBIGUOUS 6
#define VOICE_UNSUPPORTED 7
#define VOICE_TIMEOUT 8
#define VOICE_OFFLINE 9
#define VOICE_RECOGNITION_ERROR 10
#define VOICE_COMMAND_FAILED 11
#define VOICE_CANCELLED 12
#define VOICE_RESOURCE_ERROR 13
#define VOICE_INTENT_ERROR 14

static const char *TAG = "rock-voice";

typedef struct { int kind; int state; } voice_event_t;
static atomic_int s_state;
static atomic_bool s_cancel;
static atomic_bool s_running;
static atomic_bool s_token_ready;
static atomic_bool s_paused_for_voice;
static atomic_int_fast64_t s_pressed_at;
static QueueHandle_t s_events;
static char *s_frame;
static char s_token[512];
static char s_transcript[513];
static int16_t *s_preroll;
static size_t s_preroll_count;

static void log_heap(const char *phase)
{
    ESP_LOGI(TAG, "%s heap internal=%u largest=%u psram=%u largest=%u", phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void post_event(int kind, int state)
{
    voice_event_t event = { kind, state };
    if (s_events) xQueueSend(s_events, &event, 0);
}

static int error_state(const char *code)
{
    if (!code) return VOICE_RECOGNITION_ERROR;
    if (!strcmp(code, "speech_not_recognized")) return VOICE_SILENCE;
    if (!strcmp(code, "intent_resolution_failed")) return VOICE_INTENT_ERROR;
    if (!strcmp(code, "clarification_required")) return VOICE_AMBIGUOUS;
    if (!strcmp(code, "unsupported_intent") || !strcmp(code, "station_not_found")) return VOICE_UNSUPPORTED;
    if (strstr(code, "timeout")) return VOICE_TIMEOUT;
    if (!strcmp(code, "target_offline") || !strcmp(code, "search_unavailable")) return VOICE_OFFLINE;
    if (!strcmp(code, "cancelled")) return VOICE_CANCELLED;
    return VOICE_RECOGNITION_ERROR;
}

static void voice_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WEBSOCKET_EVENT_CONNECTED) { post_event(1, 0); return; }
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_CLOSED) {
        const esp_websocket_event_data_t *event = data;
        if (event) ESP_LOGW(TAG, "socket event=%ld http=%d close=%d", (long)id,
                            event->error_handle.esp_ws_handshake_status_code, event->close_status_code);
        post_event(3, VOICE_OFFLINE);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA) return;
    esp_websocket_event_data_t *event = data;
    if (event->op_code != 1 || event->payload_len <= 0 ||
        event->payload_offset < 0 || event->data_len < 0 ||
        event->payload_offset + event->data_len > event->payload_len) return;
    if (event->payload_len >= VOICE_FRAME_CAP) {
        post_event(3, VOICE_RECOGNITION_ERROR);
        return;
    }
    memcpy(s_frame + event->payload_offset, event->data_ptr, event->data_len);
    if (!event->fin || event->payload_offset + event->data_len != event->payload_len) return;
    s_frame[event->payload_len] = 0;
    cJSON *root = cJSON_Parse(s_frame);
    if (!root) { post_event(3, VOICE_RECOGNITION_ERROR); return; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        if (!strcmp(type->valuestring, "ready")) post_event(2, 0);
        else if (!strcmp(type->valuestring, "error")) {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
            ESP_LOGW(TAG, "server error code=%s", cJSON_IsString(code) ? code->valuestring : "(missing)");
            post_event(3, error_state(cJSON_IsString(code) ? code->valuestring : NULL));
        } else if (!strcmp(type->valuestring, "result")) {
            const cJSON *transcript = cJSON_GetObjectItemCaseSensitive(root, "transcript");
            if (cJSON_IsString(transcript) && transcript->valuestring[0] &&
                strlen(transcript->valuestring) < sizeof(s_transcript)) {
                strcpy(s_transcript, transcript->valuestring);
                post_event(3, VOICE_SUCCESS);
            } else {
                post_event(3, VOICE_RECOGNITION_ERROR);
            }
        }
    }
    cJSON_Delete(root);
}

static bool wait_event(int kind, uint32_t timeout_ms, int *terminal)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms && !atomic_load(&s_cancel)) {
        voice_event_t event;
        if (xQueueReceive(s_events, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (event.kind == 3) { *terminal = event.state; return kind == 3; }
            if (event.kind == kind) return true;
        }
        elapsed += 100;
    }
    *terminal = atomic_load(&s_cancel) ? VOICE_CANCELLED : VOICE_TIMEOUT;
    return false;
}

// Capture starts before the network handshake, so the first spoken words are
// kept even when TLS takes several seconds. Only bounded PCM lives in PSRAM.
static bool capture_utterance(int16_t *recording, size_t *recorded, int *terminal)
{
    int16_t samples[1600];
    uint32_t phase = 0, previous_rate = 0;
    uint8_t previous_channels = 0;
    uint64_t window_squares = 0, absolute_sum = 0;
    uint32_t window_samples = 0, noise = UINT32_MAX;
    uint32_t first_speech = *recorded ? 1 : 0;
    uint32_t last_speech = (uint32_t)(*recorded / 16);
    int peak = 0;
    int64_t started = esp_timer_get_time();
    while (*recorded < VOICE_MAX_SAMPLES && !atomic_load(&s_cancel)) {
        if (esp_timer_get_time() - started > 14000000) { *terminal = VOICE_TIMEOUT; return false; }
        uint32_t rate;
        uint8_t channels;
        rock_audio_capture_format(&rate, &channels);
        if (rate < 16000 || rate > 96000 || (channels != 1 && channels != 2)) {
            *terminal = VOICE_RECOGNITION_ERROR;
            return false;
        }
        if (rate != previous_rate || channels != previous_channels) {
            phase = 0; previous_rate = rate; previous_channels = channels;
        }
        if (rock_audio_capture_read(samples, sizeof(samples)) != sizeof(samples)) {
            ESP_LOGW(TAG, "microphone read failed");
            *terminal = VOICE_RECOGNITION_ERROR;
            return false;
        }
        size_t out = 0;
        bool left_only = false, right_only = false;
        if (channels == 2) {
            uint32_t left = 0, right = 0;
            for (size_t frame = 0; frame < 800; frame++) {
                left += (unsigned)(samples[2 * frame] < 0 ? -(int)samples[2 * frame] : samples[2 * frame]);
                right += (unsigned)(samples[2 * frame + 1] < 0 ? -(int)samples[2 * frame + 1] : samples[2 * frame + 1]);
            }
            left_only = left > right * 4;
            right_only = right > left * 4;
        }
        for (size_t frame = 0; frame < 1600 / channels; frame++) {
            int16_t sample = channels == 1 ? samples[frame] :
                left_only ? samples[2 * frame] : right_only ? samples[2 * frame + 1] :
                (int16_t)(((int32_t)samples[2 * frame] + samples[2 * frame + 1]) / 2);
            phase += 16000;
            if (phase >= rate) { phase -= rate; samples[out++] = sample; }
        }
        if (out > VOICE_MAX_SAMPLES - *recorded) out = VOICE_MAX_SAMPLES - *recorded;
        memcpy(recording + *recorded, samples, out * sizeof(int16_t));
        for (size_t i = 0; i < out; i++) {
            int sample = samples[i];
            int magnitude = sample < 0 ? -sample : sample;
            absolute_sum += magnitude;
            if (magnitude > peak) peak = magnitude;
            window_squares += (int64_t)sample * sample;
            if (++window_samples == VOICE_WINDOW_SAMPLES) {
                uint32_t elapsed = (uint32_t)((*recorded + i + 1) / 16);
                uint32_t rms = (uint32_t)sqrt((double)window_squares / VOICE_WINDOW_SAMPLES);
                window_squares = 0;
                window_samples = 0;
                if (elapsed < 350) {
                    if (rms < noise) noise = rms;
                } else {
                    uint32_t threshold = noise == UINT32_MAX ? 340 : noise + 140;
                    if (threshold < 280) threshold = 280;
                    if (threshold > 520) threshold = 520;
                    if (rms >= threshold) {
                        if (!first_speech) first_speech = elapsed;
                        last_speech = elapsed;
                    }
                    if (first_speech && (elapsed - last_speech >= 1000 || elapsed - first_speech >= 8000)) {
                        *recorded += out;
                        ESP_LOGI(TAG, "capture speech samples=%u mean_abs=%u peak=%d", (unsigned)*recorded,
                                 (unsigned)(absolute_sum / *recorded), peak);
                        return true;
                    }
                    if (!first_speech && elapsed >= 4500) {
                        *terminal = VOICE_SILENCE;
                        return false;
                    }
                }
            }
        }
        *recorded += out;
    }
    if (atomic_load(&s_cancel)) { *terminal = VOICE_CANCELLED; return false; }
    if (!first_speech) { *terminal = VOICE_SILENCE; return false; }
    ESP_LOGI(TAG, "capture limit samples=%u mean_abs=%u peak=%d", (unsigned)*recorded,
             (unsigned)(absolute_sum / *recorded), peak);
    return true;
}

static void voice_task(void *arg)
{
    (void)arg;
    int terminal = VOICE_OFFLINE;
    esp_websocket_client_handle_t ws = NULL;
    int16_t *recording = NULL;
    size_t recorded = 0;
    if (rock_audio_init() != 0) { terminal = VOICE_RECOGNITION_ERROR; goto done; }
    recording = heap_caps_malloc(VOICE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording) { terminal = VOICE_RESOURCE_ERROR; goto done; }
    if (s_preroll && s_preroll_count) {
        recorded = s_preroll_count;
        memcpy(recording, s_preroll, recorded * sizeof(int16_t));
        ESP_LOGI(TAG, "wake pre-roll samples=%u", (unsigned)recorded);
        rock_voice_clear_preroll();
    }
    s_frame = heap_caps_malloc(VOICE_FRAME_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) { terminal = VOICE_RESOURCE_ERROR; goto done; }
    atomic_store(&s_state, VOICE_LISTENING);
    ESP_LOGI(TAG, "capture started %lld ms after button", (long long)((esp_timer_get_time() - atomic_load(&s_pressed_at)) / 1000));
    if (!capture_utterance(recording, &recorded, &terminal)) goto done;
    atomic_store(&s_state, VOICE_PROCESSING);
    log_heap("after capture");
    int64_t token_deadline = esp_timer_get_time() + 20000000;
    while (!atomic_load(&s_token_ready) && !atomic_load(&s_cancel) && esp_timer_get_time() < token_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (atomic_load(&s_cancel)) { terminal = VOICE_CANCELLED; goto done; }
    if (!atomic_load(&s_token_ready)) { terminal = VOICE_OFFLINE; goto done; }
    const char *base = rock_server_base_url();
    const char *host = NULL;
    const char *scheme = NULL;
    if (!strncmp(base, "https://", 8)) { host = base + 8; scheme = "wss"; }
    else if (!strncmp(base, "http://", 7)) { host = base + 7; scheme = "ws"; }
    if (!host) goto done;
    char uri[512], headers[540];
    if (snprintf(uri, sizeof(uri), "%s://%s/api/v1/voice/stream", scheme, host) >= sizeof(uri) ||
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", s_token) >= sizeof(headers)) goto done;
    esp_websocket_client_config_t cfg = {
        .uri = uri, .headers = headers, .disable_auto_reconnect = true,
        .task_stack = 4096, .buffer_size = 2048, .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_events = xQueueCreate(8, sizeof(voice_event_t));
    if (!s_events) { log_heap("event queue allocation failed"); terminal = VOICE_RESOURCE_ERROR; goto done; }
    ws = esp_websocket_client_init(&cfg);
    if (!ws) { log_heap("websocket init failed"); terminal = VOICE_RESOURCE_ERROR; goto done; }
    log_heap("before websocket start");
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, voice_ws_event, NULL);
    if (esp_websocket_client_start(ws) != ESP_OK) { log_heap("websocket start failed"); terminal = VOICE_RESOURCE_ERROR; goto done; }
    if (!wait_event(1, 10000, &terminal)) goto done;
    const char *start = "{\"type\":\"start\",\"locale\":\"ru-RU\",\"sample_rate_hz\":16000,\"recognizer_mode\":\"buffered_v1\",\"limit\":10}";
    if (esp_websocket_client_send_text(ws, start, strlen(start), pdMS_TO_TICKS(1000)) < 0 ||
        !wait_event(2, 15000, &terminal)) goto done;
    for (size_t offset = 0; offset < recorded && !atomic_load(&s_cancel); offset += 800) {
        size_t count = recorded - offset;
        if (count > 800) count = 800;
        if (esp_websocket_client_send_bin(ws, (const char *)(recording + offset), count * 2,
                                          pdMS_TO_TICKS(3000)) < 0) {
            terminal = VOICE_OFFLINE;
            goto done;
        }
    }
    if (atomic_load(&s_cancel)) { terminal = VOICE_CANCELLED; goto done; }
    heap_caps_free(recording);
    recording = NULL;
    const char *commit = "{\"type\":\"commit\"}";
    if (esp_websocket_client_send_text(ws, commit, strlen(commit), pdMS_TO_TICKS(1000)) < 0) goto done;
    wait_event(3, 45000, &terminal);
done:
    rock_voice_clear_preroll();
    if (recording) heap_caps_free(recording);
    if (ws) {
        if (atomic_load(&s_cancel) && esp_websocket_client_is_connected(ws)) {
            const char *cancel = "{\"type\":\"cancel\"}";
            esp_websocket_client_send_text(ws, cancel, strlen(cancel), pdMS_TO_TICKS(500));
        }
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    if (s_events) { vQueueDelete(s_events); s_events = NULL; }
    if (s_frame) { heap_caps_free(s_frame); s_frame = NULL; }
    memset(s_token, 0, sizeof(s_token));
    atomic_store(&s_token_ready, false);
    if (atomic_exchange(&s_paused_for_voice, false)) rock_player_play();
    ESP_LOGI(TAG, "session finished state=%d", terminal);
    atomic_store(&s_state, terminal);
    atomic_store(&s_running, false);
    vTaskDeleteWithCaps(NULL);
}

int rock_voice_toggle_capture(void)
{
    if (atomic_load(&s_running)) {
        atomic_store(&s_cancel, true);
        return 1;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_running, &expected, true)) return -1;
    atomic_store(&s_cancel, false);
    s_transcript[0] = 0;
    atomic_store(&s_token_ready, false);
    atomic_store(&s_state, VOICE_CONNECTING);
    atomic_store(&s_pressed_at, esp_timer_get_time());
    atomic_store(&s_paused_for_voice, rock_player_pause() == 0);
    if (xTaskCreateWithCaps(voice_task, "rock_voice", 8192, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "voice task allocation failed internal=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (atomic_exchange(&s_paused_for_voice, false)) rock_player_play();
        atomic_store(&s_state, VOICE_RESOURCE_ERROR);
        atomic_store(&s_running, false);
        return -1;
    }
    return 0;
}

int rock_voice_set_preroll(const int16_t *ring, size_t capacity, size_t used, size_t next)
{
    if (!ring || !capacity || used > capacity || used > 32000 || next >= capacity ||
        atomic_load(&s_running)) return -1;
    int16_t *copy = heap_caps_malloc(used * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return -1;
    size_t start = (next + capacity - used) % capacity;
    for (size_t i = 0; i < used; ++i) copy[i] = ring[(start + i) % capacity];
    rock_voice_clear_preroll();
    s_preroll = copy;
    s_preroll_count = used;
    return 0;
}

void rock_voice_clear_preroll(void)
{
    if (s_preroll) heap_caps_free(s_preroll);
    s_preroll = NULL;
    s_preroll_count = 0;
}

int rock_voice_start(const char *access_token)
{
    if (!access_token || !access_token[0] || strlen(access_token) >= sizeof(s_token) ||
        !atomic_load(&s_running) || atomic_load(&s_cancel)) return -1;
    strcpy(s_token, access_token);
    atomic_store(&s_token_ready, true);
    ESP_LOGI(TAG, "token attached %lld ms after button", (long long)((esp_timer_get_time() - atomic_load(&s_pressed_at)) / 1000));
    return 0;
}

int rock_voice_take_transcript(char *out, size_t cap)
{
    size_t len = strlen(s_transcript);
    if (atomic_load(&s_state) != VOICE_SUCCESS || !len || len >= cap) return -1;
    memcpy(out, s_transcript, len + 1);
    s_transcript[0] = 0;
    return 0;
}

void rock_voice_cancel(void) { atomic_store(&s_cancel, true); }
void rock_voice_release_pause(void) { atomic_store(&s_paused_for_voice, false); }
int rock_voice_state(void) { return atomic_load(&s_state); }
