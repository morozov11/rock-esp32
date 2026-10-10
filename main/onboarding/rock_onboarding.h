#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the interactive Wi-Fi onboarding flow:
 * 1. Brings up SoftAP (CONFIG_ROCK_AP_SSID)
 * 2. Starts HTTP server on port 80 serving the mobile setup portal
 * 3. Starts lightweight captive DNS redirector
 * 4. Displays onboarding instructions and QR code on the ST7701 display
 *
 * Runs until valid credentials are saved, then schedules device reboot into STA mode.
 */
esp_err_t rock_onboarding_run(void);

#ifdef __cplusplus
}
#endif
