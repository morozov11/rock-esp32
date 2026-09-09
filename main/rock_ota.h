#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize OTA status tracking and check if the running partition
 * is in ESP_OTA_IMG_PENDING_VERIFY state.
 */
esp_err_t rock_ota_init(void);

/**
 * Mark the current running app as valid and cancel rollback.
 * Call after network and critical subsystems are verified.
 */
esp_err_t rock_ota_mark_valid(void);

/**
 * Mark the current running app as invalid and immediately reboot
 * into the previous known-good OTA slot.
 */
esp_err_t rock_ota_mark_invalid_and_rollback(void);

/**
 * Start OTA update task on Core 0 (pacing chunks with vTaskDelay to protect Core 1 audio).
 * @param url If NULL, uses CONFIG_ROCK_OTA_URL.
 * @param force If true, downloads and flashes even if image version matches current.
 */
esp_err_t rock_ota_check_start(const char *url, bool force);

/**
 * Returns true if an OTA update download is currently active.
 */
bool rock_ota_is_in_progress(void);

#ifdef __cplusplus
}
#endif
