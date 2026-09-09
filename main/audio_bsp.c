// RE-4 audio bring-up proof for the JC4880P443C_I_W board: ES8311 codec +
// full-duplex I2S through esp_codec_dev, deterministic playback tone,
// microphone capture metrics and a concurrent playback+capture stress loop
// with resource measurement (plan steps 1.4-1.9). Everything is isolated
// from the device-control protocol on purpose; the player is RE-5.
//
// Pin map and init values follow the working vendor voice demo for this
// exact board (xiaozhi guition-jc4880p443): ES8311 on the shared I2C1 bus,
// I2S0 master with MCLK 256*fs, speaker data GPIO9, mic data GPIO48,
// PA enable GPIO11 (active high). The PA is driven manually from this
// module and stays low unless samples are actually being played.
#include "audio_bsp.h"

#include "board_bus.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "rock-audio";

// ES8311 and I2S wiring (docs/board-hardware.md). ES8311_CODEC_DEFAULT_ADDR
// is the 8-bit form (0x30); the control interface shifts it to 7-bit 0x18.
#define ROCK_CODEC_ADDR ES8311_CODEC_DEFAULT_ADDR
#define ROCK_I2S_PORT I2S_NUM_0
#define ROCK_I2S_MCLK_GPIO GPIO_NUM_13
#define ROCK_I2S_BCLK_GPIO GPIO_NUM_12
#define ROCK_I2S_WS_GPIO GPIO_NUM_10
#define ROCK_I2S_DOUT_GPIO GPIO_NUM_9
#define ROCK_I2S_DIN_GPIO GPIO_NUM_48
#define ROCK_PA_GPIO GPIO_NUM_11

#define ROCK_SAMPLE_RATE 16000
#define ROCK_TONE_HZ 440
#define ROCK_TONE_MS 3000
#define ROCK_TONE_AMP 16000 // ~-6.3 dBFS, comfortable speaker level
#define ROCK_LOOP_AMP 12000
#define ROCK_CAPTURE_S 10
#define ROCK_LOOP_S 60
#define ROCK_CHUNK_SAMPLES 1600 // 100 ms of mono 16 kHz
#define ROCK_DMA_DESC_NUM 6
#define ROCK_DMA_FRAME_NUM 480
#define ROCK_DMA_DEPTH_MS (ROCK_DMA_DESC_NUM * ROCK_DMA_FRAME_NUM / (ROCK_SAMPLE_RATE / 1000))
#define ROCK_FADE_IN_SAMPLES (ROCK_SAMPLE_RATE / 33) // ~30 ms
#define ROCK_FADE_OUT_SAMPLES (ROCK_SAMPLE_RATE / 3) // ~300 ms
#define ROCK_OUT_VOL 70
#define ROCK_IN_GAIN_DB 30.0f
#define ROCK_GT911_PROBE_REG 0x8140
#define ROCK_GT911_ADDR 0x5D // 7-bit, same address the touch driver uses

// Idle-hook CPU accounting: while a core stays in the idle loop, the hook
// fires every few microseconds; gaps above this threshold are busy periods
// (running tasks or ISRs).
#define ROCK_IDLE_GAP_BUSY_US 250

typedef enum {
    WIRE_16_MONO = 0,    // plain int16 mono samples (expected)
    WIRE_16_EMPTY_RIGHT, // int16 pairs, right slot always near zero
    WIRE_32_FRAME_16,    // 16-bit data in the high half of 32-bit frames
} wire_fmt_t;

typedef struct {
    uint64_t samples;
    int64_t sum_sq;
    int32_t peak;
    uint32_t clip;
    // per-second buckets (16k samples each) for the noise-floor estimate
    uint64_t bucket_samples;
    int64_t bucket_sum_sq;
    int32_t bucket_peak;
    double min_bucket_dbfs;
    double max_bucket_dbfs;
    uint32_t buckets;
    bool quiet; // suppress per-window log lines (loopback mode)
} pcm_stats_t;

typedef struct {
    atomic_int rx_overrun;
    atomic_int read_errors;
    atomic_int write_errors;
    atomic_int workers_done;
} loop_counters_t;

static esp_codec_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_es8311_if;
static TaskHandle_t s_bringup_task;
static TaskHandle_t s_writer_task;
static TaskHandle_t s_reader_task;
static UBaseType_t s_writer_stack_min;
static UBaseType_t s_reader_stack_min;
static UBaseType_t s_bringup_stack_min;
static wire_fmt_t s_wire_fmt = WIRE_16_MONO;

static volatile bool s_loop_run;
static loop_counters_t s_loop;
// TX starvation tracking: an inter-write gap >= the DMA buffer depth means
// the channel certainly ran dry (auto_clear sends silence); >= half the
// depth is a warning.
static volatile int s_underrun_definite;
static volatile int s_underrun_warn;
static volatile int64_t s_max_write_gap_us;

static void pa_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ROCK_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(ROCK_PA_GPIO, 0);
}

static inline void pa_set(bool on)
{
    gpio_set_level(ROCK_PA_GPIO, on ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Per-core CPU load via idle hooks (no Kconfig dependency; removed after use).
static volatile int64_t s_idle_busy_us[2];
static volatile int64_t s_idle_last_us[2];

static void idle_account(int cpu)
{
    int64_t now = esp_timer_get_time();
    int64_t last = s_idle_last_us[cpu];
    s_idle_last_us[cpu] = now;
    if (last == 0) return;
    int64_t gap = now - last;
    if (gap > ROCK_IDLE_GAP_BUSY_US) {
        __atomic_fetch_add(&s_idle_busy_us[cpu], gap, __ATOMIC_RELAXED);
    }
}

// The hooks must return false: esp_vApplicationIdleHook() only re-loops when
// a hook refuses idle, otherwise it parks the core in WFI until the next
// interrupt and the hook fires once per tick - which would make every gap
// look like busy time. Refusing idle keeps the hook spinning in a tight
// loop while the core is truly idle, so gaps above the threshold are real
// busy periods. Hooks are deregistered again right after the measurement.
static bool IRAM_ATTR idle_hook_cpu0(void) { idle_account(0); return false; }
static bool IRAM_ATTR idle_hook_cpu1(void) { idle_account(1); return false; }

static void cpu_mon_start(void)
{
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0);
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1);
}

static void cpu_mon_stop(void)
{
    esp_deregister_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0);
    esp_deregister_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1);
}

static void cpu_mon_reset(void)
{
    for (int i = 0; i < 2; i++) {
        __atomic_store_n(&s_idle_busy_us[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_idle_last_us[i], 0, __ATOMIC_RELAXED);
    }
}

// Busy percent of each core over the window since the last cpu_mon_reset.
static void cpu_mon_read(int pct[2], int64_t window_us)
{
    for (int i = 0; i < 2; i++) {
        int64_t busy = __atomic_load_n(&s_idle_busy_us[i], __ATOMIC_RELAXED);
        pct[i] = window_us > 0 ? (int)(busy * 100 / window_us) : 0;
        if (pct[i] < 0) pct[i] = 0;
        if (pct[i] > 100) pct[i] = 100;
    }
}

// ---------------------------------------------------------------------------
// GT911 health probe over the shared I2C1 bus: read the static product-id
// register ("911"), which cannot interfere with coordinate polling.
static int gt911_probe(i2c_master_dev_handle_t *dev)
{
    if (!*dev) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ROCK_GT911_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(rock_i2c1_bus_acquire(), &cfg, dev) != ESP_OK) {
            return 0;
        }
    }
    const uint8_t reg[2] = { ROCK_GT911_PROBE_REG >> 8, ROCK_GT911_PROBE_REG & 0xff };
    uint8_t id[3] = { 0 };
    if (i2c_master_transmit_receive(*dev, reg, sizeof(reg), id, sizeof(id), pdMS_TO_TICKS(100)) != ESP_OK) {
        return 0;
    }
    return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

// ---------------------------------------------------------------------------
// PCM metrics. Sample values themselves never enter any log line.
static double dbfs(double rms)
{
    return 20.0 * log10(rms / 32768.0 + 1e-12);
}

static void pcm_stats_reset(pcm_stats_t *st, bool quiet)
{
    memset(st, 0, sizeof(*st));
    st->quiet = quiet;
    st->min_bucket_dbfs = 0.0;    // replaced by the first bucket
    st->max_bucket_dbfs = -120.0; // anything real is louder
}

static void pcm_stats_bucket_close(pcm_stats_t *st)
{
    if (st->bucket_samples == 0) return;
    double rms = sqrt((double)st->bucket_sum_sq / (double)st->bucket_samples);
    double d = dbfs(rms);
    if (st->buckets == 0 || d < st->min_bucket_dbfs) st->min_bucket_dbfs = d;
    if (d > st->max_bucket_dbfs) st->max_bucket_dbfs = d;
    st->buckets++;
    if (!st->quiet) {
        ESP_LOGI(TAG, "CAPTURE 1s window %u: rms=%.1f dBFS peak=%.1f dBFS",
                 st->buckets, d, dbfs((double)st->bucket_peak));
    }
    st->bucket_samples = 0;
    st->bucket_sum_sq = 0;
    st->bucket_peak = 0;
}

static void pcm_stats_feed(pcm_stats_t *st, const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        int32_t a = s < 0 ? -s : s;
        if (a > st->peak) st->peak = a;
        if (a > st->bucket_peak) st->bucket_peak = a;
        if (a >= 32700) st->clip++;
        st->sum_sq += (int64_t)s * s;
        st->bucket_sum_sq += (int64_t)s * s;
        st->samples++;
        st->bucket_samples++;
        if (st->bucket_samples >= ROCK_SAMPLE_RATE) {
            pcm_stats_bucket_close(st);
        }
    }
}

// Classify the raw byte stream by comparing mean magnitudes of even/odd
// int16 lanes, so the reported levels are computed over the real samples
// even when the wire format differs from the requested 16-bit mono (plan
// step 1.7 explicitly asks to verify it).
static wire_fmt_t detect_wire_fmt(const uint8_t *buf, size_t len)
{
    int64_t even_abs = 0, odd_abs = 0;
    size_t pairs = len / 4;
    const int16_t *s = (const int16_t *)buf;
    for (size_t i = 0; i + 1 < len / 2; i += 2) {
        even_abs += s[i] < 0 ? -s[i] : s[i];
        odd_abs += s[i + 1] < 0 ? -s[i + 1] : s[i + 1];
    }
    if (pairs < 32) return WIRE_16_MONO;
    int64_t even = even_abs / (int64_t)pairs;
    int64_t odd = odd_abs / (int64_t)pairs;
    if (even == 0 && odd == 0) return WIRE_16_MONO; // digital silence
    if (even * 16 < odd) return WIRE_32_FRAME_16;   // low half of 32-bit frames
    if (odd * 16 < even) return WIRE_16_EMPTY_RIGHT; // right slot unused
    return WIRE_16_MONO;
}

static const char *wire_fmt_str(wire_fmt_t f)
{
    switch (f) {
    case WIRE_16_MONO: return "16-bit mono int16 (matches request)";
    case WIRE_16_EMPTY_RIGHT: return "16-bit stereo frames, right slot unused (stride 2)";
    case WIRE_32_FRAME_16: return "32-bit I2S frames, 16-bit data in high half (stride 2)";
    }
    return "unknown";
}

// Number of logical samples extracted from one esp_codec_dev_read buffer.
static size_t extract_samples(const uint8_t *buf, size_t len, wire_fmt_t fmt, int16_t *out)
{
    size_t n = 0;
    const int16_t *s = (const int16_t *)buf;
    switch (fmt) {
    case WIRE_16_MONO:
        memcpy(out, buf, len);
        return len / 2;
    case WIRE_16_EMPTY_RIGHT:
        for (size_t i = 0; i + 1 < len / 2; i += 2) out[n++] = s[i];
        return n;
    case WIRE_32_FRAME_16:
        for (size_t i = 1; i < len / 2; i += 2) out[n++] = s[i];
        return n;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Deterministic sine tone with a short fade-in and a long fade-out (no click).
typedef struct {
    uint32_t freq_hz;
    int32_t amplitude;
    uint32_t total_samples;
    uint32_t fade_in;
    uint32_t fade_out;
    uint32_t pos; // absolute sample index of the next sample to emit
} tone_gen_t;

static void tone_init(tone_gen_t *g, uint32_t freq_hz, uint32_t duration_ms, int32_t amplitude)
{
    memset(g, 0, sizeof(*g));
    g->freq_hz = freq_hz;
    g->amplitude = amplitude;
    g->total_samples = (uint32_t)((uint64_t)duration_ms * ROCK_SAMPLE_RATE / 1000);
    g->fade_in = ROCK_FADE_IN_SAMPLES;
    g->fade_out = ROCK_FADE_OUT_SAMPLES;
    if (g->fade_out > g->total_samples / 2) g->fade_out = g->total_samples / 2;
}

static size_t tone_fill(tone_gen_t *g, int16_t *dst, size_t count)
{
    size_t made = 0;
    while (made < count && g->pos < g->total_samples) {
        double env = 1.0;
        if (g->pos < g->fade_in) {
            env = (double)g->pos / g->fade_in;
        } else if (g->pos >= g->total_samples - g->fade_out) {
            env = (double)(g->total_samples - g->pos) / g->fade_out;
        }
        double t = (double)g->pos / ROCK_SAMPLE_RATE;
        double v = sin(2.0 * M_PI * g->freq_hz * t) * env * g->amplitude;
        int32_t q = (int32_t)(v > 0 ? v + 0.5 : v - 0.5);
        if (q > 32767) q = 32767;
        if (q < -32768) q = -32768;
        dst[made++] = (int16_t)q;
        g->pos++;
    }
    return made;
}

static void reset_underrun_stats(void)
{
    s_underrun_definite = 0;
    s_underrun_warn = 0;
    s_max_write_gap_us = 0;
}

static void note_write_gap(int64_t start_us, int64_t prev_end_us)
{
    if (prev_end_us == 0) return;
    int64_t gap = start_us - prev_end_us;
    if (gap > s_max_write_gap_us) s_max_write_gap_us = gap;
    if (gap > (int64_t)ROCK_DMA_DEPTH_MS * 1000) s_underrun_definite++;
    else if (gap > (int64_t)ROCK_DMA_DEPTH_MS * 500) s_underrun_warn++;
}

// ---------------------------------------------------------------------------
static bool IRAM_ATTR rx_overflow_cb(i2s_chan_handle_t chan, i2s_event_data_t *event, void *user_ctx)
{
    (void)chan; (void)event; (void)user_ctx;
    atomic_fetch_add(&s_loop.rx_overrun, 1);
    return false;
}

static int rock_audio_init(void)
{
    i2c_master_bus_handle_t bus = rock_i2c1_bus_acquire();
    if (!bus) return -1;

    // Full-duplex I2S pair; esp_codec_dev owns enable/disable and reconfigures
    // slots/clock itself when the device is opened, matching the vendor demo.
    const i2s_chan_config_t chan_cfg = {
        .id = ROCK_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = ROCK_DMA_DESC_NUM,
        .dma_frame_num = ROCK_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel pair creation failed");
        return -1;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ROCK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = ROCK_I2S_MCLK_GPIO,
            .bclk = ROCK_I2S_BCLK_GPIO,
            .ws = ROCK_I2S_WS_GPIO,
            .dout = ROCK_I2S_DOUT_GPIO,
            .din = ROCK_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx_chan, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed");
        return -1;
    }
    // Event callbacks must be registered while the channel is still stopped,
    // and the channels are pre-enabled so the disable/enable cycle
    // esp_codec_dev runs during open hits enabled channels quietly (same
    // order as the vendor demo).
    const i2s_event_callbacks_t rx_cbs = { .on_recv_q_ovf = rx_overflow_cb };
    i2s_channel_register_event_callback(s_rx_chan, &rx_cbs, NULL);
    i2s_channel_enable(s_tx_chan);
    i2s_channel_enable(s_rx_chan);

    const audio_codec_i2s_cfg_t i2s_cfg = {
        .port = ROCK_I2S_PORT,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    s_data_if = audio_codec_new_i2s_data((audio_codec_i2s_cfg_t *)&i2s_cfg);
    if (!s_data_if) return -1;

    // ES8311 hangs off the same I2C1 bus as the GT911 (single bus owner).
    const audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ROCK_CODEC_ADDR,
        .bus_handle = bus,
        .clock_speed_hz = 400000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl((audio_codec_i2c_cfg_t *)&i2c_cfg);
    if (!s_ctrl_if) return -1;

    s_gpio_if = audio_codec_new_gpio();
    if (!s_gpio_if) return -1;

    const es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        // PA is driven manually from this module so it is on only while
        // samples are really being played.
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    s_es8311_if = es8311_codec_new((es8311_codec_cfg_t *)&es8311_cfg);
    if (!s_es8311_if) return -1;

    const esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_es8311_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new((esp_codec_dev_cfg_t *)&dev_cfg);
    if (!s_codec) return -1;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = ROCK_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return -1;
    }
    esp_codec_dev_set_out_vol(s_codec, ROCK_OUT_VOL);
    esp_codec_dev_set_in_gain(s_codec, ROCK_IN_GAIN_DB);

    pa_init();
    ESP_LOGI(TAG, "ES8311+I2S up: fs=%d Hz, ch=1, bits=16, vol=%d, in_gain=%.0f dB, MCLK=%d Hz, PA=GPIO%d (off)",
             ROCK_SAMPLE_RATE, ROCK_OUT_VOL, ROCK_IN_GAIN_DB,
             ROCK_SAMPLE_RATE * 256, ROCK_PA_GPIO);
    return 0;
}

static int play_tone(uint32_t freq_hz, uint32_t duration_ms, int32_t amplitude, bool with_pa)
{
    ESP_LOGI(TAG, "TONE start: sine %u Hz, %u ms, 16-bit mono %d Hz, amp=%d (%.1f dBFS), PA=%s",
             (unsigned)freq_hz, (unsigned)duration_ms, ROCK_SAMPLE_RATE, amplitude,
             dbfs((double)amplitude), with_pa ? "on during output" : "off");
    tone_gen_t gen;
    tone_init(&gen, freq_hz, duration_ms, amplitude);
    static int16_t chunk[ROCK_CHUNK_SAMPLES];
    reset_underrun_stats();
    if (with_pa) pa_set(true);
    int failures = 0;
    int64_t prev_end = 0;
    while (gen.pos < gen.total_samples) {
        size_t n = tone_fill(&gen, chunk, ROCK_CHUNK_SAMPLES);
        if (n == 0) break;
        int64_t t0 = esp_timer_get_time();
        if (esp_codec_dev_write(s_codec, chunk, (int)(n * sizeof(int16_t))) != 0) failures++;
        int64_t t1 = esp_timer_get_time();
        note_write_gap(t0, prev_end);
        prev_end = t1;
    }
    // Let the DMA queue drain before cutting the PA so the fade-out survives.
    vTaskDelay(pdMS_TO_TICKS(ROCK_DMA_DEPTH_MS + 100));
    if (with_pa) pa_set(false);
    ESP_LOGI(TAG, "TONE done: %u samples (%u ms), write failures=%d, max inter-write gap=%lld us (DMA depth %d ms)",
             (unsigned)gen.total_samples, (unsigned)duration_ms, failures,
             (long long)s_max_write_gap_us, ROCK_DMA_DEPTH_MS);
    return failures == 0 ? 0 : -1;
}

static int capture_metrics(uint32_t seconds)
{
    ESP_LOGI(TAG, "CAPTURE start: %u s mic PCM, requested 16-bit mono %d Hz, in gain %.0f dB",
             (unsigned)seconds, ROCK_SAMPLE_RATE, ROCK_IN_GAIN_DB);
    static uint8_t raw[ROCK_CHUNK_SAMPLES * 2];
    static int16_t samples[ROCK_CHUNK_SAMPLES];
    pcm_stats_t st;
    pcm_stats_reset(&st, false);
    int read_errors = 0;

    // Wire-format detection first, so the metric phase interprets the stream
    // correctly. The first two chunks are skipped: the RX DMA start and the
    // codec enable transition can leave stale samples in the queue. Majority
    // vote across the next three chunks; the verdict is reused by the
    // loopback reader so both phases interpret the stream identically.
    wire_fmt_t votes[3] = { WIRE_16_MONO, WIRE_16_MONO, WIRE_16_MONO };
    for (int chunk = 0; chunk < 5; chunk++) {
        if (esp_codec_dev_read(s_codec, raw, sizeof(raw)) != 0) {
            if (++read_errors > 5) break;
            chunk--;
            continue;
        }
        if (chunk >= 2) votes[chunk - 2] = detect_wire_fmt(raw, sizeof(raw));
    }
    if (votes[0] == votes[1] || votes[0] == votes[2]) s_wire_fmt = votes[0];
    else if (votes[1] == votes[2]) s_wire_fmt = votes[1];

    const uint32_t target = seconds * ROCK_SAMPLE_RATE;
    while (st.samples < target) {
        if (esp_codec_dev_read(s_codec, raw, sizeof(raw)) != 0) {
            if (++read_errors > 5) break;
            continue;
        }
        size_t n = extract_samples(raw, sizeof(raw), s_wire_fmt, samples);
        pcm_stats_feed(&st, samples, n);
    }
    pcm_stats_bucket_close(&st);
    double rms = st.samples ? sqrt((double)st.sum_sq / st.samples) : 0.0;
    ESP_LOGI(TAG, "CAPTURE summary: samples=%llu, wire format: %s",
             (unsigned long long)st.samples, wire_fmt_str(s_wire_fmt));
    ESP_LOGI(TAG, "CAPTURE levels: peak=%d (%.1f dBFS), rms=%.1f dBFS, clipping(|s|>=32700)=%u, read_errors=%d",
             st.peak, dbfs((double)st.peak), dbfs(rms), st.clip, read_errors);
    ESP_LOGI(TAG, "CAPTURE noise floor (quietest 1 s window)=%.1f dBFS, loudest window=%.1f dBFS",
             st.min_bucket_dbfs, st.max_bucket_dbfs);
    return (st.samples >= target && st.peak > 0) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Concurrent loopback: continuous tone to the speaker + mic capture while
// LVGL, GT911 and the ESP-Hosted Wi-Fi scanner keep running.
static void loopback_writer(void *arg)
{
    (void)arg;
    static int16_t chunk[ROCK_CHUNK_SAMPLES];
    tone_gen_t gen;
    tone_init(&gen, ROCK_TONE_HZ, 3000, ROCK_LOOP_AMP);
    gen.total_samples = UINT32_MAX; // continuous; fade-out set on stop
    int64_t prev_end = 0;
    while (s_loop_run) {
        tone_fill(&gen, chunk, ROCK_CHUNK_SAMPLES);
        int64_t t0 = esp_timer_get_time();
        if (esp_codec_dev_write(s_codec, chunk, sizeof(chunk)) != 0) {
            atomic_fetch_add(&s_loop.write_errors, 1);
        }
        int64_t t1 = esp_timer_get_time();
        note_write_gap(t0, prev_end);
        prev_end = t1;
    }
    // Stop cleanly: emit a short fade-out so the PA cut is click-free.
    gen.total_samples = gen.pos + ROCK_FADE_OUT_SAMPLES;
    size_t n = tone_fill(&gen, chunk, ROCK_CHUNK_SAMPLES);
    if (n > 0) {
        esp_codec_dev_write(s_codec, chunk, (int)(n * sizeof(int16_t)));
    }
    atomic_fetch_add(&s_loop.workers_done, 1);
    vTaskDelete(NULL);
}

static void loopback_reader(void *arg)
{
    static uint8_t raw[ROCK_CHUNK_SAMPLES * 2];
    static int16_t samples[ROCK_CHUNK_SAMPLES];
    pcm_stats_t *st = (pcm_stats_t *)arg;
    while (s_loop_run) {
        if (esp_codec_dev_read(s_codec, raw, sizeof(raw)) != 0) {
            atomic_fetch_add(&s_loop.read_errors, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t n = extract_samples(raw, sizeof(raw), s_wire_fmt, samples);
        pcm_stats_feed(st, samples, n);
    }
    pcm_stats_bucket_close(st);
    atomic_fetch_add(&s_loop.workers_done, 1);
    vTaskDelete(NULL);
}

static int loopback_run(uint32_t seconds)
{
    ESP_LOGI(TAG, "LOOP start: %u s concurrent playback (%u Hz, amp %d) + capture, LVGL/GT911/Wi-Fi scanner active",
             (unsigned)seconds, (unsigned)ROCK_TONE_HZ, ROCK_LOOP_AMP);
    atomic_store(&s_loop.rx_overrun, 0);
    atomic_store(&s_loop.read_errors, 0);
    atomic_store(&s_loop.write_errors, 0);
    atomic_store(&s_loop.workers_done, 0);
    reset_underrun_stats();

    cpu_mon_start();
    // 3 s baseline: LVGL + touch polling + Wi-Fi scanner already running,
    // audio streams not yet started. The RX DMA keeps filling its queue
    // without a reader here, so queue overflows during the baseline are
    // expected and are reset before the loaded window starts.
    cpu_mon_reset();
    int64_t t_base = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(3000));
    int base_pct[2];
    cpu_mon_read(base_pct, esp_timer_get_time() - t_base);
    atomic_store(&s_loop.rx_overrun, 0);

    pcm_stats_t st;
    pcm_stats_reset(&st, true);
    s_loop_run = true;
    pa_set(true);
    if (xTaskCreate(loopback_writer, "rock_au_out", 4096, NULL, 5, &s_writer_task) != pdPASS ||
        xTaskCreate(loopback_reader, "rock_au_in", 4096, &st, 5, &s_reader_task) != pdPASS) {
        ESP_LOGE(TAG, "loopback worker creation failed");
        s_loop_run = false;
        pa_set(false);
        cpu_mon_stop();
        return -1;
    }

    i2c_master_dev_handle_t gt911 = NULL;
    int gt911_ok = 0, gt911_total = 0;
    uint32_t int_free_min = UINT32_MAX;
    cpu_mon_reset();
    int64_t t_loop = esp_timer_get_time();
    for (uint32_t elapsed = 0; elapsed < seconds; elapsed += 10) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        int64_t now = esp_timer_get_time();
        int pct[2];
        cpu_mon_read(pct, now - t_loop);
        uint32_t int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (int_free < int_free_min) int_free_min = int_free;
        gt911_total++;
        gt911_ok += gt911_probe(&gt911) ? 1 : 0;
        if (s_writer_task) s_writer_stack_min = uxTaskGetStackHighWaterMark(s_writer_task);
        if (s_reader_task) s_reader_stack_min = uxTaskGetStackHighWaterMark(s_reader_task);
        if (s_bringup_task) s_bringup_stack_min = uxTaskGetStackHighWaterMark(s_bringup_task);
        ESP_LOGI(TAG, "LOOP %us/%us: cpu0=%d%% cpu1=%d%%, int_ram_free=%uB, psram_free=%uB, "
                 "stack_min(out/in/mon)=%u/%u/%uB, uflow(warn/def)=%d/%d, rx_ovf=%d, "
                 "rw_err=%d/%d, gt911=%d/%d",
                 (unsigned)(elapsed + 10), (unsigned)seconds, pct[0], pct[1],
                 int_free,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 s_writer_stack_min, s_reader_stack_min, s_bringup_stack_min,
                 s_underrun_warn, s_underrun_definite, atomic_load(&s_loop.rx_overrun),
                 atomic_load(&s_loop.write_errors), atomic_load(&s_loop.read_errors),
                 gt911_ok, gt911_total);
    }
    int64_t t_end = esp_timer_get_time();

    s_loop_run = false;
    for (int i = 0; i < 200 && atomic_load(&s_loop.workers_done) < 2; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(ROCK_DMA_DEPTH_MS + 100));
    pa_set(false);
    cpu_mon_stop();

    double rms = st.samples ? sqrt((double)st.sum_sq / st.samples) : 0.0;
    int pct[2];
    cpu_mon_read(pct, t_end - t_loop);
    ESP_LOGI(TAG, "LOOP done: baseline cpu0=%d%% cpu1=%d%%; loaded avg cpu0=%d%% cpu1=%d%%",
             base_pct[0], base_pct[1], pct[0], pct[1]);
    ESP_LOGI(TAG, "LOOP audio: captured=%llu samples, peak=%d (%.1f dBFS), rms=%.1f dBFS, clip=%u; "
             "max inter-write gap=%lld us (DMA depth %d ms)",
             (unsigned long long)st.samples, st.peak, dbfs((double)st.peak), dbfs(rms), st.clip,
             (long long)s_max_write_gap_us, ROCK_DMA_DEPTH_MS);
    ESP_LOGI(TAG, "LOOP i2s: tx_underrun(warn/definite)=%d/%d, rx_overrun=%d; GT911 probe %d/%d ok",
             s_underrun_warn, s_underrun_definite, atomic_load(&s_loop.rx_overrun), gt911_ok, gt911_total);
    ESP_LOGI(TAG, "LOOP ram: int_ram_free_min=%uB, largest_int_block=%uB",
             int_free_min,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return 0;
}

// ---------------------------------------------------------------------------
static void audio_bringup_task(void *arg)
{
    (void)arg;
    // Let the display and Wi-Fi come up first so the concurrency loop sees
    // the full system, exactly like the acceptance scenario requires.
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (rock_audio_init() != 0) {
        ESP_LOGE(TAG, "audio init failed; PA stays off");
        pa_set(false);
        vTaskDelete(NULL);
        return;
    }
    int rc = 0;
    rc |= play_tone(ROCK_TONE_HZ, ROCK_TONE_MS, ROCK_TONE_AMP, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    rc |= capture_metrics(ROCK_CAPTURE_S);
    vTaskDelay(pdMS_TO_TICKS(500));
    rc |= loopback_run(ROCK_LOOP_S);

    esp_codec_dev_close(s_codec);
    pa_set(false);
    ESP_LOGI(TAG, "RE-4 bring-up sequence finished, codec closed, PA off, overall rc=%d", rc);
    vTaskDelete(NULL);
}

int rock_audio_bringup_start(void)
{
    if (s_bringup_task) return 0;
    if (xTaskCreate(audio_bringup_task, "rock_audio", 6144, NULL, 3, &s_bringup_task) != pdPASS) {
        s_bringup_task = NULL;
        return -1;
    }
    return 0;
}
