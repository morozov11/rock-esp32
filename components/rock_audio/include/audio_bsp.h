#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_ROCK_BRINGUP_TESTS
/**
 * RE-4 audio bring-up proof (plan steps 1.4-1.9).
 */
int rock_audio_bringup_start(void);
#endif

/**
 * RE-5 audio subsystem primitives used by the player.
 */
int rock_audio_init(void);
void *rock_audio_codec_handle(void);
void rock_audio_pa_set(bool on);
int rock_audio_reconfig(uint32_t sample_rate, uint8_t channels);
// Read one bounded PCM16 mono microphone chunk; returns bytes or -1.
int rock_audio_capture_read(void *buffer, size_t bytes);
void rock_audio_capture_format(uint32_t *sample_rate, uint8_t *channels);

#ifdef __cplusplus
}
#endif

