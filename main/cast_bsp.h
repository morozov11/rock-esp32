#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[65];        // mDNS instance or id
    char name[129];     // friendly name (fn TXT record)
    char model[65];     // model name (md TXT record)
    char host[65];      // IPv4 string
    uint16_t port;      // default 8009
} rock_cast_device_t;

/**
 * Perform an mDNS scan for _googlecast._tcp services.
 * Populates out_devices up to max_devices.
 * Returns the number of devices discovered (>=0) or negative on error.
 */
int rock_cast_discover(uint32_t timeout_ms, rock_cast_device_t *out_devices, size_t max_devices);

/**
 * Opaque TLS connection handle for Chromecast communication.
 */
typedef struct rock_cast_tls rock_cast_tls_t;

/**
 * Open a TLS connection to host:port with MBEDTLS_SSL_VERIFY_NONE.
 * Non-blocking / bounded by timeout_ms.
 * Returns handle on success, NULL on failure.
 */
rock_cast_tls_t *rock_cast_tls_open(const char *host, uint16_t port, uint32_t timeout_ms);

/**
 * Send data over the TLS connection.
 * Returns number of bytes sent, or negative on error.
 */
int rock_cast_tls_send(rock_cast_tls_t *tls, const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * Receive data from the TLS connection.
 * Returns number of bytes received (>0), 0 on timeout, or negative on error/disconnect.
 */
int rock_cast_tls_recv(rock_cast_tls_t *tls, uint8_t *data, size_t max_len, uint32_t timeout_ms);

void rock_cast_tls_close(rock_cast_tls_t *tls);

/**
 * Log a message under the rock_cast tag.
 */
void rock_cast_log(const char *msg);

#ifdef __cplusplus
}
#endif
