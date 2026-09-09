#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RE-4 audio bring-up proof (plan steps 1.4-1.9). Starts one background
 * task that, isolated from any device-control protocol:
 *
 *  1. brings up the ES8311 codec over the shared I2C1 bus and a full-duplex
 *     I2S channel pair through esp_codec_dev (playback GPIO9 + PA GPIO11,
 *     capture GPIO48);
 *  2. plays a deterministic 440 Hz sine tone (3 s, 16 bit mono 16 kHz,
 *     faded in/out against clicks) through the speaker;
 *  3. captures 10 s of microphone PCM and logs format, peak/RMS level,
 *     noise floor and clipping counters;
 *  4. runs 60 s of concurrent playback + capture while LVGL, GT911 touch
 *     and the ESP-Hosted Wi-Fi scanner stay active, logging I2S
 *     underrun/overrun counters, free internal RAM, free PSRAM, task stack
 *     minima and per-core CPU load.
 *
 * All results go to the "rock-audio" log tag; nothing is transmitted or
 * stored. The task parks after the sequence, leaving the PA disabled.
 * Returns 0 when the task was started, -1 otherwise.
 */
int rock_audio_bringup_start(void);

#ifdef __cplusplus
}
#endif
