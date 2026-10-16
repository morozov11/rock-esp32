//! Pairing/session lifecycle, separate from WebSocket command handling.

use std::sync::Mutex;

use super::platform;
use crate::device_control::{
    self, Credentials, DeviceSession, PairingComplete, PairingCreate, PairingRequest, SessionToken,
};

pub(crate) enum SessionError {
    Unauthorized,
    Temporary,
}

#[derive(Clone, Debug)]
struct SessionCache {
    token: String,
    expires_at: String,
}

static SESSION_CACHE: Mutex<Option<SessionCache>> = Mutex::new(None);

pub(crate) fn invalidate_session_cache() {
    let mut cache = SESSION_CACHE.lock().unwrap_or_else(|e| e.into_inner());
    *cache = None;
}

fn update_session_cache(token: &str, expires_at: &str) {
    let mut cache = SESSION_CACHE.lock().unwrap_or_else(|e| e.into_inner());
    *cache = Some(SessionCache {
        token: token.to_string(),
        expires_at: expires_at.to_string(),
    });
}

pub(crate) fn credentials() -> Result<Credentials, ()> {
    match platform::identity_load() {
        Some((id, secret)) => match session(&id, &secret) {
            Ok(creds) => Ok(creds),
            Err(SessionError::Unauthorized) => {
                platform::identity_clear();
                provision()
            }
            Err(SessionError::Temporary) => Err(()),
        },
        None => provision(),
    }
}

#[allow(dead_code)]
pub(crate) fn fresh_access_token() -> Result<String, ()> {
    let (id, secret) = platform::identity_load().ok_or(())?;
    session(&id, &secret)
        .map(|credentials| credentials.access_token)
        .map_err(|_| ())
}

pub(crate) fn cached_access_token(min_remaining: u64) -> Result<String, ()> {
    {
        let cache = SESSION_CACHE.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(entry) = cache.as_ref() {
            if let Ok(now) = platform::now() {
                if !should_fetch(Some(&entry.expires_at), &now, min_remaining) {
                    return Ok(entry.token.clone());
                }
            }
        }
    }

    let (id, secret) = platform::identity_load().ok_or(())?;
    let creds = session(&id, &secret).map_err(|_| ())?;
    Ok(creds.access_token)
}

fn provision() -> Result<Credentials, ()> {
    let create = PairingCreate {
        device_display_name: "Rock ESP32",
        device_type: "esp32",
        app_version: env!("CARGO_PKG_VERSION"),
    };
    let (status, value) = platform::post("/api/v1/pairing-requests", &create)?;
    if status != 201 {
        return Err(());
    }
    let request: PairingRequest = serde_json::from_value(value).map_err(|_| ())?;
    let link = request.deep_link(&platform::server_base_url());
    let (width, modules) = device_control::qr_matrix(&link).map_err(|_| ())?;
    platform::show_pairing(
        &request.short_code,
        &request.verification_phrase,
        &modules,
        width,
    )?;
    let path = format!(
        "/api/v1/pairing-requests/{}/complete",
        request.pairing_request_id
    );
    loop {
        let (status, value) = platform::post(
            &path,
            &PairingComplete {
                desktop_token: &request.desktop_token,
            },
        )?;
        if status == 200 {
            let credentials: Credentials = serde_json::from_value(value).map_err(|_| ())?;
            platform::identity_save(&credentials.device_id, &credentials.device_secret)?;
            update_session_cache(&credentials.access_token, &credentials.access_expires_at);
            return Ok(credentials);
        }
        if status != 202 {
            return Err(());
        }
        platform::delay(2_000);
    }
}

fn session(id: &str, secret: &str) -> Result<Credentials, SessionError> {
    let (status, value) = platform::post(
        "/api/v1/auth/device-session",
        &DeviceSession {
            device_id: id,
            device_secret: secret,
        },
    )
    .map_err(|_| SessionError::Temporary)?;

    if status == 401 || status == 403 || status == 404 {
        invalidate_session_cache();
        return Err(SessionError::Unauthorized);
    }
    if status != 200 {
        return Err(SessionError::Temporary);
    }
    let token: SessionToken = serde_json::from_value(value).map_err(|_| SessionError::Temporary)?;
    update_session_cache(&token.access_token, &token.access_expires_at);
    Ok(Credentials {
        device_id: id.into(),
        device_secret: secret.into(),
        access_token: token.access_token,
        access_expires_at: token.access_expires_at,
    })
}

pub(crate) use crate::device_control::should_fetch;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rfc3339_parsing_basic_and_subseconds() {
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:00Z"), Some(0));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01Z"), Some(1000));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01.5Z"), Some(1500));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01.54Z"), Some(1540));
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T00:00:01.123Z"), Some(1123));
        assert_eq!(
            parse_rfc3339_to_ms("1970-01-01T00:00:01.123456Z"),
            Some(1123)
        );
        assert_eq!(parse_rfc3339_to_ms("1970-01-01T01:00:00+01:00"), Some(0));
        assert_eq!(
            parse_rfc3339_to_ms("1970-01-01T00:00:00-05:00"),
            Some(5 * 3600 * 1000)
        );
    }

    #[test]
    fn fresh_token_does_not_fetch() {
        let now = "2026-10-15T23:00:00Z";
        let expires_at = "2026-10-15T23:15:00Z"; // 15 min = 900_000 ms remaining
        assert!(!should_fetch(Some(expires_at), now, 60_000));

        // With fractional microseconds from server
        let expires_with_us = "2026-10-15T23:15:00.654321Z";
        assert!(!should_fetch(Some(expires_with_us), now, 60_000));
    }

    #[test]
    fn threshold_min_remaining_triggers_fetch() {
        let now = "2026-10-15T23:14:00Z";
        let expires_exact = "2026-10-15T23:15:00Z"; // exactly 60_000 ms remaining
        assert!(should_fetch(Some(expires_exact), now, 60_000));

        let expires_under = "2026-10-15T23:14:59Z"; // 59_000 ms remaining
        assert!(should_fetch(Some(expires_under), now, 60_000));

        let expires_above = "2026-10-15T23:15:01Z"; // 61_000 ms remaining
        assert!(!should_fetch(Some(expires_above), now, 60_000));
    }

    #[test]
    fn expired_token_triggers_fetch() {
        let now = "2026-10-15T23:16:00Z";
        let expires_past = "2026-10-15T23:15:00Z"; // expired 1 min ago
        assert!(should_fetch(Some(expires_past), now, 60_000));

        let expires_equal = "2026-10-15T23:16:00Z"; // exactly now
        assert!(should_fetch(Some(expires_equal), now, 60_000));
    }

    #[test]
    fn missing_cache_triggers_fetch() {
        let now = "2026-10-15T23:00:00Z";
        assert!(should_fetch(None, now, 60_000));
    }

    #[test]
    fn malformed_timestamps_trigger_fetch() {
        let valid = "2026-10-15T23:00:00Z";
        assert!(should_fetch(Some("garbage"), valid, 60_000));
        assert!(should_fetch(Some(valid), "garbage", 60_000));
        assert!(should_fetch(Some("2026-99-99T99:99:99Z"), valid, 60_000));
    }

    #[test]
    fn in_memory_cache_invalidation_and_update() {
        invalidate_session_cache();
        {
            let cache = SESSION_CACHE.lock().unwrap();
            assert!(cache.is_none());
        }

        update_session_cache("token123", "2026-10-15T23:15:00Z");
        {
            let cache = SESSION_CACHE.lock().unwrap();
            let entry = cache.as_ref().unwrap();
            assert_eq!(entry.token, "token123");
            assert_eq!(entry.expires_at, "2026-10-15T23:15:00Z");
        }

        invalidate_session_cache();
        {
            let cache = SESSION_CACHE.lock().unwrap();
            assert!(cache.is_none());
        }
    }
}
