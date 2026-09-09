use crate::device_control::{
    self, Credentials, DeviceSession, PairingComplete, PairingCreate, PairingRequest, SessionToken,
};
use core::ffi::{c_char, c_int};
use serde_json::Value;
use std::ffi::{CStr, CString};

const RESPONSE_CAPACITY: usize = 65_537;

unsafe extern "C" {
    fn rock_platform_init() -> c_int;
    fn rock_identity_load(
        id: *mut c_char,
        id_cap: usize,
        secret: *mut c_char,
        secret_cap: usize,
    ) -> c_int;
    fn rock_identity_save(id: *const c_char, secret: *const c_char) -> c_int;
    fn rock_http_post(
        path: *const c_char,
        bearer: *const c_char,
        body: *const c_char,
        response: *mut c_char,
        response_cap: usize,
        status: *mut c_int,
    ) -> c_int;
    fn rock_ws_start(access_token: *const c_char) -> c_int;
    fn rock_ws_stop();
    fn rock_ws_send(data: *const c_char, len: usize) -> c_int;
    fn rock_ws_receive(data: *mut c_char, cap: usize, timeout_ms: u32) -> c_int;
    fn rock_now_rfc3339(out: *mut c_char, cap: usize) -> c_int;
    fn rock_random_fill(out: *mut u8, len: usize);
    fn rock_server_base_url() -> *const c_char;
    fn rock_hosted_recover();
    fn rock_ui_pairing_show(
        short_code: *const c_char,
        phrase: *const c_char,
        modules: *const u8,
        width: u16,
    ) -> bool;
    fn rock_delay_ms(milliseconds: u32);
}

fn cstring(value: &str) -> Result<CString, ()> {
    CString::new(value).map_err(|_| ())
}

fn post<T: serde::Serialize>(path: &str, body: &T) -> Result<(u16, Value), ()> {
    let path = cstring(path)?;
    let body = cstring(&serde_json::to_string(body).map_err(|_| ())?)?;
    let mut response = vec![0 as c_char; RESPONSE_CAPACITY];
    let mut status = 0;
    let len = unsafe {
        rock_http_post(
            path.as_ptr(),
            core::ptr::null(),
            body.as_ptr(),
            response.as_mut_ptr(),
            response.len(),
            &mut status,
        )
    };
    if len < 0 {
        return Err(());
    }
    let bytes = unsafe { CStr::from_ptr(response.as_ptr()) }.to_bytes();
    let json = if bytes.is_empty() {
        Value::Null
    } else {
        serde_json::from_slice(bytes).map_err(|_| ())?
    };
    Ok((status as u16, json))
}

fn now() -> Result<String, ()> {
    let mut buf = [0 as c_char; 32];
    if unsafe { rock_now_rfc3339(buf.as_mut_ptr(), buf.len()) } != 0 {
        return Err(());
    }
    Ok(unsafe { CStr::from_ptr(buf.as_ptr()) }
        .to_string_lossy()
        .into_owned())
}

fn message_id() -> String {
    let mut b = [0u8; 16];
    unsafe { rock_random_fill(b.as_mut_ptr(), b.len()) };
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

fn load_identity() -> Option<(String, String)> {
    let mut id = [0 as c_char; 64];
    let mut secret = [0 as c_char; 160];
    if unsafe { rock_identity_load(id.as_mut_ptr(), id.len(), secret.as_mut_ptr(), secret.len()) }
        != 1
    {
        return None;
    }
    Some((
        unsafe { CStr::from_ptr(id.as_ptr()) }
            .to_string_lossy()
            .into_owned(),
        unsafe { CStr::from_ptr(secret.as_ptr()) }
            .to_string_lossy()
            .into_owned(),
    ))
}

fn provision() -> Result<Credentials, ()> {
    let create = PairingCreate {
        device_display_name: "Rock ESP32",
        device_type: "esp32",
        app_version: env!("CARGO_PKG_VERSION"),
    };
    let (status, value) = post("/api/v1/pairing-requests", &create)?;
    if status != 201 {
        return Err(());
    }
    let request: PairingRequest = serde_json::from_value(value).map_err(|_| ())?;
    let base_url = unsafe { CStr::from_ptr(rock_server_base_url()) }.to_string_lossy();
    let link = request.deep_link(&base_url);
    let (width, modules) = device_control::qr_matrix(&link).map_err(|_| ())?;
    let code = cstring(&request.short_code)?;
    let phrase = cstring(&request.verification_phrase)?;
    unsafe {
        rock_ui_pairing_show(code.as_ptr(), phrase.as_ptr(), modules.as_ptr(), width);
    }
    let path = format!(
        "/api/v1/pairing-requests/{}/complete",
        request.pairing_request_id
    );
    loop {
        let (status, value) = post(
            &path,
            &PairingComplete {
                desktop_token: &request.desktop_token,
            },
        )?;
        if status == 200 {
            let credentials: Credentials = serde_json::from_value(value).map_err(|_| ())?;
            let id = cstring(&credentials.device_id)?;
            let secret = cstring(&credentials.device_secret)?;
            if unsafe { rock_identity_save(id.as_ptr(), secret.as_ptr()) } != 0 {
                return Err(());
            }
            return Ok(credentials);
        }
        if status != 202 {
            return Err(());
        }
        unsafe { rock_delay_ms(2_000) };
    }
}

fn session(id: &str, secret: &str) -> Result<Credentials, ()> {
    let (status, value) = post(
        "/api/v1/auth/device-session",
        &DeviceSession {
            device_id: id,
            device_secret: secret,
        },
    )?;
    if status != 200 {
        return Err(());
    }
    let token: SessionToken = serde_json::from_value(value).map_err(|_| ())?;
    Ok(Credentials {
        device_id: id.into(),
        device_secret: secret.into(),
        access_token: token.access_token,
        access_expires_at: token.access_expires_at,
    })
}

fn send(value: &Value) -> Result<(), ()> {
    let text = serde_json::to_string(value).map_err(|_| ())?;
    if text.len() > device_control::MAX_JSON_FRAME_BYTES {
        return Err(());
    }
    let c = cstring(&text)?;
    if unsafe { rock_ws_send(c.as_ptr(), text.len()) } < 0 {
        Err(())
    } else {
        Ok(())
    }
}

fn connected_loop(token: &str) -> Result<(), ()> {
    let token = cstring(token)?;
    if unsafe { rock_ws_start(token.as_ptr()) } != 0 {
        return Err(());
    }
    send(&device_control::hello(&message_id(), &now()?))?;
    let mut frame = vec![0 as c_char; RESPONSE_CAPACITY];
    let len = unsafe { rock_ws_receive(frame.as_mut_ptr(), frame.len(), 10_000) };
    if len <= 0 {
        unsafe { rock_ws_stop() };
        return Err(());
    }
    let welcome = device_control::parse_frame(unsafe {
        core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
    })
    .map_err(|_| ())?;
    if welcome.kind != "protocol.welcome" {
        unsafe { rock_ws_stop() };
        return Err(());
    }
    send(&device_control::registration(
        &message_id(),
        &now()?,
        env!("CARGO_PKG_VERSION"),
    ))?;
    loop {
        let len = unsafe { rock_ws_receive(frame.as_mut_ptr(), frame.len(), 20_000) };
        if len < 0 {
            unsafe { rock_ws_stop() };
            return Err(());
        }
        if len == 0 {
            continue;
        }
        if let Ok(inbound) = device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
        }) {
            if inbound.kind == "device.command" {
                send(&device_control::error_response(
                    &message_id(),
                    &now()?,
                    &inbound.message_id,
                    "capability_not_supported",
                ))?;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {
    esp_idf_sys::link_patches();
    esp_idf_sys::restore_posix_stdio_fds();
    if unsafe { rock_platform_init() } != 0 {
        loop {
            unsafe { rock_delay_ms(5_000) }
        }
    }
    let mut failures = 0u32;
    loop {
        let credentials = match load_identity() {
            Some((id, secret)) => session(&id, &secret),
            None => provision(),
        };
        if let Ok(credentials) = credentials {
            let _ = connected_loop(&credentials.access_token);
        }
        failures = failures.saturating_add(1).min(5);
        if failures == 5 {
            unsafe { rock_hosted_recover() }
        }
        let ceiling = (1u32 << failures).min(30) * 1_000;
        let mut random = [0u8; 4];
        unsafe { rock_random_fill(random.as_mut_ptr(), random.len()) };
        unsafe { rock_delay_ms(u32::from_le_bytes(random) % ceiling.max(1)) };
    }
}
