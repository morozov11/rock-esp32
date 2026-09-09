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

#[unsafe(no_mangle)]
pub extern "C" fn rock_qr_encode(
    text: *const core::ffi::c_char,
    out_buf: *mut u8,
    out_cap: usize,
    out_width: *mut u16,
) -> core::ffi::c_int {
    if text.is_null() || out_buf.is_null() || out_width.is_null() {
        return -1;
    }
    let cstr = unsafe { core::ffi::CStr::from_ptr(text) };
    let text_str = match cstr.to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };
    match qr_matrix(text_str) {
        Ok((width, modules)) => {
            if modules.len() > out_cap {
                return -2;
            }
            unsafe {
                core::ptr::copy_nonoverlapping(modules.as_ptr(), out_buf, modules.len());
                *out_width = width;
            }
            0
        }
        Err(_) => -3,
    }
}

