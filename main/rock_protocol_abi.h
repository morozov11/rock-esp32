#pragma once

// Cross-language numeric constants shared by the C firmware and the Rust
// session logic (phase 5, single source of truth). Numbers only, no include
// dependencies, so both sides can pull it in cheaply. The Rust side binds
// these at build time (build.rs parses this file and the enum order of
// rock_ui_event_type_t in gui/rock_gui.h — see src/abi.rs).
#ifdef __cplusplus
extern "C" {
#endif

/// Maximum single WSS/JSON frame size accepted in either direction.
#define ROCK_ABI_MAX_FRAME_BYTES 65536

/// Heartbeat cadence of the device WSS session (Rust transport).
#define ROCK_ABI_HEARTBEAT_MS 20000

/// Rust session watchdog: no heartbeat ACK for this long kills the session.
#define ROCK_ABI_HEARTBEAT_ACK_TIMEOUT_MS 75000

#ifdef __cplusplus
}
#endif
