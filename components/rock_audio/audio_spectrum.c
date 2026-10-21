#include "audio_spectrum.h"
#include "rock_player.h"
#include "esp_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

#define FFT_N 256
#define INTERVAL_MS 40
static const char *TAG = "rock-spectrum";
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static float s_bands[SPECTRUM_BANDS_COUNT];
static float s_window[FFT_N] __attribute__((aligned(16)));
static float s_fft[FFT_N * 2] __attribute__((aligned(16)));
static int16_t s_pcm[FFT_N * 2];
static uint16_t s_edges[SPECTRUM_BANDS_COUNT + 1];
static uint64_t s_elapsed_us;
static uint32_t s_frames, s_active_frames, s_held_frames, s_max_us;
static uint16_t s_pcm_peak_max;
static int64_t s_started_us;

/* A real mono signal in a complex FFT. No two-real-channel unpacking:
 * stereo is downmixed before the Hann window, independent of chip revision. */
static void analyze(size_t count, uint8_t channels, float out[SPECTRUM_BANDS_COUNT])
{
    for (size_t i = 0; i < FFT_N; i++) {
        size_t j = i * channels;
        float sample = 0;
        if (j + channels <= count) {
            sample = s_pcm[j];
            if (channels == 2) sample = (sample + s_pcm[j + 1]) * 0.5f;
        }
        s_fft[2 * i] = (sample / 32768.0f) * s_window[i];
        s_fft[2 * i + 1] = 0;
    }
    dsps_fft2r_fc32_ansi(s_fft, FFT_N);
    dsps_bit_rev_fc32(s_fft, FFT_N);
    for (int b = 0; b < SPECTRUM_BANDS_COUNT; b++) {
        float peak = 0;
        for (int k = s_edges[b]; k < s_edges[b + 1]; k++) {
            float re = s_fft[2*k], im = s_fft[2*k+1];
            float magnitude = sqrtf(re*re + im*im) * (4.0f / FFT_N);
            if (magnitude > peak) peak = magnitude;
        }
        /* -60..0 dBFS display range; normalization includes Hann coherent gain. */
        float level = (20.0f * log10f(fmaxf(peak, 0.001f)) + 60.0f) / 60.0f;
        out[b] = fminf(1.0f, fmaxf(0, level));
    }
}

static bool self_check(void)
{
    /* Exact FFT bin, stereo and silence exercise the actual public DSP path. */
    float bands[SPECTRUM_BANDS_COUNT];
    for (int i = 0; i < FFT_N; i++) {
        s_pcm[2*i] = s_pcm[2*i+1] = (int16_t)(16000 * sinf(2 * M_PI * 16 * i / FFT_N));
    }
    analyze(FFT_N * 2, 2, bands);
    int peak = 0;
    for (int b = 1; b < SPECTRUM_BANDS_COUNT; b++) if (bands[b] > bands[peak]) peak = b;
    bool ok = (peak == 9) && s_edges[peak] <= 16 && s_edges[peak+1] > 16 && bands[peak] > 0.8f && bands[peak] < 0.99f;
    memset(s_pcm, 0, sizeof(s_pcm));
    analyze(FFT_N, 1, bands);
    for (int b = 0; b < SPECTRUM_BANDS_COUNT; b++) ok = ok && bands[b] == 0;
    ESP_LOGI(TAG, "FFT self-check: %s (stereo bin 16 in band %d [%u..%u], peak=%.2f, mono silence)",
             ok ? "PASS" : "FAIL", peak, s_edges[peak], s_edges[peak+1], bands[peak]);
    return ok;
}

static void analysis_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Spectrum task Core %d, FFT %d, 25 Hz", xPortGetCoreID(), FFT_N);
    TickType_t wake = xTaskGetTickCount();
    int64_t last_pcm_us = 0;
    while (1) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INTERVAL_MS));
        int64_t start = esp_timer_get_time();
        uint32_t rate = 0;
        uint8_t channels = 0;
        size_t n = rock_player_copy_pcm_snapshot(s_pcm, FFT_N * 2, &rate, &channels);
        bool active = rate > 0 && (channels == 1 || channels == 2) && n >= FFT_N * channels;
        bool hold = !active && rock_player_is_cast_active() && start - last_pcm_us < 300000;
        float next[SPECTRUM_BANDS_COUNT] = {0};
        uint16_t pcm_peak = 0;
        if (active) {
            analyze(n, channels, next);
            last_pcm_us = start;
            for (size_t i = 0; i < n; i++) {
                int32_t sample = s_pcm[i];
                if (sample < 0) sample = -sample;
                if (sample > pcm_peak) pcm_peak = (uint16_t)sample;
            }
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int b = 0; b < SPECTRUM_BANDS_COUNT; b++) {
            if (!hold) s_bands[b] = fmaxf(next[b], s_bands[b] * 0.74f);
            if (s_bands[b] < 0.01f) s_bands[b] = 0;
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - start);
        s_elapsed_us += us;
        s_frames++;
        s_active_frames += active;
        s_held_frames += hold;
        if (pcm_peak > s_pcm_peak_max) s_pcm_peak_max = pcm_peak;
        if (us > s_max_us) s_max_us = us;
        xSemaphoreGive(s_mutex);
    }
}

int rock_spectrum_init(void)
{
    if (s_task) return 0;
    if (dsps_fft2r_init_fc32(NULL, FFT_N) != ESP_OK) return -1;
    dsps_wind_hann_f32(s_window, FFT_N);
    s_edges[0] = 1;
    for (int b = 1; b <= SPECTRUM_BANDS_COUNT; b++) {
        int edge = (int)roundf(powf((float)SPECTRUM_MAX_BIN, b / (float)SPECTRUM_BANDS_COUNT));
        s_edges[b] = edge > s_edges[b-1] ? edge : s_edges[b-1] + 1;
    }
    if (!self_check()) return -1;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return -1;
    s_started_us = esp_timer_get_time();
    return xTaskCreatePinnedToCore(analysis_task, "rock_spectrum", 4096, NULL, 2, &s_task, 0) == pdPASS ? 0 : -1;
}

void rock_spectrum_get_bands(float bands[SPECTRUM_BANDS_COUNT])
{
    if (!bands) return;
    if (s_mutex && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        memcpy(bands, s_bands, sizeof(s_bands));
        xSemaphoreGive(s_mutex);
    }
    /* Caller retains last frame if analysis is publishing. LVGL never waits. */
}

uint32_t rock_spectrum_get_cpu_us(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t average = s_frames ? (uint32_t)(s_elapsed_us / s_frames) : 0;
    xSemaphoreGive(s_mutex);
    return average;
}

void rock_spectrum_log_metrics(void)
{
    if (!s_task) return;
    TaskStatus_t status;
    vTaskGetInfo(s_task, &status, pdTRUE, eInvalid);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint64_t wall = esp_timer_get_time() - s_started_us;
    float peak = 0;
    for (int b = 0; b < SPECTRUM_BANDS_COUNT; b++) peak = fmaxf(peak, s_bands[b]);
    ESP_LOGI(TAG, "metrics frames=%lu active=%lu held=%lu pcm_peak=%u avg_us=%lu max_us=%lu runtime_us=%lu wall_us=%llu cpu_pct=%.3f stack_free=%u peak=%.3f",
             (unsigned long)s_frames, (unsigned long)s_active_frames,
             (unsigned long)s_held_frames, (unsigned)s_pcm_peak_max,
             (unsigned long)(s_frames ? s_elapsed_us / s_frames : 0), (unsigned long)s_max_us,
             (unsigned long)status.ulRunTimeCounter, (unsigned long long)wall,
             wall ? 100.0 * status.ulRunTimeCounter / wall : 0, (unsigned)status.usStackHighWaterMark, peak);
    xSemaphoreGive(s_mutex);
}
