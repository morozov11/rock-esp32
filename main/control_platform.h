#pragma once

#include <stddef.h>
#include <stdint.h>

int rock_platform_init(void);
int rock_identity_load(char *id, size_t id_cap, char *secret, size_t secret_cap);
int rock_identity_save(const char *id, const char *secret);
int rock_http_post(const char *path, const char *bearer, const char *body,
                   char *response, size_t response_cap, int *status);
int rock_ws_start(const char *access_token);
void rock_ws_stop(void);
int rock_ws_send(const char *data, size_t len);
int rock_ws_receive(char *data, size_t cap, uint32_t timeout_ms);
int rock_now_rfc3339(char *out, size_t cap);
void rock_random_fill(uint8_t *out, size_t len);
const char *rock_server_base_url(void);
void rock_hosted_recover(void);
int rock_wifi_scan_and_show(void);
