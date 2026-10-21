//! Bounded WebSocket session and player/display command dispatch.
//!
//! Submodules:
//! - `revision`: atomic state revision counter tracking.
//! - `catalog`: station metadata caching and HTTP catalog retrieval.
//! - `media`: stream proxy source lifecycle, local/Cast playback, and hardware sync.
//! - `session`: session state machine and tick orchestrator.
//! - `inbound`: server command execution and inbound protocol frames.
//! - `gui_sink`: LVGL GUI event handlers (`GuiEventSink` implementation).

mod catalog;
mod favourites;
mod gui_sink;
mod inbound;
mod media;
mod revision;
mod session;

pub(crate) use media::refresh_player;

use crate::device_control;
use crate::firmware::platform;
use crate::firmware::update_link_indicator;
use core::ffi::c_char;
use serde_json::Value;
use session::Session;

// Single source (phase 5 A3): generated from main/rock_protocol_abi.h.
const HEARTBEAT_MS: u64 = crate::abi::ROCK_ABI_HEARTBEAT_MS;
const HEARTBEAT_ACK_TIMEOUT_MS: u64 = crate::abi::ROCK_ABI_HEARTBEAT_ACK_TIMEOUT_MS;

const FRAME_CAPACITY: usize = device_control::MAX_JSON_FRAME_BYTES + 1;

pub(crate) fn run(token: &str) -> Result<(), ()> {
    platform::ws_start(token)?;
    let result = session(token);
    platform::ws_stop();
    platform::relay_stop();
    update_link_indicator(false);
    platform::gui_mark_catalog_stale();
    result
}

fn send(value: &Value) -> Result<(), ()> {
    let text = serde_json::to_string(value).map_err(|_| ())?;
    if text.len() > device_control::MAX_JSON_FRAME_BYTES {
        return Err(());
    }
    platform::ws_send(&text)
}

fn message_id() -> String {
    let mut b = [0u8; 16];
    platform::random_fill(&mut b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
        b[14], b[15]
    )
}

fn session(token: &str) -> Result<(), ()> {
    let token = token.to_string();
    send(&device_control::hello(&message_id(), &platform::now()?))?;
    let mut frame = vec![0 as c_char; FRAME_CAPACITY];
    let len = platform::ws_receive(&mut frame, 30_000);
    if len <= 0
        || device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
        })
        .map_err(|_| ())?
        .kind
            != "protocol.welcome"
    {
        return Err(());
    }
    send(&device_control::registration(
        &message_id(),
        &platform::now()?,
        env!("CARGO_PKG_VERSION"),
    ))?;

    let mut s = Session::new(token, frame)?;
    loop {
        s.tick_token_refresh();
        s.tick_favourites();
        s.tick_favourite_metadata();
        s.check_heartbeat_watchdog()?;
        s.tick_heartbeat()?;
        s.drain_gui_events();
        if s.fatal {
            return Err(());
        }
        s.tick_player_refresh()?;
        s.tick_cast_pump()?;
        s.tick_pending_deadline();
        s.tick_network_check();

        let len = platform::ws_receive(&mut s.frame, 50);
        if len < 0 {
            return Err(());
        }
        if len == 0 {
            continue;
        }
        s.handle_inbound(len)?;
    }
}
