#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RE-4 audio bring-up proof (plan steps 1.4-1.9).
 */
int rock_audio_bringup_start(void);

/**
 * RE-5 audio subsystem primitives used by the player.
 */
int rock_audio_init(void);
void *rock_audio_codec_handle(void);
void rock_audio_pa_set(bool on);
int rock_audio_reconfig(uint32_t sample_rate, uint8_t channels);

#ifdef __cplusplus
}
#endif

