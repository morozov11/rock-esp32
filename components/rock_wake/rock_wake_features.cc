#include "rock_wake_probe.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

#include <cstdint>
#include <cstring>

namespace {
constexpr size_t kSamples = 16000;
constexpr size_t kChunk = 160;
constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;
constexpr const char* kTag = "rock-wake";

void fill_config(FrontendConfig* config) {
    memset(config, 0, sizeof(*config));
    config->window.size_ms = 30;
    config->window.step_size_ms = 10;
    config->filterbank.num_channels = 40;
    config->filterbank.lower_band_limit = 125.0f;
    config->filterbank.upper_band_limit = 7500.0f;
    config->noise_reduction.smoothing_bits = 10;
    config->noise_reduction.even_smoothing = 0.025f;
    config->noise_reduction.odd_smoothing = 0.06f;
    config->noise_reduction.min_signal_remaining = 0.05f;
    config->pcan_gain_control.enable_pcan = 1;
    config->pcan_gain_control.strength = 0.95f;
    config->pcan_gain_control.offset = 80.0f;
    config->pcan_gain_control.gain_bits = 21;
    config->log_scale.enable_log = 1;
    config->log_scale.scale_shift = 6;
}

uint32_t hash_feature(uint32_t hash, uint16_t value) {
    hash = (hash ^ (value & 0xffu)) * kFnvPrime;
    return (hash ^ (value >> 8)) * kFnvPrime;
}
}  // namespace

extern "C" void rock_wake_feature_probe(void) {
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    auto* samples = static_cast<int16_t*>(heap_caps_malloc(
        kSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!samples) {
        ESP_LOGE(kTag, "RE8FEATURE error reason=sample_alloc");
        return;
    }
    uint32_t generator = 1;
    for (size_t i = 0; i < kSamples; ++i) {
        generator = generator * 1664525u + 1013904223u;
        samples[i] = static_cast<int16_t>((static_cast<int32_t>(generator >> 16) - 32768) / 8);
    }

    FrontendConfig config;
    fill_config(&config);
    FrontendState state = {};
    if (!FrontendPopulateState(&config, &state, 16000)) {
        ESP_LOGE(kTag, "RE8FEATURE error reason=frontend_init");
        heap_caps_free(samples);
        return;
    }
    const int64_t start_us = esp_timer_get_time();
    uint32_t hash = kFnvOffset;
    size_t frames = 0;
    size_t position = 0;
    bool valid = true;
    while (position + kChunk < kSamples) {
        size_t read = 0;
        const FrontendOutput output = FrontendProcessSamples(&state, samples + position,
                                                               kChunk, &read);
        if (read == 0 || read > kChunk || (output.size != 0 && output.size != 40)) {
            valid = false;
            break;
        }
        position += read;
        if (output.size) {
            for (size_t i = 0; i < output.size; ++i) hash = hash_feature(hash, output.values[i]);
            ++frames;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    ESP_LOGI(kTag, "RE8FEATURE result valid=%d frames=%u samples_read=%u fnv1a=%08lx elapsed_us=%lld int_free=%u psram_free=%u",
             valid, static_cast<unsigned>(frames), static_cast<unsigned>(position),
             static_cast<unsigned long>(hash), static_cast<long long>(elapsed_us),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    FrontendFreeStateContents(&state);
    heap_caps_free(samples);
    ESP_LOGI(kTag, "RE8FEATURE end int_delta=%d psram_delta=%d",
             static_cast<int>(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<int>(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
