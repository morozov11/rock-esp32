#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load Wi-Fi credentials from NVS ("rock_wifi" namespace).
 * Returns true if a non-empty SSID was found and loaded.
 * Password buffer is optional (can pass NULL if not needed).
 */
bool rock_wifi_storage_load(char *ssid, size_t ssid_cap, char *password, size_t password_cap);

/**
 * Save Wi-Fi credentials into NVS ("rock_wifi" namespace).
 * Never logs credentials.
 * Returns true on success.
 */
bool rock_wifi_storage_save(const char *ssid, const char *password);

/**
 * Clear Wi-Fi credentials from NVS ("rock_wifi" namespace only).
 * Does not touch "rock_auth" or other namespaces.
 */
bool rock_wifi_storage_clear(void);

/** Request onboarding on the next boot without changing saved credentials. */
bool rock_wifi_storage_request_setup(void);

/** Consume a one-shot onboarding request from the BOOT button. */
bool rock_wifi_storage_take_setup_request(void);

#ifdef __cplusplus
}
#endif
