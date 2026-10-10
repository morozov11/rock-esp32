#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C-visible exports implemented in Rust (#[no_mangle], linked from the Rust
// static library). Signatures are verbatim copies of the definitions in
// src/device_control/protocol.rs and src/device_control/pairing.rs — keep in
// lockstep until a shared ABI header exists (phase 5).

/** Pure-Rust SSRF validator for stream URIs (protocol.rs). */
bool rock_validate_stream_uri(const char *uri);

/** Pure-Rust QR matrix encoder (pairing.rs): 0 on success, <0 on error,
 *  -2 when out_cap is too small. out_width receives the matrix width. */
int rock_qr_encode(const char *text, uint8_t *out_buf, size_t out_cap, uint16_t *out_width);

#ifdef __cplusplus
}
#endif
