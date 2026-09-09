//! Narrow ESP-IDF FFI boundary; no protocol policy lives here.

use core::ffi::{c_char, c_int};
use serde::Serialize;
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
    fn rock_player_play_stream(stream_uri: *const c_char, station_id: *const c_char) -> c_int;
    fn rock_player_play() -> c_int;
    fn rock_player_pause() -> c_int;
    fn rock_player_stop() -> c_int;
    fn rock_player_set_volume(volume: u8) -> c_int;
    fn rock_player_set_mute(muted: bool) -> c_int;
    fn rock_player_get_state(
        status_buf: *mut c_char,
        status_cap: usize,
        station_buf: *mut c_char,
        station_cap: usize,
        vol: *mut u8,
        muted: *mut bool,
    ) -> c_int;
}

pub(crate) fn cstring(value: &str) -> Result<CString, ()> {
    CString::new(value).map_err(|_| ())
}
pub(crate) fn init() -> bool {
    unsafe { rock_platform_init() == 0 }
}
pub(crate) fn delay(ms: u32) {
    unsafe { rock_delay_ms(ms) }
}
pub(crate) fn recover() {
    unsafe { rock_hosted_recover() }
}

pub(crate) fn random_fill(out: &mut [u8]) {
    unsafe { rock_random_fill(out.as_mut_ptr(), out.len()) }
}
pub(crate) fn now() -> Result<String, ()> {
    let mut buf = [0 as c_char; 32];
    if unsafe { rock_now_rfc3339(buf.as_mut_ptr(), buf.len()) } != 0 {
        return Err(());
    }
    Ok(unsafe { CStr::from_ptr(buf.as_ptr()) }
        .to_string_lossy()
        .into_owned())
}
pub(crate) fn identity_load() -> Option<(String, String)> {
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
pub(crate) fn identity_save(id: &str, secret: &str) -> Result<(), ()> {
    let (id, secret) = (cstring(id)?, cstring(secret)?);
    if unsafe { rock_identity_save(id.as_ptr(), secret.as_ptr()) } == 0 {
        Ok(())
    } else {
        Err(())
    }
}
pub(crate) fn server_base_url() -> String {
    unsafe { CStr::from_ptr(rock_server_base_url()) }
        .to_string_lossy()
        .into_owned()
}
pub(crate) fn show_pairing(code: &str, phrase: &str, modules: &[u8], width: u16) -> Result<(), ()> {
    let (code, phrase) = (cstring(code)?, cstring(phrase)?);
    if unsafe { rock_ui_pairing_show(code.as_ptr(), phrase.as_ptr(), modules.as_ptr(), width) } {
        Ok(())
    } else {
        Err(())
    }
}
pub(crate) fn post<T: Serialize>(path: &str, body: &T) -> Result<(u16, Value), ()> {
    let (path, body) = (
        cstring(path)?,
        cstring(&serde_json::to_string(body).map_err(|_| ())?)?,
    );
    let mut response = vec![0 as c_char; RESPONSE_CAPACITY];
    let mut status = 0;
    if unsafe {
        rock_http_post(
            path.as_ptr(),
            core::ptr::null(),
            body.as_ptr(),
            response.as_mut_ptr(),
            response.len(),
            &mut status,
        )
    } < 0
    {
        return Err(());
    }
    let bytes = unsafe { CStr::from_ptr(response.as_ptr()) }.to_bytes();
    Ok((
        status as u16,
        if bytes.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(bytes).map_err(|_| ())?
        },
    ))
}
pub(crate) fn ws_start(token: &str) -> Result<(), ()> {
    let token = cstring(token)?;
    if unsafe { rock_ws_start(token.as_ptr()) } == 0 {
        Ok(())
    } else {
        Err(())
    }
}
pub(crate) fn ws_stop() {
    unsafe { rock_ws_stop() }
}
pub(crate) fn ws_send(text: &str) -> Result<(), ()> {
    let text = cstring(text)?;
    if unsafe { rock_ws_send(text.as_ptr(), text.as_bytes().len()) } < 0 {
        Err(())
    } else {
        Ok(())
    }
}
pub(crate) fn ws_receive(frame: &mut [c_char], timeout_ms: u32) -> i32 {
    unsafe { rock_ws_receive(frame.as_mut_ptr(), frame.len(), timeout_ms) }
}

pub(crate) fn player_play_stream(stream_uri: &str, station_id: &str) -> Result<(), ()> {
    let stream_uri = cstring(stream_uri)?;
    let station_id = cstring(station_id)?;
    if unsafe { rock_player_play_stream(stream_uri.as_ptr(), station_id.as_ptr()) } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn player_play() -> Result<(), ()> {
    if unsafe { rock_player_play() } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn player_pause() -> Result<(), ()> {
    if unsafe { rock_player_pause() } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn player_stop() -> Result<(), ()> {
    if unsafe { rock_player_stop() } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn player_set_volume(volume: u8) -> Result<(), ()> {
    if unsafe { rock_player_set_volume(volume) } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn player_set_mute(muted: bool) -> Result<(), ()> {
    if unsafe { rock_player_set_mute(muted) } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn player_get_status() -> Option<String> {
    let mut status_buf = [0 as c_char; 32];
    let mut station_buf = [0 as c_char; 128];
    let mut vol = 0u8;
    let mut muted = false;
    if unsafe {
        rock_player_get_state(
            status_buf.as_mut_ptr(),
            status_buf.len(),
            station_buf.as_mut_ptr(),
            station_buf.len(),
            &mut vol,
            &mut muted,
        )
    } == 0 {
        Some(unsafe { CStr::from_ptr(status_buf.as_ptr()) }.to_string_lossy().into_owned())
    } else {
        None
    }
}
