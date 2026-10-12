#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_BANDS_COUNT 16

/**
 * Initializes the esp-dsp FFT tables and starts the background spectrum analysis task
 * on Core 0. Does NOT allocate heap per frame.
 */
int rock_spectrum_init(void);

/**
 * Thread-safe read of the current 16 normalized band amplitudes (0.0f .. 1.0f).
 * Peak decay and smoothing are applied automatically.
 * When playback is stopped or paused, smoothly decays to zero.
 */
void rock_spectrum_get_bands(float bands[SPECTRUM_BANDS_COUNT]);

/**
 * Returns the average CPU execution time of one FFT analysis cycle in microseconds.
 */
uint32_t rock_spectrum_get_cpu_us(void);
void rock_spectrum_log_metrics(void);

#ifdef __cplusplus
}
#endif
