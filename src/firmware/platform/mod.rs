//! Narrow ESP-IDF FFI boundary; no protocol policy lives here.
//!
//! Phase-4 layout: ffi.rs holds every extern declaration and repr(C) mirror;
//! the sibling modules wrap one subsystem each (http/wss, gui, cast, tls).
//! This module re-exports the full surface, so consumers see one `platform`
//! path regardless of the split, and keeps the small cross-subsystem
//! helpers plus the device-control wrappers that did not warrant a file.

mod cast;
mod ffi;
mod gui;
mod http;
mod tls;

pub(crate) use cast::{cast_discover, cast_log};
pub(crate) use gui::{
    gui_catalog_append, gui_catalog_begin, gui_catalog_load_more_error, gui_catalog_search_busy,
    gui_dismiss_cast_menu, gui_dismiss_state, gui_flush_events,
    gui_mark_catalog_stale, gui_poll_event, gui_set_cast_devices, gui_set_search_results,
    gui_set_starting_station, gui_show_cast_menu, gui_show_empty, gui_show_error,
    gui_show_loading, gui_show_offline, gui_start, gui_update_cast_status,
    gui_update_connectivity, gui_update_playback, GuiEvent,
};
#[allow(unused_imports)]
pub(crate) use gui::gui_set_browse_stations;
pub(crate) use http::{
    get, log_msg, post, ws_log_heartbeat_timeout, ws_log_rx_frame, ws_receive, ws_send, ws_start,
    ws_stop,
};
pub(crate) use tls::CastTlsStream;

use ffi::*;
use std::ffi::{CStr, CString};

pub(crate) fn cstring(value: &str) -> Result<CString, ()> {
    CString::new(value).map_err(|_| ())
}

fn copy_str_to_buf(src: &str, dst: &mut [core::ffi::c_char]) {
    let bytes = src.as_bytes();
    let mut len = bytes.len().min(dst.len().saturating_sub(1));
    while !src.is_char_boundary(len) {
        len -= 1;
    }
    for i in 0..len {
        dst[i] = bytes[i] as core::ffi::c_char;
    }
    dst[len] = 0;
}

// ------------------------------------------------------------- platform ---

pub(crate) fn init() -> bool {
    unsafe { rock_platform_init() == 0 }
}
pub(crate) fn delay(ms: u32) {
    unsafe { rock_delay_ms(ms) }
}

pub(crate) fn random_fill(out: &mut [u8]) {
    unsafe { rock_random_fill(out.as_mut_ptr(), out.len()) }
}
pub(crate) fn now() -> Result<String, ()> {
    let mut buf = [0 as core::ffi::c_char; 32];
    if unsafe { rock_now_rfc3339(buf.as_mut_ptr(), buf.len()) } != 0 {
        return Err(());
    }
    Ok(unsafe { CStr::from_ptr(buf.as_ptr()) }
        .to_string_lossy()
        .into_owned())
}
pub(crate) fn uptime_ms() -> u64 {
    unsafe { rock_uptime_ms() }
}
pub(crate) fn wifi_ready() -> bool {
    unsafe { rock_wifi_is_connected() }
}
/// Raw link quality; -128 is the "no association" sentinel. Interpreting it
/// (the "wifi up" heuristic) belongs to the callers, not to this layer.
pub(crate) fn wifi_rssi() -> i32 {
    unsafe { rock_wifi_rssi() }
}

pub(crate) fn identity_load() -> Option<(String, String)> {
    let mut id = [0 as core::ffi::c_char; 64];
    let mut secret = [0 as core::ffi::c_char; 160];
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
pub(crate) fn identity_clear() {
    unsafe { rock_identity_clear() };
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

// -------------------------------------------------------------- player ---

pub(crate) fn player_play_stream(stream_uri: &str, station_id: &str, bearer: &str) -> Result<(), ()> {
    let stream_uri = cstring(stream_uri)?;
    let station_id = cstring(station_id)?;
    let bearer = cstring(bearer)?;
    if unsafe { rock_player_play_stream(stream_uri.as_ptr(), station_id.as_ptr(), bearer.as_ptr()) } == 0 {
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

pub(crate) fn player_stop_and_wait(timeout_ms: u32) -> Result<(), ()> {
    if unsafe { rock_player_stop_and_wait(timeout_ms) } == 0 {
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

/// Player status straight from the C domain enum (single source of truth;
/// unknown values degrade to Idle instead of guessing, mirroring the string
/// accessor's historical default).
pub(crate) fn player_status() -> crate::device_control::PlaybackStatus {
    match unsafe { rock_player_status_enum() } {
        1 => crate::device_control::PlaybackStatus::Buffering,
        2 => crate::device_control::PlaybackStatus::Playing,
        3 => crate::device_control::PlaybackStatus::Paused,
        4 => crate::device_control::PlaybackStatus::Stopped,
        5 => crate::device_control::PlaybackStatus::Error,
        _ => crate::device_control::PlaybackStatus::Idle,
    }
}

pub(crate) fn player_get_full_state(
) -> Option<(crate::device_control::PlaybackStatus, String, u8, bool)> {
    let mut station_buf = [0 as core::ffi::c_char; 129];
    let mut vol = 0u8;
    let mut muted = false;
    // Status comes from the enum accessor; the string slot in the C
    // signature stays for rock_serial diagnostics only.
    let mut status_scratch = [0 as core::ffi::c_char; 16];
    if unsafe {
        rock_player_get_state(
            status_scratch.as_mut_ptr(),
            status_scratch.len(),
            station_buf.as_mut_ptr(),
            station_buf.len(),
            &mut vol,
            &mut muted,
        )
    } == 0
    {
        let station = unsafe { CStr::from_ptr(station_buf.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        Some((player_status(), station, vol, muted))
    } else {
        None
    }
}

// ------------------------------------------------- unified stream source ---

pub(crate) fn source_start(upstream_uri: &str, bearer: &str) -> bool {
    let uri_c = match cstring(upstream_uri) {
        Ok(c) => c,
        Err(_) => return false,
    };
    let bearer_c = match cstring(bearer) {
        Ok(c) => c,
        Err(_) => return false,
    };
    unsafe { rock_source_start(uri_c.as_ptr(), bearer_c.as_ptr()) == 0 }
}

pub(crate) fn source_update_bearer(bearer: &str) -> bool {
    match cstring(bearer) {
        Ok(c) => unsafe { rock_source_update_bearer(c.as_ptr()) == 0 },
        Err(_) => false,
    }
}

pub(crate) fn source_stop() {
    unsafe { rock_source_stop() };
}

pub(crate) fn source_is_active() -> bool {
    unsafe { rock_source_is_active() }
}

pub(crate) fn source_get_meta_title() -> (u32, String) {
    let mut buf = [0 as core::ffi::c_char; 512];
    let generation = unsafe { rock_source_get_meta_title(buf.as_mut_ptr(), buf.len()) };
    let title = unsafe { CStr::from_ptr(buf.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    (generation, title)
}

pub(crate) fn source_meta_generation() -> u32 {
    unsafe { rock_source_meta_generation() }
}

pub(crate) fn source_clear_meta_title() {
    unsafe { rock_source_clear_meta_title() };
}

/// Content-type enum straight from the C source domain (single source of
/// truth; unknown values degrade to Unknown instead of guessing).
pub(crate) fn source_content_type_enum() -> crate::device_control::SourceContentType {
    match unsafe { rock_source_content_type_enum() } {
        1 => crate::device_control::SourceContentType::Mpeg,
        2 => crate::device_control::SourceContentType::Wav,
        3 => crate::device_control::SourceContentType::Aac,
        _ => crate::device_control::SourceContentType::Unknown,
    }
}

#[allow(dead_code)]
pub(crate) fn source_content_type() -> String {
    // Locked copy out of the C source domain: never borrows the mutable
    // static behind the pointer-version accessor.
    let mut buf = [0 as core::ffi::c_char; 128];
    if unsafe { rock_source_content_type_copy(buf.as_mut_ptr(), buf.len()) } < 0 {
        return String::new();
    }
    unsafe { CStr::from_ptr(buf.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

pub(crate) fn relay_listen() -> Option<String> {
    let mut buf = [0 as core::ffi::c_char; 128];
    let ret = unsafe { rock_relay_listen(buf.as_mut_ptr(), buf.len()) };
    if ret == 0 {
        Some(
            unsafe { CStr::from_ptr(buf.as_ptr()) }
                .to_string_lossy()
                .into_owned(),
        )
    } else {
        None
    }
}

pub(crate) fn relay_stop() {
    unsafe { rock_source_stop() };
}

#[allow(dead_code)]
pub(crate) fn relay_is_active() -> bool {
    unsafe { rock_source_is_active() }
}
