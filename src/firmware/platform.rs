//! Narrow ESP-IDF FFI boundary; no protocol policy lives here.

use core::ffi::{c_char, c_int, c_void};
use serde::Serialize;
use serde_json::Value;
use std::ffi::{CStr, CString};

const RESPONSE_CAPACITY: usize = 65_537;

unsafe extern "C" {
    fn rock_platform_init() -> c_int;
    fn rock_wifi_is_connected() -> bool;
    fn rock_wifi_rssi() -> c_int;
    fn rock_uptime_ms() -> u64;
    fn rock_identity_load(
        id: *mut c_char,
        id_cap: usize,
        secret: *mut c_char,
        secret_cap: usize,
    ) -> c_int;
    fn rock_identity_save(id: *const c_char, secret: *const c_char) -> c_int;
    fn rock_identity_clear() -> c_int;
    fn rock_http_post(
        path: *const c_char,
        bearer: *const c_char,
        body: *const c_char,
        response: *mut c_char,
        response_cap: usize,
        status: *mut c_int,
    ) -> c_int;
    fn rock_http_get(
        path: *const c_char,
        bearer: *const c_char,
        response: *mut c_char,
        response_cap: usize,
        status: *mut c_int,
    ) -> c_int;
    fn rock_gui_start() -> c_int;
    fn rock_gui_poll_event(out_event: *mut RockUiEvent, timeout_ms: u32) -> bool;
    fn rock_gui_update_playback(
        status: c_int,
        station_id: *const c_char,
        title: *const c_char,
        subtitle: *const c_char,
        volume: u8,
        muted: bool,
    );
    fn rock_gui_update_connectivity(rssi: c_int, wifi_connected: bool, server_connected: bool);
    fn rock_gui_set_browse_stations(
        items: *const RockGuiStationItem,
        count: usize,
        next_cursor: *const c_char,
    );
    fn rock_gui_set_search_results(items: *const RockGuiStationItem, count: usize);
    fn rock_gui_show_loading(message: *const c_char);
    fn rock_gui_show_empty(message: *const c_char);
    #[allow(dead_code)]
    fn rock_gui_show_offline(message: *const c_char);
    fn rock_gui_show_error(message: *const c_char);
    fn rock_gui_dismiss_state();
    fn rock_gui_mark_catalog_stale();
    fn rock_gui_set_starting_station(station_id: *const c_char);
    fn rock_ws_start(access_token: *const c_char) -> c_int;
    fn rock_ws_stop();
    fn rock_ws_send(data: *const c_char, len: usize) -> c_int;
    fn rock_ws_receive(data: *mut c_char, cap: usize, timeout_ms: u32) -> c_int;
    fn rock_ws_log_heartbeat_timeout(age_ms: u64);
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
    fn rock_player_play_stream(stream_uri: *const c_char, station_id: *const c_char, bearer: *const c_char) -> c_int;
    fn rock_player_update_bearer(bearer: *const c_char) -> c_int;
    fn rock_player_play() -> c_int;
    fn rock_player_pause() -> c_int;
    fn rock_player_stop() -> c_int;
    fn rock_player_stop_and_wait(timeout_ms: u32) -> c_int;
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
    fn rock_cast_discover(
        timeout_ms: u32,
        out_devices: *mut RockCastDeviceRaw,
        max_devices: usize,
    ) -> c_int;
    fn rock_cast_tls_open(host: *const c_char, port: u16, timeout_ms: u32) -> *mut c_void;
    fn rock_cast_tls_send(
        tls: *mut c_void,
        data: *const u8,
        len: usize,
        timeout_ms: u32,
    ) -> c_int;
    fn rock_cast_tls_recv(
        tls: *mut c_void,
        data: *mut u8,
        max_len: usize,
        timeout_ms: u32,
    ) -> c_int;
    fn rock_cast_tls_close(tls: *mut c_void);
    fn rock_cast_log(msg: *const c_char);

    fn rock_gui_show_cast_menu(scanning: bool);
    fn rock_gui_set_cast_devices(items: *const RockGuiCastItemRaw, count: usize);
    fn rock_gui_dismiss_cast_menu();
    fn rock_gui_update_cast_status(is_casting: bool, receiver_name: *const c_char);

    fn rock_source_start(upstream_uri: *const c_char, bearer: *const c_char) -> c_int;
    fn rock_source_update_bearer(bearer: *const c_char) -> c_int;
    fn rock_source_stop();
    fn rock_source_is_active() -> bool;
    fn rock_source_content_type() -> *const c_char;
    fn rock_relay_listen(out_url: *mut c_char, out_cap: usize) -> c_int;
    fn rock_relay_stop();
    fn rock_relay_is_active() -> bool;
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RockCastDeviceRaw {
    pub id: [c_char; 65],
    pub name: [c_char; 129],
    pub model: [c_char; 65],
    pub host: [c_char; 65],
    pub port: u16,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RockGuiCastItemRaw {
    pub receiver_id: [c_char; 65],
    pub display_name: [c_char; 129],
    pub is_active: bool,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RockGuiStationItem {
    pub id: [c_char; 129],
    pub name: [c_char; 129],
    pub tags: [c_char; 64],
    pub codec: [c_char; 16],
    pub bitrate_kbps: u32,
    pub is_playing: bool,
}

#[repr(C)]
pub struct RockUiEvent {
    pub evt_type: i32,
    pub text: [c_char; 513],
    pub value: i16,
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

pub(crate) fn player_update_bearer(bearer: &str) -> Result<(), ()> {
    let bearer = cstring(bearer)?;
    if unsafe { rock_player_update_bearer(bearer.as_ptr()) } == 0 {
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

#[allow(dead_code)]
pub(crate) fn player_get_full_state() -> Option<(String, String, u8, bool)> {
    let mut status_buf = [0 as c_char; 32];
    let mut station_buf = [0 as c_char; 129];
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
    } == 0
    {
        let status = unsafe { CStr::from_ptr(status_buf.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let station = unsafe { CStr::from_ptr(station_buf.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        Some((status, station, vol, muted))
    } else {
        None
    }
}

#[derive(Clone, Debug)]
pub(crate) enum GuiEvent {
    PlayStation(String),
    PlaybackPlay,
    PlaybackPause,
    PlaybackStop,
    VolumeSet(u8),
    VolumeChange(i16),
    CatalogBrowse,
    CatalogSearch(String),
    Retry,
    CastBrowse,
    CastSelect(String),
    CastDisconnect,
}

pub(crate) fn gui_poll_event(timeout_ms: u32) -> Option<GuiEvent> {
    let mut raw = RockUiEvent {
        evt_type: 0,
        text: [0; 513],
        value: 0,
    };
    if unsafe { rock_gui_poll_event(&mut raw, timeout_ms) } {
        let text = unsafe { CStr::from_ptr(raw.text.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        match raw.evt_type {
            1 => Some(GuiEvent::PlayStation(text)),
            2 => Some(GuiEvent::PlaybackPlay),
            3 => Some(GuiEvent::PlaybackPause),
            4 => Some(GuiEvent::PlaybackStop),
            5 => Some(GuiEvent::VolumeSet(raw.value as u8)),
            6 => Some(GuiEvent::VolumeChange(raw.value)),
            7 => Some(GuiEvent::CatalogBrowse),
            8 => Some(GuiEvent::CatalogSearch(text)),
            9 => Some(GuiEvent::Retry),
            10 => Some(GuiEvent::CastBrowse),
            11 => Some(GuiEvent::CastSelect(text)),
            12 => Some(GuiEvent::CastDisconnect),
            _ => None,
        }
    } else {
        None
    }
}

pub(crate) fn gui_start() -> Result<(), ()> {
    if unsafe { rock_gui_start() } == 0 {
        Ok(())
    } else {
        Err(())
    }
}

pub(crate) fn gui_update_playback(
    status: i32,
    station_id: &str,
    title: &str,
    subtitle: &str,
    volume: u8,
    muted: bool,
) {
    let st_c = cstring(station_id).ok();
    let ti_c = cstring(title).ok();
    let su_c = cstring(subtitle).ok();
    unsafe {
        rock_gui_update_playback(
            status,
            st_c.as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(core::ptr::null()),
            ti_c.as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(core::ptr::null()),
            su_c.as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(core::ptr::null()),
            volume,
            muted,
        );
    }
}

pub(crate) fn gui_update_connectivity(rssi: i32, wifi: bool, server: bool) {
    unsafe { rock_gui_update_connectivity(rssi, wifi, server) };
}

pub(crate) fn gui_set_browse_stations(
    stations: &[crate::device_control::catalog::DeviceStationDto],
    next_cursor: Option<&str>,
) {
    let mut items = [RockGuiStationItem {
        id: [0; 129],
        name: [0; 129],
        tags: [0; 64],
        codec: [0; 16],
        bitrate_kbps: 0,
        is_playing: false,
    }; 20];
    let count = stations.len().min(20);
    for i in 0..count {
        copy_str_to_buf(&stations[i].id, &mut items[i].id);
        copy_str_to_buf(&stations[i].name, &mut items[i].name);
        copy_str_to_buf(&stations[i].tags.join(", "), &mut items[i].tags);
        if let Some(ref c) = stations[i].codec {
            copy_str_to_buf(c, &mut items[i].codec);
        }
        items[i].bitrate_kbps = stations[i].bitrate_kbps.unwrap_or(0);
        items[i].is_playing = false;
    }
    let cursor_c = next_cursor.and_then(|c| cstring(c).ok());
    let cursor_ptr = cursor_c
        .as_ref()
        .map(|c| c.as_ptr())
        .unwrap_or(core::ptr::null());
    unsafe { rock_gui_set_browse_stations(items.as_ptr(), count, cursor_ptr) };
}

pub(crate) fn gui_set_search_results(
    stations: &[crate::device_control::catalog::DeviceStationDto],
) {
    let mut items = [RockGuiStationItem {
        id: [0; 129],
        name: [0; 129],
        tags: [0; 64],
        codec: [0; 16],
        bitrate_kbps: 0,
        is_playing: false,
    }; 20];
    let count = stations.len().min(20);
    for i in 0..count {
        copy_str_to_buf(&stations[i].id, &mut items[i].id);
        copy_str_to_buf(&stations[i].name, &mut items[i].name);
        copy_str_to_buf(&stations[i].tags.join(", "), &mut items[i].tags);
        if let Some(ref c) = stations[i].codec {
            copy_str_to_buf(c, &mut items[i].codec);
        }
        items[i].bitrate_kbps = stations[i].bitrate_kbps.unwrap_or(0);
        items[i].is_playing = false;
    }
    unsafe { rock_gui_set_search_results(items.as_ptr(), count) };
}

pub(crate) fn gui_show_loading(msg: &str) {
    if let Ok(c) = cstring(msg) {
        unsafe { rock_gui_show_loading(c.as_ptr()) };
    }
}

pub(crate) fn gui_show_empty(msg: &str) {
    if let Ok(c) = cstring(msg) {
        unsafe { rock_gui_show_empty(c.as_ptr()) };
    }
}

#[allow(dead_code)]
pub(crate) fn gui_show_offline(msg: &str) {
    if let Ok(c) = cstring(msg) {
        unsafe { rock_gui_show_offline(c.as_ptr()) };
    }
}

pub(crate) fn gui_show_error(msg: &str) {
    if let Ok(c) = cstring(msg) {
        unsafe { rock_gui_show_error(c.as_ptr()) };
    }
}

pub(crate) fn gui_dismiss_state() {
    unsafe { rock_gui_dismiss_state() };
}

pub(crate) fn gui_mark_catalog_stale() {
    unsafe { rock_gui_mark_catalog_stale() };
}

pub(crate) fn gui_set_starting_station(station_id: &str) {
    if let Ok(c) = cstring(station_id) {
        unsafe { rock_gui_set_starting_station(c.as_ptr()) };
    }
}

fn copy_str_to_buf(src: &str, dst: &mut [c_char]) {
    let bytes = src.as_bytes();
    let mut len = bytes.len().min(dst.len().saturating_sub(1));
    while !src.is_char_boundary(len) {
        len -= 1;
    }
    for i in 0..len {
        dst[i] = bytes[i] as c_char;
    }
    dst[len] = 0;
}

pub(crate) fn uptime_ms() -> u64 {
    unsafe { rock_uptime_ms() }
}
pub(crate) fn wifi_ready() -> bool {
    unsafe { rock_wifi_is_connected() }
}
pub(crate) fn gui_connectivity(server: bool) {
    let rssi = unsafe { rock_wifi_rssi() };
    gui_update_connectivity(rssi, rssi != -128, server && rssi != -128);
}

pub(crate) fn cast_discover(timeout_ms: u32) -> Vec<crate::device_control::cast::DiscoveredReceiver> {
    let mut raw = [RockCastDeviceRaw {
        id: [0; 65],
        name: [0; 129],
        model: [0; 65],
        host: [0; 65],
        port: 0,
    }; 16];
    let count = unsafe { rock_cast_discover(timeout_ms, raw.as_mut_ptr(), raw.len()) };
    if count <= 0 {
        return Vec::new();
    }
    let count = (count as usize).min(raw.len());
    let mut out = Vec::with_capacity(count);
    for item in &raw[..count] {
        let id = unsafe { CStr::from_ptr(item.id.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let name = unsafe { CStr::from_ptr(item.name.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let model = unsafe { CStr::from_ptr(item.model.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let host = unsafe { CStr::from_ptr(item.host.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let port = item.port;
        if !host.is_empty() {
            out.push(crate::device_control::cast::DiscoveredReceiver {
                key: if !id.is_empty() { id } else { format!("{host}:{port}") },
                name,
                model,
                host,
                port,
            });
        }
    }
    out
}

pub(crate) struct CastTlsStream(*mut c_void);

unsafe impl Send for CastTlsStream {}

impl CastTlsStream {
    pub(crate) fn connect(host: &str, port: u16, timeout_ms: u32) -> Option<Self> {
        let host_c = cstring(host).ok()?;
        let ptr = unsafe { rock_cast_tls_open(host_c.as_ptr(), port, timeout_ms) };
        if ptr.is_null() {
            None
        } else {
            Some(Self(ptr))
        }
    }

    pub(crate) fn send(&mut self, data: &[u8], timeout_ms: u32) -> Result<(), ()> {
        let ret = unsafe { rock_cast_tls_send(self.0, data.as_ptr(), data.len(), timeout_ms) };
        if ret == data.len() as c_int {
            Ok(())
        } else {
            Err(())
        }
    }

    pub(crate) fn recv(&mut self, buf: &mut [u8], timeout_ms: u32) -> Result<usize, ()> {
        let ret = unsafe { rock_cast_tls_recv(self.0, buf.as_mut_ptr(), buf.len(), timeout_ms) };
        if ret > 0 {
            Ok(ret as usize)
        } else if ret == 0 {
            Ok(0)
        } else {
            Err(())
        }
    }
}

impl Drop for CastTlsStream {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { rock_cast_tls_close(self.0) };
            self.0 = core::ptr::null_mut();
        }
    }
}

pub(crate) fn cast_log(msg: &str) {
    if let Ok(c) = cstring(msg) {
        unsafe { rock_cast_log(c.as_ptr()) };
    }
}

pub(crate) fn gui_show_cast_menu(scanning: bool) {
    unsafe { rock_gui_show_cast_menu(scanning) };
}

pub(crate) fn gui_dismiss_cast_menu() {
    unsafe { rock_gui_dismiss_cast_menu() };
}

pub(crate) fn gui_set_cast_devices(
    items: &[crate::device_control::cast::ChromecastReceiverInfo],
    active_receiver_id: Option<&str>,
) {
    let mut raw = [RockGuiCastItemRaw {
        receiver_id: [0; 65],
        display_name: [0; 129],
        is_active: false,
    }; 16];
    let count = items.len().min(raw.len());
    for i in 0..count {
        let rec = &items[i];
        copy_str_to_buf(&rec.receiver_id, &mut raw[i].receiver_id);
        copy_str_to_buf(&rec.display_name, &mut raw[i].display_name);
        raw[i].is_active = active_receiver_id == Some(rec.receiver_id.as_str());
    }
    unsafe { rock_gui_set_cast_devices(raw.as_ptr(), count) };
}

pub(crate) fn gui_update_cast_status(is_casting: bool, receiver_name: Option<&str>) {
    let name_c = receiver_name.and_then(|n| cstring(n).ok());
    unsafe {
        rock_gui_update_cast_status(
            is_casting,
            name_c.as_ref().map(|s| s.as_ptr()).unwrap_or(core::ptr::null()),
        )
    };
}

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

pub(crate) fn source_content_type() -> String {
    unsafe { CStr::from_ptr(rock_source_content_type()) }
        .to_string_lossy()
        .into_owned()
}

pub(crate) fn relay_listen() -> Option<String> {
    let mut buf = [0 as c_char; 128];
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
    unsafe { rock_relay_stop() };
}

#[allow(dead_code)]
pub(crate) fn relay_is_active() -> bool {
    unsafe { rock_relay_is_active() }
}
