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
            "manifest": { "manifest_revision": 1, "roles": ["display_surface", "player"],
                "capabilities": {"revision": 1, "items": [
                    { "name": "media.playback", "version": 1, "actions": ["play", "pause", "stop"] },
                    { "name": "media.station", "version": 1, "sources": ["rockserver_catalog"] },
                    { "name": "media.volume", "version": 1, "minimum": 0, "maximum": 100, "step": 1, "mute": true },
                    {
                        "name": "display.presentation", "version": 1,
                        "views": ["text", "now_playing", "sensor_grid"],
                        "max_items": DISPLAY_MAX_ITEMS, "max_text_length": DISPLAY_MAX_TEXT_LENGTH
                    }
                ]}, "entities": [], "surfaces": [{
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

pub fn command_failed(
    message_id: &str,
    sent_at: &str,
    command_id: &str,
    code: &str,
    message: &str,
) -> Value {
    envelope(
        message_id,
        "command.result",
        sent_at,
        json!({
            "command_id": command_id,
            "status": "failed",
            "completed_at": sent_at,
            "error": {
                "code": code,
                "message": message,
                "request_id": command_id,
                "details": {}
            }
        }),
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

pub const MAX_STREAM_URI_LENGTH: usize = 2048;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamUriError {
    TooLong,
    NotAbsoluteHttp,
    Fragment,
    Userinfo,
    MissingHost,
    InvalidPort,
    ForbiddenDestination,
}

pub fn validate_stream_uri(uri: &str) -> Result<(), StreamUriError> {
    if uri.chars().count() > MAX_STREAM_URI_LENGTH {
        return Err(StreamUriError::TooLong);
    }
    let rest = if let Some(r) = uri.strip_prefix("https://") {
        r
    } else if let Some(r) = uri.strip_prefix("http://") {
        r
    } else {
        return Err(StreamUriError::NotAbsoluteHttp);
    };
    if rest.contains('#') {
        return Err(StreamUriError::Fragment);
    }
    let authority_end = rest.find(['/', '?']).unwrap_or(rest.len());
    let authority = &rest[..authority_end];
    if authority.contains('@') {
        return Err(StreamUriError::Userinfo);
    }
    let (host, port, literal_v6) = split_stream_authority(authority)?;
    if host.is_empty() {
        return Err(StreamUriError::MissingHost);
    }
    if let Some(port_str) = port {
        match port_str.parse::<u32>() {
            Ok(value) if (1..=65535).contains(&value) => {}
            _ => return Err(StreamUriError::InvalidPort),
        }
    }
    validate_stream_host(host, literal_v6)
}

fn split_stream_authority(authority: &str) -> Result<(&str, Option<&str>, bool), StreamUriError> {
    if let Some(bracketed) = authority.strip_prefix('[') {
        let (host, tail) = bracketed.split_once(']').ok_or(StreamUriError::MissingHost)?;
        let port = tail.strip_prefix(':');
        if !tail.is_empty() && port.is_none() {
            return Err(StreamUriError::InvalidPort);
        }
        Ok((host, port, true))
    } else {
        match authority.split_once(':') {
            Some((host, port)) => Ok((host, Some(port), false)),
            None => Ok((authority, None, false)),
        }
    }
}

fn validate_stream_host(host: &str, literal_v6: bool) -> Result<(), StreamUriError> {
    use std::net::{Ipv4Addr, Ipv6Addr};

    if literal_v6 {
        let address = host.parse::<Ipv6Addr>().map_err(|_| StreamUriError::ForbiddenDestination)?;
        return validate_stream_ipv6(&address);
    }
    if let Ok(address) = host.parse::<Ipv4Addr>() {
        return validate_stream_ipv4(&address);
    }
    if host.chars().any(|c| c.is_ascii_whitespace() || c.is_ascii_control() || c == '%')
        || host.eq_ignore_ascii_case("localhost")
        || host.chars().all(|c| c.is_ascii_digit())
    {
        return Err(StreamUriError::ForbiddenDestination);
    }
    Ok(())
}

fn validate_stream_ipv4(address: &std::net::Ipv4Addr) -> Result<(), StreamUriError> {
    let [a, b, c, _] = address.octets();
    let forbidden = matches!(a, 0 | 10 | 127)
        || (a == 100 && (64..=127).contains(&b))
        || (a == 169 && b == 254)
        || (a == 172 && (16..=31).contains(&b))
        || (a == 192 && b == 168)
        || (a == 198 && matches!(b, 18 | 19))
        || matches!(
            (a, b, c),
            (192, 0, 0) | (192, 0, 2) | (192, 88, 99) | (198, 51, 100) | (203, 0, 113)
        )
        || (224..=255).contains(&a);
    if forbidden {
        return Err(StreamUriError::ForbiddenDestination);
    }
    Ok(())
}

fn validate_stream_ipv6(address: &std::net::Ipv6Addr) -> Result<(), StreamUriError> {
    use std::net::Ipv4Addr;

    let segments = address.segments();
    if segments[..5].iter().all(|&segment| segment == 0) && matches!(segments[5], 0 | 0xffff) {
        let embedded = (u32::from(segments[6]) << 16) | u32::from(segments[7]);
        return validate_stream_ipv4(&Ipv4Addr::from(embedded));
    }
    if address.is_multicast()
        || (segments[0] & 0xffc0) == 0xfe80
        || (segments[0] & 0xfe00) == 0xfc00
    {
        return Err(StreamUriError::ForbiddenDestination);
    }
    Ok(())
}

#[unsafe(no_mangle)]
pub extern "C" fn rock_validate_stream_uri(uri: *const core::ffi::c_char) -> bool {
    if uri.is_null() {
        return false;
    }
    let s = unsafe { core::ffi::CStr::from_ptr(uri) };
    let Ok(str_slice) = s.to_str() else {
        return false;
    };
    validate_stream_uri(str_slice).is_ok()
}
