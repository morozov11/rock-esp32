//! Raw FFI surface: every extern declaration and repr(C) mirror of the C
//! ABI in one place. No wrappers, no policy — only shapes.

use core::ffi::{c_char, c_int, c_void};

unsafe extern "C" {
    pub(crate) fn rock_platform_init() -> c_int;
    pub(crate) fn rock_wifi_is_connected() -> bool;
    pub(crate) fn rock_wifi_rssi() -> c_int;
    pub(crate) fn rock_uptime_ms() -> u64;
    pub(crate) fn rock_identity_load(
        id: *mut c_char,
        id_cap: usize,
        secret: *mut c_char,
        secret_cap: usize,
    ) -> c_int;
    pub(crate) fn rock_identity_save(id: *const c_char, secret: *const c_char) -> c_int;
    pub(crate) fn rock_identity_clear() -> c_int;
    // rock_settings_volume_load is deliberately NOT bound: only app_main
    // (C composition root) reads the boot volume, before rust_main starts.
    pub(crate) fn rock_settings_volume_save(volume: u8) -> c_int;
    pub(crate) fn rock_http_post(
        path: *const c_char,
        bearer: *const c_char,
        body: *const c_char,
        response: *mut c_char,
        response_cap: usize,
        status: *mut c_int,
    ) -> c_int;
    pub(crate) fn rock_http_get(
        path: *const c_char,
        bearer: *const c_char,
        response: *mut c_char,
        response_cap: usize,
        status: *mut c_int,
    ) -> c_int;
    pub(crate) fn rock_gui_start() -> c_int;
    pub(crate) fn rock_gui_poll_event(out_event: *mut RockUiEvent, timeout_ms: u32) -> bool;
    pub(crate) fn rock_gui_flush_events();
    pub(crate) fn rock_gui_update_playback(
        status: c_int,
        station_id: *const c_char,
        station_name: *const c_char,
        title: *const c_char,
        subtitle: *const c_char,
        volume: u8,
        muted: bool,
    );
    pub(crate) fn rock_gui_update_connectivity(rssi: c_int, wifi_connected: bool, server_connected: bool);
    pub(crate) fn rock_gui_catalog_begin(
        items: *const RockGuiStationItem,
        count: usize,
        next_cursor: *const c_char,
    );
    pub(crate) fn rock_gui_catalog_append(
        items: *const RockGuiStationItem,
        count: usize,
        next_cursor: *const c_char,
    );
    #[allow(dead_code)]
    pub(crate) fn rock_gui_set_browse_stations(
        items: *const RockGuiStationItem,
        count: usize,
        next_cursor: *const c_char,
    );
    pub(crate) fn rock_gui_set_search_results(items: *const RockGuiStationItem, count: usize);
    pub(crate) fn rock_gui_catalog_load_more_error();
    pub(crate) fn rock_gui_catalog_search_busy(busy: bool);
    pub(crate) fn rock_gui_show_loading(message: *const c_char);
    pub(crate) fn rock_gui_show_empty(message: *const c_char);
    #[allow(dead_code)]
    pub(crate) fn rock_gui_show_offline(message: *const c_char);
    pub(crate) fn rock_gui_show_error(message: *const c_char);
    pub(crate) fn rock_gui_dismiss_state();
    pub(crate) fn rock_gui_mark_catalog_stale();
    pub(crate) fn rock_gui_set_starting_station(station_id: *const c_char);
    pub(crate) fn rock_ws_start(access_token: *const c_char) -> c_int;
    pub(crate) fn rock_ws_stop();
    pub(crate) fn rock_ws_send(data: *const c_char, len: usize) -> c_int;
    pub(crate) fn rock_ws_receive(data: *mut c_char, cap: usize, timeout_ms: u32) -> c_int;
    pub(crate) fn rock_ws_log_heartbeat_timeout(age_ms: u64);
    pub(crate) fn rock_ws_log_rx_frame(kind: *const c_char);
    pub(crate) fn rock_esp_log(msg: *const c_char);
    pub(crate) fn rock_now_rfc3339(out: *mut c_char, cap: usize) -> c_int;
    pub(crate) fn rock_random_fill(out: *mut u8, len: usize);
    pub(crate) fn rock_server_base_url() -> *const c_char;
    // rock_hosted_recover is deliberately NOT bound: the only caller is the
    // C Wi-Fi event handler (rock_wifi_sta.c); the Rust wrapper was dead.
    pub(crate) fn rock_ui_pairing_show(
        short_code: *const c_char,
        phrase: *const c_char,
        modules: *const u8,
        width: u16,
    ) -> bool;
    pub(crate) fn rock_delay_ms(milliseconds: u32);
    pub(crate) fn rock_player_play_stream(stream_uri: *const c_char, station_id: *const c_char, bearer: *const c_char) -> c_int;
    pub(crate) fn rock_player_play() -> c_int;
    pub(crate) fn rock_player_pause() -> c_int;
    pub(crate) fn rock_player_stop() -> c_int;
    pub(crate) fn rock_player_stop_and_wait(timeout_ms: u32) -> c_int;
    pub(crate) fn rock_player_set_volume(volume: u8) -> c_int;
    pub(crate) fn rock_player_set_mute(muted: bool) -> c_int;
    pub(crate) fn rock_player_get_state(
        status_buf: *mut c_char,
        status_cap: usize,
        station_buf: *mut c_char,
        station_cap: usize,
        vol: *mut u8,
        muted: *mut bool,
    ) -> c_int;
    pub(crate) fn rock_player_status_enum() -> c_int;
    pub(crate) fn rock_cast_discover(
        timeout_ms: u32,
        out_devices: *mut RockCastDeviceRaw,
        max_devices: usize,
    ) -> c_int;
    pub(crate) fn rock_cast_tls_open(host: *const c_char, port: u16, timeout_ms: u32) -> *mut c_void;
    pub(crate) fn rock_cast_tls_send(
        tls: *mut c_void,
        data: *const u8,
        len: usize,
        timeout_ms: u32,
    ) -> c_int;
    pub(crate) fn rock_cast_tls_recv(
        tls: *mut c_void,
        data: *mut u8,
        max_len: usize,
        timeout_ms: u32,
    ) -> c_int;
    pub(crate) fn rock_cast_tls_close(tls: *mut c_void);
    pub(crate) fn rock_cast_log(msg: *const c_char);
    pub(crate) fn rock_gui_show_cast_menu(scanning: bool);
    pub(crate) fn rock_gui_set_cast_devices(items: *const RockGuiCastItemRaw, count: usize);
    pub(crate) fn rock_gui_dismiss_cast_menu();
    pub(crate) fn rock_gui_update_cast_status(is_casting: bool, receiver_name: *const c_char);
    pub(crate) fn rock_source_start(upstream_uri: *const c_char, bearer: *const c_char) -> c_int;
    pub(crate) fn rock_source_update_bearer(bearer: *const c_char) -> c_int;
    pub(crate) fn rock_source_stop();
    pub(crate) fn rock_source_is_active() -> bool;
    // Pointer version deliberately NOT bound from Rust (mutable static):
    // C keeps it for its own serve loop; Rust uses the enum + locked copy.
    pub(crate) fn rock_source_content_type_enum() -> c_int;
    pub(crate) fn rock_source_content_type_copy(out: *mut c_char, cap: usize) -> c_int;
    pub(crate) fn rock_source_get_meta_title(buf: *mut c_char, cap: usize) -> u32;
    pub(crate) fn rock_source_meta_generation() -> u32;
    pub(crate) fn rock_source_clear_meta_title();
    pub(crate) fn rock_relay_listen(out_url: *mut c_char, out_cap: usize) -> c_int;
}

#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct RockCastDeviceRaw {
    pub id: [c_char; 65],
    pub name: [c_char; 129],
    pub model: [c_char; 65],
    pub host: [c_char; 65],
    pub port: u16,
    /// Raw eureka_info body staged by the C subnet scanner; empty for mDNS
    /// hits or failed probes. Paired with rock_cast_device_t in rock_cast_discovery.h.
    pub eureka_json: [c_char; 1024],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct RockGuiCastItemRaw {
    pub receiver_id: [c_char; 65],
    pub display_name: [c_char; 129],
    pub is_active: bool,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct RockGuiStationItem {
    pub id: [c_char; 129],
    pub name: [c_char; 129],
    pub tags: [c_char; 64],
    pub codec: [c_char; 16],
    pub bitrate_kbps: u32,
    pub is_playing: bool,
}

#[repr(C)]
pub(crate) struct RockUiEvent {
    pub evt_type: i32,
    pub text: [c_char; 513],
    pub value: i16,
}
