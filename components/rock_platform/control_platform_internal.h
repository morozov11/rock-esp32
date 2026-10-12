#pragma once

// Private declarations shared between control_platform.c (composition owner),
// rock_wifi_sta.c (STA/reconnect/scan domain) and rock_net_ws.c (HTTP/WSS
// client plumbing) after the phase-4 split. NOT a public header: everything
// the Rust ABI binds stays in control_platform.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control_platform.h"

// --- control_platform.c: UI callback forwarders ---
// The Wi-Fi module reports through these (registered via
// rock_platform_set_ui_cbs) instead of calling rock_ui_* directly.

void rock_platform_notify_splash_status(const char *text);
void rock_platform_notify_wifi_scan(const rock_wifi_scan_item_t *items, uint16_t count);
bool rock_platform_notify_onboarding_status(const char *text);
bool rock_platform_notify_onboarding_show(const char *ssid, const char *pin, const char *url, const uint8_t *qr_modules, uint16_t qr_width);

// --- rock_wifi_sta.c: pieces sequenced by rock_platform_init ---

// Create the STA event group (false on OOM). Must precede handler
// registration and the first connect attempt.
bool rock_wifi_events_create(void);

// Register the WIFI_EVENT/IP_EVENT handler on the default event loop.
void rock_wifi_register_event_handlers(void);

// Connect with exponential backoff until the deadline (uptime ms); keeps
// retrying past C6 resets. Returns true once WIFI_READY is set.
bool rock_wifi_connect_with_backoff(uint64_t deadline_ms);

// Spawn the background reconnect task (returns 0 on success).
int rock_wifi_start_reconnect_task(void);

// --- rock_net_ws.c: pieces sequenced by rock_platform_init ---

// Create the WSS frame queue + event group (false on OOM). Must precede the
// first rock_ws_start.
bool rock_ws_init(void);

// Shared HTTP/1.1 request core behind rock_http_post/rock_http_get.
int rock_http_request(const char *method, const char *path, const char *bearer,
                      const char *body, char *response, size_t response_cap, int *status);
