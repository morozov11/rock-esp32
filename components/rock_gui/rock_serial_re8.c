// RE-8 serial protocol: dataset capture (RE8CAP), wake detector control and
// the last-crossing forensic dump (RE8WAKE). Extracted from rock_serial.c so
// the console stays a thin dispatcher and all audio-transfer framing lives
// here. The exact log/response formats are a contract of the tools/re8_*.py
// scripts — keep verbatim. Persistence of the mute flag is owned by the
// rock_gui facade (rock_gui_set_wake_muted), not by this transport module.

#include "rock_serial_re8.h"
#include "control_platform.h"
#include "rock_gui.h"
#include "rock_wake_probe.h"
#include "audio_bsp.h"

#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-serial";

// RE-8 dataset capture is intentionally a serial-only diagnostic path.  It
// records on the board, then sends bounded base64 chunks to the host after the
// microphone has been released.  It never opens the voice WebSocket.
#define RE8_CAPTURE_RATE_HZ       16000u
#define RE8_CAPTURE_DELAY_MS      2000u
#define RE8_CAPTURE_DURATION_MS   8000u
#define RE8_CAPTURE_MIN_DURATION_MS 5000u
#define RE8_CAPTURE_MAX_DURATION_MS 120000u
#define RE8_CAPTURE_READ_BYTES    1600u
#define RE8_CAPTURE_DATA_BYTES     240u
#define RE8_CAPTURE_LABEL_CAP       33u

typedef struct {
    uint32_t id;
    uint32_t duration_ms;
    char label[RE8_CAPTURE_LABEL_CAP];
} re8_capture_job_t;

static atomic_bool s_re8_capture_active;
static atomic_uint s_re8_capture_next_id;

static bool re8_capture_label_valid(const char *label)
{
    if (!label || !label[0] || strlen(label) >= RE8_CAPTURE_LABEL_CAP) return false;
    for (const unsigned char *p = (const unsigned char *)label; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static uint32_t re8_capture_fnv1a(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void re8_capture_task(void *arg)
{
    rock_wake_set_paused(true);
    re8_capture_job_t *job = arg;
    const size_t total_bytes = (size_t)RE8_CAPTURE_RATE_HZ * job->duration_ms / 1000u * sizeof(int16_t);
    uint8_t *recording = NULL;
    uint32_t rate = 0;
    uint8_t channels = 0;

    if (rock_audio_init() != 0) {
        ESP_LOGE(TAG, "RE8CAP error id=%lu reason=audio_init", (unsigned long)job->id);
        goto done;
    }
    rock_audio_capture_format(&rate, &channels);
    if (rate < RE8_CAPTURE_RATE_HZ || rate > 96000 || (channels != 1 && channels != 2)) {
        ESP_LOGE(TAG, "RE8CAP error id=%lu reason=format rate=%lu channels=%u",
                 (unsigned long)job->id, (unsigned long)rate, (unsigned)channels);
        goto done;
    }
    recording = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording) {
        ESP_LOGE(TAG, "RE8CAP error id=%lu reason=no_psram bytes=%u", (unsigned long)job->id,
                 (unsigned)total_bytes);
        goto done;
    }

    ESP_LOGI(TAG, "RE8CAP ready id=%lu label=%s delay_ms=%u duration_ms=%u",
             (unsigned long)job->id, job->label, RE8_CAPTURE_DELAY_MS, (unsigned)job->duration_ms);
    vTaskDelay(pdMS_TO_TICKS(RE8_CAPTURE_DELAY_MS));
    ESP_LOGI(TAG, "RE8CAP begin id=%lu", (unsigned long)job->id);
    size_t written_samples = 0;
    uint32_t phase = 0;
    int16_t raw[RE8_CAPTURE_READ_BYTES / sizeof(int16_t)];
    while (written_samples < total_bytes / sizeof(int16_t)) {
        uint32_t current_rate = 0;
        uint8_t current_channels = 0;
        rock_audio_capture_format(&current_rate, &current_channels);
        if (current_rate < RE8_CAPTURE_RATE_HZ || current_rate > 96000 ||
            (current_channels != 1 && current_channels != 2) ||
            rock_audio_capture_read(raw, sizeof(raw)) != sizeof(raw)) {
            ESP_LOGE(TAG, "RE8CAP error id=%lu reason=microphone", (unsigned long)job->id);
            goto done;
        }
        if (current_rate != rate || current_channels != channels) {
            rate = current_rate;
            channels = current_channels;
            phase = 0;
        }
        const size_t frames = sizeof(raw) / sizeof(raw[0]) / channels;
        bool left_only = false;
        bool right_only = false;
        if (channels == 2) {
            uint32_t left = 0;
            uint32_t right = 0;
            for (size_t frame = 0; frame < frames; ++frame) {
                left += (unsigned)(raw[2 * frame] < 0 ? -(int)raw[2 * frame] : raw[2 * frame]);
                right += (unsigned)(raw[2 * frame + 1] < 0 ? -(int)raw[2 * frame + 1] : raw[2 * frame + 1]);
            }
            left_only = left > right * 4;
            right_only = right > left * 4;
        }
        int16_t *pcm16 = (int16_t *)recording;
        for (size_t frame = 0; frame < frames && written_samples < total_bytes / sizeof(int16_t); ++frame) {
            int16_t sample = channels == 1 ? raw[frame] :
                left_only ? raw[2 * frame] : right_only ? raw[2 * frame + 1] :
                (int16_t)(((int32_t)raw[2 * frame] + raw[2 * frame + 1]) / 2);
            phase += RE8_CAPTURE_RATE_HZ;
            if (phase >= rate) {
                phase -= rate;
                pcm16[written_samples++] = sample;
            }
        }
    }

    for (size_t offset = 0, seq = 0; offset < total_bytes; offset += RE8_CAPTURE_DATA_BYTES, ++seq) {
        const size_t input_len = total_bytes - offset < RE8_CAPTURE_DATA_BYTES ?
            total_bytes - offset : RE8_CAPTURE_DATA_BYTES;
        char encoded[4 * ((RE8_CAPTURE_DATA_BYTES + 2) / 3) + 1];
        size_t encoded_len = 0;
        if (mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encoded_len,
                                  recording + offset, input_len) != 0) {
            ESP_LOGE(TAG, "RE8CAP error id=%lu reason=base64", (unsigned long)job->id);
            goto done;
        }
        encoded[encoded_len] = '\0';
        ESP_LOGI(TAG, "RE8CAP data id=%lu seq=%u %s", (unsigned long)job->id,
                 (unsigned)seq, encoded);
    }
    ESP_LOGI(TAG, "RE8CAP end id=%lu bytes=%u fnv1a=%08lx", (unsigned long)job->id,
             (unsigned)total_bytes, (unsigned long)re8_capture_fnv1a(recording, total_bytes));

done:
    rock_wake_set_paused(false);
    if (recording) heap_caps_free(recording);
    free(job);
    atomic_store(&s_re8_capture_active, false);
    vTaskDeleteWithCaps(NULL);
}

static void re8_capture_start(const char *label, uint32_t duration_ms)
{
    if (!re8_capture_label_valid(label)) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=label (use 1..32 letters, digits, _ or -)");
        return;
    }
    if (duration_ms < RE8_CAPTURE_MIN_DURATION_MS || duration_ms > RE8_CAPTURE_MAX_DURATION_MS) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=duration (use %u..%u ms)",
                 RE8_CAPTURE_MIN_DURATION_MS, RE8_CAPTURE_MAX_DURATION_MS);
        return;
    }
    int voice_state = rock_voice_state();
    if (voice_state >= 1 && voice_state <= 3) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=voice_active state=%d", voice_state);
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_re8_capture_active, &expected, true)) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=busy");
        return;
    }
    re8_capture_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_DEFAULT);
    if (!job) {
        atomic_store(&s_re8_capture_active, false);
        ESP_LOGE(TAG, "RE8CAP rejected reason=no_memory");
        return;
    }
    job->id = atomic_fetch_add(&s_re8_capture_next_id, 1) + 1;
    job->duration_ms = duration_ms;
    strcpy(job->label, label);
    if (xTaskCreateWithCaps(re8_capture_task, "re8_capture", 8192, job, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(job);
        atomic_store(&s_re8_capture_active, false);
        ESP_LOGE(TAG, "RE8CAP rejected reason=task");
    }
}

// Format: CMD:RE8:BULK:<label>:<seconds>.  The command buffer is mutable, so
// split it in place before scheduling the normal capture task.
static void re8_bulk_capture_start(char *arg)
{
    char *separator = strrchr(arg, ':');
    if (!separator || separator == arg) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=bulk_format (use label:seconds)");
        return;
    }
    *separator = '\0';
    char *end = NULL;
    unsigned long seconds = strtoul(separator + 1, &end, 10);
    if (!separator[1] || !end || *end || seconds > RE8_CAPTURE_MAX_DURATION_MS / 1000u ||
        seconds < RE8_CAPTURE_MIN_DURATION_MS / 1000u) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=bulk_duration (use 5..120 seconds)");
        return;
    }
    re8_capture_start(arg, (uint32_t)seconds * 1000u);
}

// Last wake threshold-crossing dump: no microphone involved, streams the
// pre-roll snapshot kept by rock_wake so false positives can be reviewed as
// WAV.  Framing mirrors RE8CAP (base64 chunks + FNV-1a) under a distinct
// RE8WAKE dump prefix parsed by tools/re8_dump_last_detection.py.
#define RE8_WAKE_SNAPSHOT_SAMPLES (2u * 16000u)

static atomic_bool s_re8_dump_active;

static void re8_wake_last_task(void *arg)
{
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    uint8_t *snapshot = heap_caps_malloc(
        RE8_WAKE_SNAPSHOT_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        ESP_LOGE(TAG, "RE8WAKE dump error id=%lu reason=no_psram", (unsigned long)id);
        goto done;
    }
    uint32_t seq = 0;
    const size_t samples = rock_wake_copy_last_detection(
        (int16_t *)snapshot, RE8_WAKE_SNAPSHOT_SAMPLES, &seq);
    if (!samples) {
        ESP_LOGE(TAG, "RE8WAKE dump error id=%lu reason=no_snapshot", (unsigned long)id);
        goto done;
    }
    const size_t total_bytes = samples * sizeof(int16_t);
    ESP_LOGI(TAG, "RE8WAKE dump begin id=%lu seq=%lu bytes=%u",
             (unsigned long)id, (unsigned long)seq, (unsigned)total_bytes);
    for (size_t offset = 0, chunk = 0; offset < total_bytes;
         offset += RE8_CAPTURE_DATA_BYTES, ++chunk) {
        const size_t input_len = total_bytes - offset < RE8_CAPTURE_DATA_BYTES ?
            total_bytes - offset : RE8_CAPTURE_DATA_BYTES;
        char encoded[4 * ((RE8_CAPTURE_DATA_BYTES + 2) / 3) + 1];
        size_t encoded_len = 0;
        if (mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encoded_len,
                                  snapshot + offset, input_len) != 0) {
            ESP_LOGE(TAG, "RE8WAKE dump error id=%lu reason=base64", (unsigned long)id);
            goto done;
        }
        encoded[encoded_len] = '\0';
        ESP_LOGI(TAG, "RE8WAKE dump data id=%lu seq=%u %s",
                 (unsigned long)id, (unsigned)chunk, encoded);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "RE8WAKE dump end id=%lu seq=%lu bytes=%u fnv1a=%08lx",
             (unsigned long)id, (unsigned long)seq, (unsigned)total_bytes,
             (unsigned long)re8_capture_fnv1a(snapshot, total_bytes));

done:
    heap_caps_free(snapshot);
    atomic_store(&s_re8_dump_active, false);
    vTaskDeleteWithCaps(NULL);
}

static void re8_wake_last_start(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_re8_dump_active, &expected, true)) {
        ESP_LOGE(TAG, "RE8WAKE dump rejected reason=busy");
        return;
    }
    static uint32_t next_dump_id = 0;
    const uint32_t id = ++next_dump_id;
    if (xTaskCreateWithCaps(re8_wake_last_task, "re8_dump", 8192,
                            (void *)(uintptr_t)id, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&s_re8_dump_active, false);
        ESP_LOGE(TAG, "RE8WAKE dump rejected reason=task");
    }
}

bool re8_serial_handle(const char *cmd)
{
    if (!strncmp(cmd, "CMD:RE8:CAPTURE:", 16)) {
        re8_capture_start(cmd + 16, RE8_CAPTURE_DURATION_MS);
    } else if (!strncmp(cmd, "CMD:RE8:BULK:", 13)) {
        // The dispatch buffer in rock_serial.c is mutable; cast away the
        // const of this read-only view for the in-place split above.
        re8_bulk_capture_start((char *)(uintptr_t)cmd + 13);
    } else if (!strcmp(cmd, "CMD:RE8:STATUS")) {
        ESP_LOGI(TAG, "RE8CAP status active=%d next_id=%lu",
                 (int)atomic_load(&s_re8_capture_active),
                 (unsigned long)atomic_load(&s_re8_capture_next_id));
    } else if (!strcmp(cmd, "CMD:RE8:PROBE")) {
        rock_wake_probe();
    } else if (!strcmp(cmd, "CMD:RE8:FEATURES")) {
        rock_wake_feature_probe();
    } else if (!strcmp(cmd, "CMD:RE8:WAKE:ARM")) {
        rock_wake_set_armed(true);
        ESP_LOGI(TAG, "RE8WAKE armed by serial command");
    } else if (!strcmp(cmd, "CMD:RE8:WAKE:DISARM")) {
        rock_wake_set_armed(false);
        ESP_LOGI(TAG, "RE8WAKE disarmed by serial command");
    } else if (!strcmp(cmd, "CMD:RE8:WAKE:MUTE")) {
        rock_gui_set_wake_muted(true);
    } else if (!strcmp(cmd, "CMD:RE8:WAKE:UNMUTE")) {
        rock_gui_set_wake_muted(false);
    } else if (!strcmp(cmd, "CMD:RE8:WAKE:LAST")) {
        re8_wake_last_start();
    } else {
        ESP_LOGE(TAG, "RE8CAP rejected reason=unknown_command");
        return false;
    }
    return true;
}
