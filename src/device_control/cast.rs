//! Chromecast state machine, receiver cache, and protocol helpers for rock-esp32.

use super::cast_proto::CastMessage;
use serde_json::{Value, json};

pub const SENDER_ID: &str = "sender-0";
pub const RECEIVER_ID: &str = "receiver-0";
pub const NS_CONNECTION: &str = "urn:x-cast:com.google.cast.tp.connection";
pub const NS_HEARTBEAT: &str = "urn:x-cast:com.google.cast.tp.heartbeat";
pub const NS_RECEIVER: &str = "urn:x-cast:com.google.cast.receiver";
pub const NS_MEDIA: &str = "urn:x-cast:com.google.cast.media";
pub const NS_DEVICEAUTH: &str = "urn:x-cast:com.google.cast.tp.deviceauth";
pub const DEFAULT_MEDIA_RECEIVER: &str = "CC1AD845";
pub const CAST_DEFAULT_PORT: u16 = 8009;

pub const DISCOVERY_TTL_SECONDS: u64 = 60;
pub const MAX_RECEIVERS: usize = 32;

/// Connect / Launch / Load timeout budgets (milliseconds).
pub const CAST_CONNECT_TIMEOUT_MS: u32 = 5_000;
pub const CAST_GET_STATUS_TIMEOUT_MS: u32 = 3_000;
pub const CAST_LAUNCH_TIMEOUT_MS: u32 = 8_000;
/// Per-content-type LOAD attempt (rockcast load_media budget).  The total
/// across candidates stays under the session ACK-watchdog.
pub const CAST_LOAD_ATTEMPT_TIMEOUT_MS: u32 = 15_000;
pub const CAST_HEARTBEAT_INTERVAL_MS: u64 = 5_000;

/// LOAD content types in rockcast candidate order: some receiver firmware
/// keeps a freshly loaded live `audio/wav` stream silently pending, while the
/// same stream plays when loaded as `audio/mpeg`.
pub const CAST_LOAD_CONTENT_TYPES: [&str; 2] = ["audio/wav", "audio/mpeg"];

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscoveredReceiver {
    pub key: String,
    pub name: String,
    pub model: String,
    pub host: String,
    pub port: u16,
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
pub struct ChromecastReceiverInfo {
    pub receiver_id: String,
    pub display_name: String,
    pub discovered_at: u64,
    pub expires_at: u64,
}

#[derive(Clone, Debug)]
pub struct CachedReceiver {
    pub receiver_id: String,
    pub key: String,
    pub device: DiscoveredReceiver,
    pub display_name: String,
    pub discovered_at: u64,
    pub expires_at: u64,
}

#[derive(Default, Debug)]
pub struct ReceiverCache {
    entries: Vec<CachedReceiver>,
    next_counter: u32,
}

impl ReceiverCache {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            next_counter: 1,
        }
    }

    pub fn replace(
        &mut self,
        devices: impl IntoIterator<Item = DiscoveredReceiver>,
        now_sec: u64,
    ) -> Vec<ChromecastReceiverInfo> {
        let expires_at = now_sec + DISCOVERY_TTL_SECONDS;
        let mut incoming: Vec<_> = devices.into_iter().collect();
        incoming.sort_by(|a, b| a.name.cmp(&b.name).then_with(|| a.key.cmp(&b.key)));
        incoming.dedup_by(|a, b| a.key == b.key);
        incoming.truncate(MAX_RECEIVERS);

        let mut next = Vec::with_capacity(incoming.len());
        for device in incoming {
            let receiver_id = self
                .entries
                .iter()
                .find(|cached| cached.key == device.key && cached.expires_at > now_sec)
                .map(|cached| cached.receiver_id.clone())
                .unwrap_or_else(|| {
                    let id = format!("rc-{:04x}", self.next_counter);
                    self.next_counter = self.next_counter.wrapping_add(1).max(1);
                    id
                });
            let display_name = safe_display_name(&device.name);
            next.push(CachedReceiver {
                receiver_id,
                key: device.key.clone(),
                display_name,
                device,
                discovered_at: now_sec,
                expires_at,
            });
        }
        self.entries = next;
        self.active_receivers(now_sec)
    }

    pub fn get_fresh(&self, receiver_id: &str, now_sec: u64) -> Option<DiscoveredReceiver> {
        self.entries
            .iter()
            .find(|cached| cached.receiver_id == receiver_id && cached.expires_at > now_sec)
            .map(|cached| cached.device.clone())
    }

    pub fn get_entry(&self, receiver_id: &str, now_sec: u64) -> Option<CachedReceiver> {
        self.entries
            .iter()
            .find(|cached| cached.receiver_id == receiver_id && cached.expires_at > now_sec)
            .cloned()
    }

    pub fn active_receivers(&self, now_sec: u64) -> Vec<ChromecastReceiverInfo> {
        self.entries
            .iter()
            .filter(|cached| cached.expires_at > now_sec)
            .map(|cached| ChromecastReceiverInfo {
                receiver_id: cached.receiver_id.clone(),
                display_name: cached.display_name.clone(),
                discovered_at: cached.discovered_at,
                expires_at: cached.expires_at,
            })
            .collect()
    }
}

pub fn safe_display_name(raw: &str) -> String {
    let cleaned: String = raw
        .chars()
        .filter(|c| !c.is_control())
        .collect();
    let trimmed = cleaned.trim();
    if trimmed.is_empty() {
        "Chromecast".to_string()
    } else {
        trimmed.chars().take(128).collect()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum OutputMode {
    Local,
    Chromecast {
        receiver_id: String,
        receiver_name: String,
    },
}

impl OutputMode {
    pub fn is_chromecast(&self) -> bool {
        matches!(self, Self::Chromecast { .. })
    }

    pub fn to_snapshot_value(&self) -> Value {
        match self {
            Self::Local => json!({
                "mode": "local"
            }),
            Self::Chromecast { receiver_id, .. } => json!({
                "mode": "chromecast",
                "receiver_id": receiver_id
            }),
        }
    }
}

// ---------------------------------------------------------------------------
// Protocol message constructors
// ---------------------------------------------------------------------------

pub fn msg_connect(destination_id: &str) -> CastMessage {
    CastMessage::string(
        SENDER_ID,
        destination_id,
        NS_CONNECTION,
        r#"{"type":"CONNECT","userAgent":"RockCast-ESP32/0.1"}"#,
    )
}

pub fn msg_launch_dmr(request_id: u32) -> CastMessage {
    let payload = json!({
        "type": "LAUNCH",
        "requestId": request_id,
        "appId": DEFAULT_MEDIA_RECEIVER,
    });
    CastMessage::string(
        SENDER_ID,
        RECEIVER_ID,
        NS_RECEIVER,
        payload.to_string(),
    )
}

pub fn msg_get_receiver_status(request_id: u32) -> CastMessage {
    let payload = json!({
        "type": "GET_STATUS",
        "requestId": request_id,
    });
    CastMessage::string(
        SENDER_ID,
        RECEIVER_ID,
        NS_RECEIVER,
        payload.to_string(),
    )
}

pub fn msg_load_stream(
    request_id: u32,
    transport_id: &str,
    session_id: &str,
    stream_url: &str,
    title: &str,
    content_type: &str,
) -> CastMessage {
    let payload = json!({
        "type": "LOAD",
        "requestId": request_id,
        "sessionId": session_id,
        "media": {
            "contentId": stream_url,
            "contentType": content_type,
            "streamType": "LIVE",
            "metadata": {
                "metadataType": 0,
                "title": title
            }
        },
        "autoplay": true,
        "currentTime": 0,
        "customData": {}
    });
    CastMessage::string(
        SENDER_ID,
        transport_id,
        NS_MEDIA,
        payload.to_string(),
    )
}

pub fn msg_stop_media(request_id: u32, transport_id: &str, media_session_id: i64) -> CastMessage {
    let payload = json!({
        "type": "STOP",
        "requestId": request_id,
        "mediaSessionId": media_session_id
    });
    CastMessage::string(
        SENDER_ID,
        transport_id,
        NS_MEDIA,
        payload.to_string(),
    )
}

/// Best-effort PLAY nudge (rockcast parity): some receiver firmware keeps a
/// freshly loaded live stream in a silent pending state until an explicit
/// media command.
pub fn msg_play(request_id: u32, transport_id: &str, media_session_id: i64) -> CastMessage {
    let payload = json!({
        "type": "PLAY",
        "requestId": request_id,
        "mediaSessionId": media_session_id
    });
    CastMessage::string(
        SENDER_ID,
        transport_id,
        NS_MEDIA,
        payload.to_string(),
    )
}

pub fn msg_stop_receiver(request_id: u32, session_id: &str) -> CastMessage {
    let payload = json!({
        "type": "STOP",
        "requestId": request_id,
        "sessionId": session_id
    });
    CastMessage::string(
        SENDER_ID,
        RECEIVER_ID,
        NS_RECEIVER,
        payload.to_string(),
    )
}

pub fn msg_close_connection(destination_id: &str) -> CastMessage {
    CastMessage::string(
        SENDER_ID,
        destination_id,
        NS_CONNECTION,
        r#"{"type":"CLOSE"}"#,
    )
}

pub fn msg_set_volume(request_id: u32, level_0_to_1: f32) -> CastMessage {
    let clamped = level_0_to_1.clamp(0.0, 1.0);
    let payload = json!({
        "type": "SET_VOLUME",
        "requestId": request_id,
        "volume": { "level": clamped }
    });
    CastMessage::string(
        SENDER_ID,
        RECEIVER_ID,
        NS_RECEIVER,
        payload.to_string(),
    )
}

pub fn msg_ping() -> CastMessage {
    msg_ping_to(RECEIVER_ID)
}

pub fn msg_ping_to(destination_id: &str) -> CastMessage {
    CastMessage::string(
        SENDER_ID,
        destination_id,
        NS_HEARTBEAT,
        r#"{"type":"PING"}"#,
    )
}

pub fn msg_pong() -> CastMessage {
    msg_pong_to(RECEIVER_ID)
}

pub fn msg_pong_to(destination_id: &str) -> CastMessage {
    CastMessage::string(
        SENDER_ID,
        destination_id,
        NS_HEARTBEAT,
        r#"{"type":"PONG"}"#,
    )
}

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AppSession {
    pub transport_id: String,
    pub session_id: String,
}

pub fn parse_dmr(v: &Value) -> Option<AppSession> {
    let apps = v.pointer("/status/applications")?.as_array()?;
    for app in apps {
        let app_id = app.get("appId")?.as_str()?;
        if app_id == DEFAULT_MEDIA_RECEIVER {
            return Some(AppSession {
                transport_id: app.get("transportId")?.as_str()?.to_string(),
                session_id: app.get("sessionId")?.as_str()?.to_string(),
            });
        }
    }
    for app in apps {
        let namespaces = app
            .get("namespaces")
            .and_then(|n| n.as_array())
            .cloned()
            .unwrap_or_default();
        let has_media = namespaces.iter().any(|n| {
            n.get("name")
                .and_then(|x| x.as_str())
                .is_some_and(|s| s == NS_MEDIA)
                || n.as_str() == Some(NS_MEDIA)
        });
        if has_media {
            return Some(AppSession {
                transport_id: app.get("transportId")?.as_str()?.to_string(),
                session_id: app.get("sessionId")?.as_str()?.to_string(),
            });
        }
    }
    None
}

#[derive(Debug, PartialEq, Eq)]
pub enum MediaLoadStatus {
    Pending,
    Ready(Option<i64>),
    Error(String),
}

pub fn classify_media_status(v: &Value, expected_req: u32, stream_url: &str) -> MediaLoadStatus {
    let rid = v.get("requestId").and_then(Value::as_u64).unwrap_or(0) as u32;
    let status = v
        .get("status")
        .and_then(Value::as_array)
        .and_then(|a| a.first());

    let Some(st) = status else {
        return if rid == expected_req {
            MediaLoadStatus::Ready(None)
        } else {
            MediaLoadStatus::Pending
        };
    };

    // If media.contentId is present, ensure it matches our stream_url
    if let Some(content_id) = st.pointer("/media/contentId").and_then(Value::as_str) {
        if content_id != stream_url {
            return MediaLoadStatus::Pending;
        }
    }

    let state = st.get("playerState").and_then(Value::as_str).unwrap_or("");
    let idle_reason = st.get("idleReason").and_then(Value::as_str).unwrap_or("");
    let mid = st.get("mediaSessionId").and_then(Value::as_i64);

    match state {
        "BUFFERING" | "PLAYING" | "PAUSED" => MediaLoadStatus::Ready(mid),
        // This receiver fetches and plays live audio while reporting IDLE with
        // no idleReason. Its matching LOAD response is the only status it sends.
        "IDLE" if rid == expected_req && idle_reason.is_empty() && mid.is_some()
            && st.pointer("/media/contentId").and_then(Value::as_str) == Some(stream_url) => {
            MediaLoadStatus::Ready(mid)
        }
        "IDLE" if matches!(idle_reason, "ERROR" | "CANCELLED" | "INTERRUPTED") => {
            MediaLoadStatus::Error(format!("Cast LOAD failed with idle reason: {idle_reason}"))
        }
        _ => MediaLoadStatus::Pending,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cache_limits_and_sorting() {
        let mut cache = ReceiverCache::new();
        let devices = vec![
            DiscoveredReceiver {
                key: "k2".into(),
                name: "Living Room".into(),
                model: "Audio".into(),
                host: "192.168.1.102".into(),
                port: 8009,
            },
            DiscoveredReceiver {
                key: "k1".into(),
                name: "Bedroom".into(),
                model: "Speaker".into(),
                host: "192.168.1.101".into(),
                port: 8009,
            },
        ];
        let active = cache.replace(devices, 1000);
        assert_eq!(active.len(), 2);
        assert_eq!(active[0].display_name, "Bedroom");
        assert_eq!(active[1].display_name, "Living Room");

        // Fresh lookup
        let d = cache.get_fresh(&active[0].receiver_id, 1010).unwrap();
        assert_eq!(d.host, "192.168.1.101");

        // Expired lookup
        assert!(cache.get_fresh(&active[0].receiver_id, 1000 + DISCOVERY_TTL_SECONDS + 1).is_none());
    }

    #[test]
    fn parse_dmr_extracts_transport_id() {
        let val = json!({
            "status": {
                "applications": [{
                    "appId": "CC1AD845",
                    "displayName": "Default Media Receiver",
                    "sessionId": "sess-123",
                    "transportId": "transport-456"
                }]
            }
        });
        let app = parse_dmr(&val).unwrap();
        assert_eq!(app.transport_id, "transport-456");
        assert_eq!(app.session_id, "sess-123");
    }

    #[test]
    fn classify_media_status_tracks_playback() {
        let val = json!({
            "requestId": 42,
            "status": [{
                "mediaSessionId": 999,
                "playerState": "PLAYING",
                "media": { "contentId": "http://stream.url/audio.mp3" }
            }]
        });
        assert_eq!(
            classify_media_status(&val, 42, "http://stream.url/audio.mp3"),
            MediaLoadStatus::Ready(Some(999))
        );

        // Broadcast status update (requestId 0) without media object
        let val_delta = json!({
            "requestId": 0,
            "status": [{
                "mediaSessionId": 999,
                "playerState": "BUFFERING"
            }]
        });
        assert_eq!(
            classify_media_status(&val_delta, 42, "http://stream.url/audio.mp3"),
            MediaLoadStatus::Ready(Some(999))
        );

        // Mismatched contentId
        let val_mismatch = json!({
            "requestId": 0,
            "status": [{
                "mediaSessionId": 999,
                "playerState": "BUFFERING",
                "media": { "contentId": "http://other.url/stream" }
            }]
        });
        assert_eq!(
            classify_media_status(&val_mismatch, 42, "http://stream.url/audio.mp3"),
            MediaLoadStatus::Pending
        );

        // Transitional IDLE
        let val_idle = json!({
            "requestId": 42,
            "status": [{
                "mediaSessionId": 999,
                "playerState": "IDLE"
            }]
        });
        assert_eq!(
            classify_media_status(&val_idle, 42, "http://stream.url/audio.mp3"),
            MediaLoadStatus::Pending
        );

        let val_idle_fetching = json!({
            "requestId": 42,
            "status": [{
                "mediaSessionId": 999,
                "playerState": "IDLE",
                "media": { "contentId": "http://stream.url/audio.mp3" }
            }]
        });
        assert_eq!(
            classify_media_status(&val_idle_fetching, 42, "http://stream.url/audio.mp3"),
            MediaLoadStatus::Ready(Some(999))
        );
    }
}
