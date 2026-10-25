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
// User-level mute: while set the wake task releases the microphone and never
// runs the detector; persistence is the caller's (NVS) responsibility.
void rock_wake_set_muted(bool muted);
bool rock_wake_is_muted(void);
// Generation of the last threshold-crossing snapshot; 0 means none yet.
uint32_t rock_wake_last_detection_seq(void);
// Copy the last crossing's pre-roll PCM16 16 kHz mono in playback order.
size_t rock_wake_copy_last_detection(int16_t *dst, size_t capacity_samples,
                                     uint32_t *seq);
void rock_wake_get_metrics(bool *running, uint32_t *frames,
                           uint32_t *invocations, uint32_t *detections,
                           uint32_t *stage, bool *armed, uint32_t *peak_milli);

#ifdef __cplusplus
}
#endif
