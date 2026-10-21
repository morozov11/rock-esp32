//! HTTP/WSS client wrappers and their buffer semantics.

use super::ffi::*;
use super::cstring;
use core::ffi::c_char;
use serde::Serialize;
use serde_json::Value;
use std::ffi::CStr;

const RESPONSE_CAPACITY: usize = 65_537;
const SYNC_RESPONSE_CAPACITY: usize = 512 * 1024;

pub(crate) fn post<T: Serialize>(path: &str, body: &T) -> Result<(u16, Value), ()> {
    let (path, body) = (
        cstring(path)?,
        cstring(&serde_json::to_string(body).map_err(|_| ())?)?,
    );
    let mut response = vec![0 as c_char; RESPONSE_CAPACITY];
    let mut status = 0;
    let res = unsafe {
        rock_http_post(
            path.as_ptr(),
            core::ptr::null(),
            body.as_ptr(),
            response.as_mut_ptr(),
            response.len(),
            &mut status,
        )
    };
    if res < 0 && status == 0 {
        return Err(());
    }
    let bytes = unsafe { CStr::from_ptr(response.as_ptr()) }.to_bytes();
    Ok((
        status as u16,
        if bytes.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(bytes).unwrap_or(Value::Null)
        },
    ))
}

pub(crate) fn post_sync<T: Serialize>(body: &T, bearer: &str) -> Result<(u16, Vec<u8>), ()> {
    let path = cstring("/api/v1/sync")?;
    let bearer = cstring(bearer)?;
    let body = cstring(&serde_json::to_string(body).map_err(|_| ())?)?;
    let mut response = vec![0u8; SYNC_RESPONSE_CAPACITY];
    let mut status = 0;
    let res = unsafe { rock_http_post(path.as_ptr(), bearer.as_ptr(), body.as_ptr(),
                                       response.as_mut_ptr().cast(), response.len(), &mut status) };
    if res < 0 || status == 0 { return Err(()); }
    response.truncate(res as usize);
    Ok((status as u16, response))
}

pub(crate) fn get(path: &str, bearer: Option<&str>) -> Result<(u16, Vec<u8>), ()> {
    let path = cstring(path)?;
    let bearer_c = match bearer {
        Some(b) => Some(cstring(b)?),
        None => None,
    };
    let bearer_ptr = bearer_c
        .as_ref()
        .map(|b| b.as_ptr())
        .unwrap_or(core::ptr::null());
    let mut response = vec![0 as c_char; RESPONSE_CAPACITY];
    let mut status = 0;
    let res = unsafe {
        rock_http_get(
            path.as_ptr(),
            bearer_ptr,
            response.as_mut_ptr(),
            response.len(),
            &mut status,
        )
    };
    if res < 0 && status == 0 {
        return Err(());
    }
    let bytes = unsafe { CStr::from_ptr(response.as_ptr()) }
        .to_bytes()
        .to_vec();
    Ok((status as u16, bytes))
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

pub(crate) fn ws_log_heartbeat_timeout(age_ms: u64) {
    unsafe { rock_ws_log_heartbeat_timeout(age_ms) }
}

pub(crate) fn ws_log_rx_frame(kind: &str) {
    if let Ok(kind) = cstring(kind) {
        unsafe { rock_ws_log_rx_frame(kind.as_ptr()) }
    }
}

/// Session-domain log line (TAG rock-control) for errors the session
/// deliberately swallows instead of propagating. Same contract as the
/// ws_log_* helpers: text only, never secrets or frame content.
pub(crate) fn log_msg(msg: &str) {
    if let Ok(msg) = cstring(msg) {
        unsafe { rock_esp_log(msg.as_ptr()) }
    }
}
