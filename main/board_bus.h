#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Acquire the one and only I2C1 master bus of the board (SDA GPIO7,
 * SCL GPIO8), shared by the GT911 touch controller and the ES8311 audio
 * codec. The first call creates the bus; every later call returns the same
 * handle without touching the hardware. app_main performs the first call
 * during boot, before the display and audio paths run, so no locking is
 * needed around the lazy creation.
 *
 * The new I2C master driver serializes transactions between devices of one
 * bus with an internal bus mutex, so GT911 polling from the LVGL task and
 * ES8311 register access from the audio task can run concurrently without
 * an additional lock here.
 */
i2c_master_bus_handle_t rock_i2c1_bus_acquire(void);

#ifdef __cplusplus
}
#endif
