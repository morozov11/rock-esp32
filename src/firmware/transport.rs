//! Bounded WebSocket session and display-command dispatch.

use super::platform;
use crate::device_control::{self, DisplayState};
use core::ffi::c_char;
use serde_json::Value;

const FRAME_CAPACITY: usize = device_control::MAX_JSON_FRAME_BYTES + 1;

pub(crate) fn run(token: &str) -> Result<(), ()> {
    platform::ws_start(token)?;
    let result = session();
    platform::ws_stop();
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
        b[0],
        b[1],
        b[2],
        b[3],
        b[4],
        b[5],
        b[6],
        b[7],
        b[8],
        b[9],
        b[10],
        b[11],
        b[12],
        b[13],
        b[14],
        b[15]
    )
}
fn session() -> Result<(), ()> {
    send(&device_control::hello(&message_id(), &platform::now()?))?;
    let mut frame = vec![0 as c_char; FRAME_CAPACITY];
    let len = platform::ws_receive(&mut frame, 10_000);
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
    let mut display = DisplayState::new();
    publish_state(&display)?;
    loop {
        let len = platform::ws_receive(&mut frame, 20_000);
        if len < 0 {
            return Err(());
        }
        if len == 0 {
            continue;
        }
        let Ok(inbound) = device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
        }) else {
            continue;
        };
        if inbound.kind != "device.command" {
            continue;
        }
        let command = match device_control::display_command(&inbound) {
            Ok(command) => command,
            Err(code) => {
                send(&device_control::error_response(
                    &message_id(),
                    &platform::now()?,
                    &inbound.message_id,
                    inbound.payload.get("command_id").and_then(Value::as_str),
                    code,
                ))?;
                continue;
            }
        };
        send(&device_control::command_accepted(
            &message_id(),
            &platform::now()?,
            &command.command_id,
        ))?;
        let changed = display.apply(command.view.clone());
        if changed && !crate::presentation::render(&command.view) {
            return Err(());
        }
        if changed {
            publish_state(&display)?;
        }
        send(&device_control::command_succeeded(
            &message_id(),
            &platform::now()?,
            &command.command_id,
            display.revision(),
        ))?;
    }
}
fn publish_state(display: &DisplayState) -> Result<(), ()> {
    send(&device_control::state_full(
        &message_id(),
        &platform::now()?,
        display.revision(),
        &platform::now()?,
        device_control::display_state(display),
    ))
}
