#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Serial-only diagnostic. Does not open the microphone or start playback.
void rock_wake_probe(void);
void rock_wake_feature_probe(void);

// The ring is valid until the callback returns; next is its write position.
typedef void (*rock_wake_detected_fn)(const int16_t *ring, size_t capacity,
                                      size_t used, size_t next);
int rock_wake_start(rock_wake_detected_fn on_detected);
void rock_wake_set_paused(bool paused);
void rock_wake_set_armed(bool armed);
void rock_wake_get_metrics(bool *running, uint32_t *frames,
                           uint32_t *invocations, uint32_t *detections,
                           uint32_t *stage, bool *armed, uint32_t *peak_milli);

#ifdef __cplusplus
}
#endif
