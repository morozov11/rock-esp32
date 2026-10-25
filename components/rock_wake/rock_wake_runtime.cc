#include "rock_wake_probe.h"

#include "audio_bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" int rock_voice_state(void);

namespace {
constexpr size_t kArenaBytes = 64 * 1024;
constexpr size_t kPreRollSamples = 2 * 16000;
constexpr size_t kFrameSamples = 160;
constexpr float kThreshold = 0.617f;
// Energy gate: the board's noise floor measures RMS 84-91 (mic self-noise,
// confirmed by the 2026-10-25 false-positive snapshot), while the quietest
// real corpus speech peaks at RMS 215 (P4 block, quiet voice). 130 separates
// both with margin and stops the detector from firing on the adapting noise
// floor right after every (re)start of the task.
constexpr uint32_t kMinRmsMilli = 130'000;
constexpr size_t kEnergyFrames = 100;  // 1 s of 10 ms frames
// After any detector (re)start the mic path emits a decaying LF transient
// (window RMS 160-270 vs the 90 steady floor, measured 2026-10-25 from
// fill-time ring dumps) while PCAN and the model state adapt; the detector
// scored 0.65-0.73 on that material around frame 110-120.  Crossings stay
// suppressed until the transient and the adaptation window have both passed.
constexpr uint32_t kSettleFrames = 300;
const char* kTag = "rock-wake";
std::atomic<bool> s_started{false};
std::atomic<bool> s_paused{false};
// The deployed device is voice-controlled by default. Serial DISARM remains
// available as an immediate diagnostic and recovery control.
std::atomic<bool> s_armed{true};
// User-level mute: the task must not read the microphone at all while set.
std::atomic<bool> s_muted{false};
std::atomic<uint32_t> s_frames{0};
std::atomic<uint32_t> s_invocations{0};
std::atomic<uint32_t> s_detections{0};
std::atomic<uint32_t> s_stage{0};
std::atomic<uint32_t> s_peak_milli{0};
// Forensic snapshot of the pre-roll ring at the last threshold crossing:
// written by the wake task only, read by the serial dump path. Seqlock:
// 0 = none yet, odd = copy in progress, even = stable.
int16_t* s_snapshot = nullptr;
std::atomic<uint32_t> s_snapshot_seq{0};
std::atomic<size_t> s_snapshot_samples{0};
rock_wake_detected_fn s_on_detected;
extern const unsigned char kModelStart[] asm("_binary_re8_rockserver_tflite_start");

bool register_ops(tflite::MicroMutableOpResolver<13>& r) {
    return r.AddStridedSlice() == kTfLiteOk && r.AddConcatenation() == kTfLiteOk &&
           r.AddVarHandle() == kTfLiteOk && r.AddReadVariable() == kTfLiteOk &&
           r.AddAssignVariable() == kTfLiteOk && r.AddDepthwiseConv2D() == kTfLiteOk &&
           r.AddConv2D() == kTfLiteOk && r.AddSplitV() == kTfLiteOk &&
           r.AddCallOnce() == kTfLiteOk && r.AddFullyConnected() == kTfLiteOk &&
           r.AddLogistic() == kTfLiteOk && r.AddQuantize() == kTfLiteOk &&
           r.AddReshape() == kTfLiteOk;
}

void fill_config(FrontendConfig* c) {
    memset(c, 0, sizeof(*c));
    c->window.size_ms = 30;
    c->window.step_size_ms = 10;
    c->filterbank.num_channels = 40;
    c->filterbank.lower_band_limit = 125.0f;
    c->filterbank.upper_band_limit = 7500.0f;
    c->noise_reduction.smoothing_bits = 10;
    c->noise_reduction.even_smoothing = 0.025f;
    c->noise_reduction.odd_smoothing = 0.06f;
    c->noise_reduction.min_signal_remaining = 0.05f;
    c->pcan_gain_control.enable_pcan = 1;
    c->pcan_gain_control.strength = 0.95f;
    c->pcan_gain_control.offset = 80.0f;
    c->pcan_gain_control.gain_bits = 21;
    c->log_scale.enable_log = 1;
    c->log_scale.scale_shift = 6;
}

void wake_task(void*) {
    uint8_t* arena = nullptr;
    int16_t* ring = nullptr;
    FrontendState frontend = {};
    bool frontend_ready = false;
    if (rock_audio_init() != 0) goto done;
    s_stage.store(1);
    arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ring = static_cast<int16_t*>(heap_caps_malloc(
        kPreRollSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena || !ring) goto done;
    // Optional: keeps the audio of the last crossing for false-positive
    // forensics. Its absence must not stop detection.
    s_snapshot = static_cast<int16_t*>(heap_caps_malloc(
        kPreRollSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_snapshot) ESP_LOGW(kTag, "RE8WAKE snapshot disabled (no PSRAM)");
    s_snapshot_seq.store(0, std::memory_order_relaxed);
    s_snapshot_samples.store(0, std::memory_order_relaxed);
    {
        FrontendConfig config;
        fill_config(&config);
        if (!FrontendPopulateState(&config, &frontend, 16000)) goto done;
        frontend_ready = true;
        const auto* model = tflite::GetModel(kModelStart);
        if (model->version() != TFLITE_SCHEMA_VERSION) goto done;
        tflite::MicroMutableOpResolver<13> resolver;
        if (!register_ops(resolver)) goto done;
        auto* allocator = tflite::MicroAllocator::Create(arena, kArenaBytes);
        if (!allocator) goto done;
        auto* variables = tflite::MicroResourceVariables::Create(allocator, 16);
        tflite::MicroInterpreter interpreter(model, resolver, allocator, variables);
        if (interpreter.AllocateTensors() != kTfLiteOk) goto done;
        TfLiteTensor* input = interpreter.input(0);
        TfLiteTensor* output = interpreter.output(0);
        if (!input || !output || input->type != kTfLiteInt8 ||
            output->type != kTfLiteUInt8 || input->bytes != 120 || output->bytes != 1) goto done;
        s_stage.store(2);

        size_t ring_next = 0, ring_used = 0, feature_index = 0;
        uint32_t frames = 0, invocations = 0, next_allowed = 0;
        uint8_t recent[3] = {};
        size_t recent_count = 0;
        bool was_inactive = false;
        uint32_t previous_rate = 0, phase = 0;
        uint8_t previous_channels = 0;
        int16_t pending[kFrameSamples];
        size_t pending_count = 0;
        uint64_t energy[kEnergyFrames] = {};
        size_t energy_index = 0;
        uint64_t energy_total = 0;
        bool energy_filled = false;
        ESP_LOGI(kTag, "RE8WAKE running arena_used=%u psram_free=%u int_free=%u",
                 static_cast<unsigned>(interpreter.arena_used_bytes()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        while (true) {
            const int voice_state = rock_voice_state();
            const bool muted = s_muted.load();
            const bool inactive = (voice_state >= 1 && voice_state <= 3) ||
                                  s_paused.load() || muted;
            if (inactive) {
                // Mute must release the microphone entirely; stages 3 (RE-7
                // session) and 9 (user mute) tell the two apart in metrics.
                s_stage.store(muted && voice_state < 1 ? 9 : 3);
                was_inactive = true;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (was_inactive) {
                FrontendReset(&frontend);
                if (interpreter.Reset() != kTfLiteOk) break;
                feature_index = recent_count = ring_used = ring_next = 0;
                frames = invocations = next_allowed = 0;
                pending_count = phase = previous_rate = previous_channels = 0;
                energy_index = energy_total = energy_filled = 0;
                memset(energy, 0, sizeof(energy));
                was_inactive = false;
            }
            uint32_t rate = 0;
            uint8_t channels = 0;
            rock_audio_capture_format(&rate, &channels);
            if (rate < 16000 || rate > 96000 || (channels != 1 && channels != 2)) {
                s_stage.store(4);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (rate != previous_rate || channels != previous_channels) {
                FrontendReset(&frontend);
                if (interpreter.Reset() != kTfLiteOk) break;
                feature_index = recent_count = ring_used = ring_next = 0;
                frames = invocations = next_allowed = 0;
                phase = pending_count = 0;
                energy_index = energy_total = energy_filled = 0;
                memset(energy, 0, sizeof(energy));
                previous_rate = rate;
                previous_channels = channels;
            }
            int16_t samples[10 * kFrameSamples];
            s_stage.store(5);
            if (rock_audio_capture_read(samples, sizeof(samples)) != sizeof(samples)) {
                ESP_LOGW(kTag, "RE8WAKE microphone read failed");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_stage.store(6);
            bool stream_ok = true;
            uint32_t left_sum = 0, right_sum = 0;
            if (channels == 2) {
                for (size_t i = 0; i < 800; ++i) {
                    int32_t left = samples[2 * i], right = samples[2 * i + 1];
                    left_sum += left < 0 ? -left : left;
                    right_sum += right < 0 ? -right : right;
                }
            }
            const bool left_only = left_sum > right_sum * 4;
            const bool right_only = right_sum > left_sum * 4;
            for (size_t source = 0; source < 1600 / channels; ++source) {
                const int16_t sample = channels == 1 ? samples[source] :
                    left_only ? samples[2 * source] :
                    right_only ? samples[2 * source + 1] :
                    static_cast<int16_t>((static_cast<int32_t>(samples[2 * source]) +
                                          samples[2 * source + 1]) / 2);
                phase += 16000;
                if (phase < rate) continue;
                phase -= rate;
                pending[pending_count++] = sample;
                if (pending_count != kFrameSamples) continue;
                pending_count = 0;
                uint64_t frame_energy = 0;
                for (size_t i = 0; i < kFrameSamples; ++i) {
                    frame_energy += (int64_t)pending[i] * pending[i];
                }
                energy_total -= energy[energy_index];
                energy_total += frame_energy;
                energy[energy_index] = frame_energy;
                energy_index = (energy_index + 1) % kEnergyFrames;
                if (energy_index == 0 && !energy_filled) {
                    energy_filled = true;
                    const uint64_t mean_square =
                        energy_total / (kEnergyFrames * kFrameSamples);
                    const uint32_t floor_milli = static_cast<uint32_t>(
                        sqrt(static_cast<double>(mean_square)) * 1000.0);
                    ESP_LOGI(kTag, "RE8WAKE floor rms_milli=%lu",
                             static_cast<unsigned long>(floor_milli));
                    // Keep the first second after every (re)start: it is the
                    // evidence for the warm-up artifact investigation.  Only
                    // when no crossing snapshot exists — a floor capture must
                    // never overwrite crossing audio.
                    if (s_snapshot && s_snapshot_seq.load(std::memory_order_relaxed) == 0) {
                        const size_t start =
                            (ring_next + kPreRollSamples - ring_used) % kPreRollSamples;
                        const uint32_t seq =
                            s_snapshot_seq.load(std::memory_order_relaxed);
                        s_snapshot_seq.store(seq + 1, std::memory_order_release);
                        s_snapshot_samples.store(ring_used, std::memory_order_relaxed);
                        for (size_t i = 0; i < ring_used; ++i) {
                            s_snapshot[i] = ring[(start + i) % kPreRollSamples];
                        }
                        s_snapshot_seq.store(seq + 2, std::memory_order_release);
                    }
                }
                for (size_t i = 0; i < kFrameSamples; ++i) {
                    ring[ring_next] = pending[i];
                    ring_next = (ring_next + 1) % kPreRollSamples;
                }
                ring_used = ring_used + kFrameSamples < kPreRollSamples ?
                    ring_used + kFrameSamples : kPreRollSamples;
                size_t consumed = 0;
                const FrontendOutput features = FrontendProcessSamples(
                    &frontend, pending, kFrameSamples, &consumed);
                if (consumed != kFrameSamples || (features.size != 0 && features.size != 40)) {
                    stream_ok = false;
                    break;
                }
                if (!features.size) continue;
                for (size_t i = 0; i < 40; ++i) {
                    const float value = features.values[i] * 0.0390625f;
                    int quantized = static_cast<int>(value / input->params.scale + input->params.zero_point);
                    if (quantized < -128) quantized = -128;
                    if (quantized > 127) quantized = 127;
                    input->data.int8[feature_index * 40 + i] = static_cast<int8_t>(quantized);
                }
                ++frames;
                s_stage.store(7);
                s_frames.fetch_add(1, std::memory_order_relaxed);
                if (++feature_index != 3) continue;
                feature_index = 0;
                if (interpreter.Invoke() != kTfLiteOk) {
                    stream_ok = false;
                    break;
                }
                ++invocations;
                s_invocations.fetch_add(1, std::memory_order_relaxed);
                recent[invocations % 3] = output->data.uint8[0];
                if (recent_count < 3) ++recent_count;
                if (frames < 99 || recent_count < 3 || invocations < next_allowed) continue;
                const float score = (recent[0] + recent[1] + recent[2]) / (3.0f * 255.0f);
                const uint32_t milli = static_cast<uint32_t>(score * 1000.0f);
                uint32_t peak = s_peak_milli.load(std::memory_order_relaxed);
                while (milli > peak && !s_peak_milli.compare_exchange_weak(
                           peak, milli, std::memory_order_relaxed)) {}
                if (score < kThreshold) continue;
                uint32_t rms_milli = 0;
                if (energy_filled) {
                    const uint64_t mean_square =
                        energy_total / (kEnergyFrames * kFrameSamples);
                    rms_milli = static_cast<uint32_t>(
                        sqrt(static_cast<double>(mean_square)) * 1000.0);
                }
                if (frames < kSettleFrames || !energy_filled || rms_milli < kMinRmsMilli) {
                    // Crossing on noise, the post-restart transient or during
                    // frontend adaptation: keep it in the log as FAR evidence,
                    // but no detection, snapshot or RE-7 handoff.
                    ESP_LOGI(kTag, "RE8WAKE gated score=%.3f frame=%lu rms_milli=%lu",
                             score, static_cast<unsigned long>(frames),
                             static_cast<unsigned long>(rms_milli));
                    continue;
                }
                // Count and snapshot every crossing, disarmed soaks included:
                // that is the FAR evidence. ARM only gates the RE-7 handoff.
                next_allowed = invocations + 50;
                s_detections.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGI(kTag, "RE8WAKE crossing score=%.3f frame=%lu armed=%d rms_milli=%lu",
                         score, static_cast<unsigned long>(frames),
                         static_cast<int>(s_armed.load()),
                         static_cast<unsigned long>(rms_milli));
                if (s_snapshot) {
                    const size_t start =
                        (ring_next + kPreRollSamples - ring_used) % kPreRollSamples;
                    const uint32_t seq =
                        s_snapshot_seq.load(std::memory_order_relaxed);
                    s_snapshot_seq.store(seq + 1, std::memory_order_release);
                    s_snapshot_samples.store(ring_used, std::memory_order_relaxed);
                    for (size_t i = 0; i < ring_used; ++i) {
                        s_snapshot[i] = ring[(start + i) % kPreRollSamples];
                    }
                    s_snapshot_seq.store(seq + 2, std::memory_order_release);
                }
                if (s_armed.load() && s_on_detected) {
                    s_on_detected(ring, kPreRollSamples, ring_used, ring_next);
                }
            }
            if (!stream_ok) break;
        }
    }
done:
    s_stage.store(0);
    ESP_LOGE(kTag, "RE8WAKE stopped");
    if (frontend_ready) FrontendFreeStateContents(&frontend);
    s_snapshot_seq.store(0, std::memory_order_relaxed);
    s_snapshot_samples.store(0, std::memory_order_relaxed);
    heap_caps_free(s_snapshot);
    s_snapshot = nullptr;
    heap_caps_free(ring);
    heap_caps_free(arena);
    s_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}
}  // namespace

extern "C" int rock_wake_start(rock_wake_detected_fn on_detected) {
    if (!on_detected) return -1;
    bool expected = false;
    if (!s_started.compare_exchange_strong(expected, true)) return 0;
    s_on_detected = on_detected;
    if (xTaskCreateWithCaps(wake_task, "rock_wake", 16384, nullptr, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_started.store(false);
        return -1;
    }
    return 0;
}

extern "C" void rock_wake_set_paused(bool paused) {
    s_paused.store(paused);
}

extern "C" void rock_wake_set_armed(bool armed) {
    s_armed.store(armed);
}

extern "C" void rock_wake_set_muted(bool muted) {
    s_muted.store(muted);
}

extern "C" bool rock_wake_is_muted(void) {
    return s_muted.load();
}

extern "C" uint32_t rock_wake_last_detection_seq(void) {
    return s_snapshot_seq.load(std::memory_order_acquire);
}

// Copies the last crossing's pre-roll PCM in playback order. Returns the
// number of int16 samples copied (0 when no snapshot exists yet or when the
// writer kept racing); *seq then holds the snapshot generation.
extern "C" size_t rock_wake_copy_last_detection(int16_t *dst, size_t capacity_samples,
                                                uint32_t *seq) {
    if (!dst) return 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = s_snapshot_seq.load(std::memory_order_acquire);
        if (before == 0) return 0;
        if (before & 1) continue;  // writer mid-copy
        const size_t samples = s_snapshot_samples.load(std::memory_order_relaxed);
        const size_t count = samples < capacity_samples ? samples : capacity_samples;
        memcpy(dst, s_snapshot, count * sizeof(int16_t));
        const uint32_t after = s_snapshot_seq.load(std::memory_order_acquire);
        if (before == after) {
            if (seq) *seq = before;
            return count;
        }
    }
    return 0;
}

extern "C" void rock_wake_get_metrics(bool *running, uint32_t *frames,
                                       uint32_t *invocations, uint32_t *detections,
                                       uint32_t *stage, bool *armed,
                                       uint32_t *peak_milli) {
    if (running) *running = s_started.load();
    if (frames) *frames = s_frames.load(std::memory_order_relaxed);
    if (invocations) *invocations = s_invocations.load(std::memory_order_relaxed);
    if (detections) *detections = s_detections.load(std::memory_order_relaxed);
    if (stage) *stage = s_stage.load();
    if (armed) *armed = s_armed.load();
    if (peak_milli) *peak_milli = s_peak_milli.load(std::memory_order_relaxed);
}
