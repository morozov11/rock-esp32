#include "rock_wake_probe.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {
constexpr size_t kArenaBytes = 64 * 1024;
constexpr int kProbeFrames = 100;
constexpr int kResourceVariables = 16;
const char* kTag = "rock-wake";

extern const unsigned char kModelStart[] asm("_binary_re8_rockserver_tflite_start");
extern const unsigned char kModelEnd[] asm("_binary_re8_rockserver_tflite_end");

bool register_ops(tflite::MicroMutableOpResolver<13>& resolver) {
    return resolver.AddStridedSlice() == kTfLiteOk &&
           resolver.AddConcatenation() == kTfLiteOk &&
           resolver.AddVarHandle() == kTfLiteOk &&
           resolver.AddReadVariable() == kTfLiteOk &&
           resolver.AddAssignVariable() == kTfLiteOk &&
           resolver.AddDepthwiseConv2D() == kTfLiteOk &&
           resolver.AddConv2D() == kTfLiteOk &&
           resolver.AddSplitV() == kTfLiteOk &&
           resolver.AddCallOnce() == kTfLiteOk &&
           resolver.AddFullyConnected() == kTfLiteOk &&
           resolver.AddLogistic() == kTfLiteOk &&
           resolver.AddQuantize() == kTfLiteOk &&
           resolver.AddReshape() == kTfLiteOk;
}
}  // namespace

extern "C" void rock_wake_probe(void) {
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t model_bytes = kModelEnd - kModelStart;
    ESP_LOGI(kTag, "RE8PROBE begin model_bytes=%u arena_bytes=%u int_free=%u psram_free=%u",
             static_cast<unsigned>(model_bytes), static_cast<unsigned>(kArenaBytes),
             static_cast<unsigned>(internal_before), static_cast<unsigned>(psram_before));

    auto* arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena) {
        ESP_LOGE(kTag, "RE8PROBE error reason=arena_alloc");
        return;
    }

    const auto* model = tflite::GetModel(kModelStart);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(kTag, "RE8PROBE error reason=schema model=%d runtime=%d",
                 model->version(), TFLITE_SCHEMA_VERSION);
        heap_caps_free(arena);
        return;
    }

    tflite::MicroMutableOpResolver<13> resolver;
    if (!register_ops(resolver)) {
        ESP_LOGE(kTag, "RE8PROBE error reason=resolver");
        heap_caps_free(arena);
        return;
    }

    const int64_t start_us = esp_timer_get_time();
    auto* allocator = tflite::MicroAllocator::Create(arena, kArenaBytes);
    if (!allocator) {
        ESP_LOGE(kTag, "RE8PROBE error reason=allocator");
        heap_caps_free(arena);
        return;
    }
    auto* variables = tflite::MicroResourceVariables::Create(allocator, kResourceVariables);
    {
        tflite::MicroInterpreter interpreter(model, resolver, allocator, variables);
        const TfLiteStatus status = interpreter.AllocateTensors();
        if (status != kTfLiteOk) {
            ESP_LOGE(kTag, "RE8PROBE error reason=allocate_tensors elapsed_ms=%lld",
                     static_cast<long long>((esp_timer_get_time() - start_us) / 1000));
        } else {
            TfLiteTensor* input = interpreter.input(0);
            TfLiteTensor* output = interpreter.output(0);
            if (!input || !output || input->type != kTfLiteInt8 ||
                output->type != kTfLiteUInt8 || input->bytes != 120 || output->bytes != 1) {
                ESP_LOGE(kTag, "RE8PROBE error reason=tensor_shape in_type=%d in_bytes=%u out_type=%d out_bytes=%u",
                         input ? input->type : -1, input ? static_cast<unsigned>(input->bytes) : 0,
                         output ? output->type : -1, output ? static_cast<unsigned>(output->bytes) : 0);
            } else {
                memset(input->data.int8, input->params.zero_point, input->bytes);
                const int64_t invoke_us = esp_timer_get_time();
                TfLiteStatus invoke_status = kTfLiteOk;
                int64_t worst_us = 0;
                for (int i = 0; i < kProbeFrames; ++i) {
                    const int64_t frame_start_us = esp_timer_get_time();
                    invoke_status = interpreter.Invoke();
                    const int64_t frame_us = esp_timer_get_time() - frame_start_us;
                    if (frame_us > worst_us) worst_us = frame_us;
                    if (invoke_status != kTfLiteOk) break;
                }
                const int64_t total_us = esp_timer_get_time() - invoke_us;
                ESP_LOGI(kTag, "RE8PROBE result status=%d output=%u arena_used=%u alloc_ms=%lld frames=%d avg_us=%lld worst_us=%lld int_free=%u psram_free=%u",
                         invoke_status, static_cast<unsigned>(output->data.uint8[0]),
                         static_cast<unsigned>(interpreter.arena_used_bytes()),
                         static_cast<long long>((invoke_us - start_us) / 1000),
                         kProbeFrames, static_cast<long long>(total_us / kProbeFrames),
                         static_cast<long long>(worst_us),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            }
        }
    }
    heap_caps_free(arena);
    ESP_LOGI(kTag, "RE8PROBE end int_free=%u psram_free=%u int_delta=%d psram_delta=%d",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<int>(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<int>(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
