use serde::Deserialize;
use serde_json::{json, Value};

pub const PROTOCOL_VERSION: u8 = 1;
pub const MAX_JSON_FRAME_BYTES: usize = 65_536;
pub const MAX_PAYLOAD_BYTES: usize = 61_440;
pub const DISPLAY_SURFACE_ID: &str = "display.main";

#[derive(Debug, Deserialize)]
pub struct Envelope {
    pub protocol_version: u8,
    pub message_id: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub sent_at: String,
    pub payload: Value,
}

#[derive(Debug, PartialEq)]
pub enum FrameError {
    Oversized,
    Malformed,
    UnsupportedVersion,
    OversizedPayload,
}

pub fn parse_frame(bytes: &[u8]) -> Result<Envelope, FrameError> {
    if bytes.len() > MAX_JSON_FRAME_BYTES {
        return Err(FrameError::Oversized);
    }
    let frame: Envelope = serde_json::from_slice(bytes).map_err(|_| FrameError::Malformed)?;
    if frame.protocol_version != PROTOCOL_VERSION {
        return Err(FrameError::UnsupportedVersion);
    }
    if frame.message_id.len() != 36 || frame.kind.is_empty() || frame.sent_at.is_empty() {
        return Err(FrameError::Malformed);
    }
    if serde_json::to_vec(&frame.payload)
        .map_err(|_| FrameError::Malformed)?
        .len()
        > MAX_PAYLOAD_BYTES
    {
        return Err(FrameError::OversizedPayload);
    }
    Ok(frame)
}

pub fn hello(message_id: &str, sent_at: &str) -> Value {
    envelope(
        message_id,
        "protocol.hello",
        sent_at,
        json!({"supported_protocol_versions": [1]}),
    )
}

pub fn registration(message_id: &str, sent_at: &str, app_version: &str) -> Value {
    use super::display::{DISPLAY_MAX_ITEMS, DISPLAY_MAX_TEXT_LENGTH};
    envelope(
        message_id,
        "device.register",
        sent_at,
        json!({
            "device_type": "esp32", "app_version": safe_firmware_version(app_version),
            "manifest": { "manifest_revision": 1, "roles": ["display_surface"],
                "capabilities": {"revision": 1, "items": [{
                    "name": "display.presentation", "version": 1,
                    "views": ["text", "now_playing", "sensor_grid"],
                    "max_items": DISPLAY_MAX_ITEMS, "max_text_length": DISPLAY_MAX_TEXT_LENGTH
                }]}, "entities": [], "surfaces": [{
                    "surface_id": DISPLAY_SURFACE_ID, "kind": "display",
                    "display_name": "Rock display",
                    "views": ["text", "now_playing", "sensor_grid"]
                }] }
        }),
    )
}

pub fn state_full(
    message_id: &str,
    sent_at: &str,
    state_revision: u64,
    observed_at: &str,
    state: Value,
) -> Value {
    envelope(
        message_id,
        "device.state_full",
        sent_at,
        json!({"snapshot": {
            "state_revision": state_revision,
            "observed_at": observed_at,
            "state": state
        }}),
    )
}

pub fn error_response(
    message_id: &str,
    sent_at: &str,
    reply_to: &str,
    command_id: Option<&str>,
    code: &str,
) -> Value {
    envelope(
        message_id,
        "protocol.error",
        sent_at,
        json!({"error": {"code": code, "message": code, "request_id": reply_to, "details": {}}, "command_id": command_id, "in_reply_to_message_id": reply_to}),
    )
}

pub fn command_accepted(message_id: &str, sent_at: &str, command_id: &str) -> Value {
    envelope(
        message_id,
        "command.accepted",
        sent_at,
        json!({"command_id": command_id, "accepted_at": sent_at}),
    )
}

pub fn command_succeeded(
    message_id: &str,
    sent_at: &str,
    command_id: &str,
    state_revision: u64,
) -> Value {
    envelope(
        message_id,
        "command.result",
        sent_at,
        json!({"command_id": command_id, "status": "succeeded", "completed_at": sent_at, "error": null, "output": {"state_revision": state_revision}}),
    )
}

fn envelope(message_id: &str, kind: &str, sent_at: &str, payload: Value) -> Value {
    json!({"protocol_version": PROTOCOL_VERSION, "message_id": message_id, "type": kind, "sent_at": sent_at, "payload": payload})
}

pub fn safe_firmware_version(version: &str) -> &str {
    if !version.is_empty() && version.len() <= 64 && version.bytes().all(|b| b.is_ascii_graphic()) {
        version
    } else {
        "invalid-version"
    }
}
