use serde::{Deserialize, Serialize};

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
