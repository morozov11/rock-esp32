#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One scanned Wi-Fi network, normalized for the scan list UI. Owner of the
// Wi-Fi domain: this module (authmode mirrors esp_wifi authmode codes).
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
} rock_wifi_scan_item_t;

// UI callbacks registered by the composition root; the platform layer never
// calls rock_ui_* directly (dependency inversion, phase 3).
typedef struct {
    bool (*on_splash_status)(const char *text);
    void (*on_wifi_scan)(const rock_wifi_scan_item_t *items, uint16_t count);
    bool (*on_onboarding_status)(const char *text);
    bool (*on_onboarding_show)(const char *ssid, const char *pin, const char *url, const uint8_t *qr_modules, uint16_t qr_width);
} rock_platform_ui_cbs_t;

void rock_platform_set_ui_cbs(const rock_platform_ui_cbs_t *cbs);

/** Bring NVS up (idempotent); needed before any rock_settings_* read. */
int rock_nvs_init(void);

int rock_platform_init(void);
int rock_identity_load(char *id, size_t id_cap, char *secret, size_t secret_cap);
int rock_identity_save(const char *id, const char *secret);
int rock_identity_clear(void);
/** Last user volume in the "rock_settings" NVS namespace: load returns 1 and
 *  fills *out_volume when a valid 0..100 value is stored, 0 otherwise. */
int rock_settings_volume_load(uint8_t *out_volume);
int rock_settings_volume_save(uint8_t volume);
int rock_http_post(const char *path, const char *bearer, const char *body,
                   char *response, size_t response_cap, int *status);
int rock_http_get(const char *path, const char *bearer,
                  char *response, size_t response_cap, int *status);
int rock_ws_start(const char *access_token);
void rock_ws_stop(void);
int rock_ws_send(const char *data, size_t len);
int rock_ws_receive(char *data, size_t cap, uint32_t timeout_ms);
void rock_ws_log_heartbeat_timeout(uint64_t age_ms);
/** Log a received frame's protocol kind (string like "device.command") from
 *  the Rust session loop. Kind only — never frame content. */
void rock_ws_log_rx_frame(const char *kind);
/** Generic session-domain log line from Rust (same pattern as the
 *  rock_ws_log_* helpers above): observability for errors the session
 *  deliberately swallows. Never pass secrets or frame content. */
void rock_esp_log(const char *msg);
int rock_now_rfc3339(char *out, size_t cap);
void rock_random_fill(uint8_t *out, size_t len);
const char *rock_server_base_url(void);
void rock_hosted_recover(void);
int rock_wifi_scan_and_show(void);

bool rock_wifi_is_connected(void);
int rock_wifi_rssi(void);
int rock_wifi_channel(void);
int rock_get_local_ip(char *out, size_t cap);
uint64_t rock_uptime_ms(void);
