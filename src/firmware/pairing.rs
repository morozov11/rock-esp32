//! Pairing/session lifecycle, separate from WebSocket command handling.

use super::platform;
use crate::device_control::{
    self, Credentials, DeviceSession, PairingComplete, PairingCreate, PairingRequest, SessionToken,
};

pub(crate) enum SessionError {
    Unauthorized,
    Temporary,
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
        return Err(SessionError::Unauthorized);
    }
    if status != 200 {
        return Err(SessionError::Temporary);
    }
    let token: SessionToken = serde_json::from_value(value).map_err(|_| SessionError::Temporary)?;
    Ok(Credentials {
        device_id: id.into(),
        device_secret: secret.into(),
        access_token: token.access_token,
        access_expires_at: token.access_expires_at,
    })
}
