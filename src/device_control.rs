use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

pub const PROTOCOL_VERSION: u8 = 1;
pub const MAX_JSON_FRAME_BYTES: usize = 65_536;
pub const MAX_PAYLOAD_BYTES: usize = 61_440;

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

pub trait CommandHandler {
    fn capability(&self) -> &'static str;
    fn handle(&mut self, body: &Value) -> Result<Value, &'static str>;
}

pub struct Dispatcher<'a> {
    handlers: &'a mut [&'a mut dyn CommandHandler],
}

impl<'a> Dispatcher<'a> {
    pub fn new(handlers: &'a mut [&'a mut dyn CommandHandler]) -> Self {
        Self { handlers }
    }
    pub fn dispatch(&mut self, frame: &Envelope) -> Result<Value, &'static str> {
        if frame.kind != "device.command" {
            return Err("unsupported_message_type");
        }
        let body = frame.payload.get("body").ok_or("invalid_command")?;
        let name = body
            .get("name")
            .and_then(Value::as_str)
            .ok_or("invalid_command")?;
        self.handlers
            .iter_mut()
            .find(|h| name.starts_with(h.capability()))
            .ok_or("capability_not_supported")?
            .handle(body)
    }
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
    envelope(
        message_id,
        "device.register",
        sent_at,
        json!({
            "device_type": "esp32", "app_version": safe_firmware_version(app_version),
            "manifest": { "manifest_revision": 1, "roles": [],
                "capabilities": {"revision": 1, "items": []}, "entities": [], "surfaces": [] }
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

pub fn error_response(message_id: &str, sent_at: &str, reply_to: &str, code: &str) -> Value {
    envelope(
        message_id,
        "protocol.error",
        sent_at,
        json!({"reply_to": reply_to, "code": code, "message": code}),
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

#[derive(Serialize)]
pub struct PairingCreate<'a> {
    pub device_display_name: &'a str,
    pub device_type: &'static str,
    pub app_version: &'a str,
}

#[derive(Deserialize)]
pub struct PairingRequest {
    pub pairing_request_id: String,
    pub desktop_token: String,
    pub approval_secret: String,
    pub short_code: String,
    pub verification_phrase: String,
}

impl PairingRequest {
    pub fn deep_link(&self, base_url: &str) -> String {
        format!(
            "{}/?code={}#secret={}",
            base_url.trim_end_matches('/'),
            self.short_code,
            self.approval_secret
        )
    }
}

#[derive(Serialize)]
pub struct PairingComplete<'a> {
    pub desktop_token: &'a str,
}

#[derive(Deserialize)]
pub struct Credentials {
    pub device_id: String,
    pub device_secret: String,
    pub access_token: String,
    pub access_expires_at: String,
}

#[derive(Deserialize)]
pub struct SessionToken {
    pub access_token: String,
    pub access_expires_at: String,
}

#[derive(Serialize)]
pub struct DeviceSession<'a> {
    pub device_id: &'a str,
    pub device_secret: &'a str,
}

pub fn qr_matrix(link: &str) -> Result<(u16, Vec<u8>), qrcode::types::QrError> {
    let code = qrcode::QrCode::new(link.as_bytes())?;
    let width = code.width() as u16;
    let modules = code
        .into_colors()
        .into_iter()
        .map(|c| u8::from(c == qrcode::Color::Dark))
        .collect();
    Ok((width, modules))
}
