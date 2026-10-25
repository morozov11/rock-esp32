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
#include <cstdint>
#include <cstring>

extern "C" int rock_voice_state(void);

namespace {
constexpr size_t kArenaBytes = 64 * 1024;
constexpr size_t kPreRollSamples = 2 * 16000;
constexpr size_t kFrameSamples = 160;
constexpr float kThreshold = 0.617f;
const char* kTag = "rock-wake";
std::atomic<bool> s_started{false};
std::atomic<bool> s_paused{false};
// The deployed device is voice-controlled by default. Serial DISARM remains
// available as an immediate diagnostic and recovery control.
std::atomic<bool> s_armed{true};
std::atomic<uint32_t> s_frames{0};
std::atomic<uint32_t> s_invocations{0};
std::atomic<uint32_t> s_detections{0};
std::atomic<uint32_t> s_stage{0};
std::atomic<uint32_t> s_peak_milli{0};
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
        bool was_voice_active = false;
        uint32_t previous_rate = 0, phase = 0;
        uint8_t previous_channels = 0;
        int16_t pending[kFrameSamples];
        size_t pending_count = 0;
        ESP_LOGI(kTag, "RE8WAKE running arena_used=%u psram_free=%u int_free=%u",
                 static_cast<unsigned>(interpreter.arena_used_bytes()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        while (true) {
            const int voice_state = rock_voice_state();
            const bool voice_active = (voice_state >= 1 && voice_state <= 3) || s_paused.load();
            if (voice_active) {
                s_stage.store(3);
                was_voice_active = true;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (was_voice_active) {
                FrontendReset(&frontend);
                if (interpreter.Reset() != kTfLiteOk) break;
                feature_index = recent_count = ring_used = ring_next = 0;
                frames = invocations = next_allowed = 0;
                pending_count = phase = previous_rate = previous_channels = 0;
                was_voice_active = false;
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
                if (score >= kThreshold && s_armed.load() && s_on_detected) {
                    ESP_LOGI(kTag, "RE8WAKE detected score=%.3f frame=%lu", score,
                             static_cast<unsigned long>(frames));
                    next_allowed = invocations + 50;
                    s_detections.fetch_add(1, std::memory_order_relaxed);
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
