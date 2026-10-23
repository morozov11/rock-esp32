//! GUI FFI wrappers: state updates, catalogs, overlays, cast menu, and the
//! GuiEvent decoding from the C event queue.

use super::copy_str_to_buf;
use super::ffi::*;
use super::cstring;

use std::ffi::CStr;

#[derive(Clone, Debug)]
pub(crate) enum GuiEvent {
    PlayStation(String),
    PlaybackPlay,
    PlaybackPause,
    PlaybackStop,
    VolumeChange(i16),
    // Empty cursor = first-page refresh (what C sends today); non-empty =
    // load-more after that keyset cursor.
    CatalogBrowse(String),
    CatalogSearch(String),
    Retry,
    CastBrowse,
    CastSelect(String),
    CastDisconnect,
    FavouritesShow,
    FavouritesPage(u16),
    FavouriteToggle(String),
    VoiceToggle,
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
        // Event numbers are the generated mirror of rock_ui_event_type_t in
        // main/gui/rock_gui.h (phase 5 A4) — no hand-copied literals.
        match raw.evt_type as u64 {
            crate::abi::ROCK_UI_EVT_PLAY_STATION => Some(GuiEvent::PlayStation(text)),
            crate::abi::ROCK_UI_EVT_PLAYBACK_PLAY => Some(GuiEvent::PlaybackPlay),
            crate::abi::ROCK_UI_EVT_PLAYBACK_PAUSE => Some(GuiEvent::PlaybackPause),
            crate::abi::ROCK_UI_EVT_PLAYBACK_STOP => Some(GuiEvent::PlaybackStop),
            crate::abi::ROCK_UI_EVT_VOLUME_CHANGE => Some(GuiEvent::VolumeChange(raw.value)),
            crate::abi::ROCK_UI_EVT_CATALOG_BROWSE => Some(GuiEvent::CatalogBrowse(text)),
            crate::abi::ROCK_UI_EVT_CATALOG_SEARCH => Some(GuiEvent::CatalogSearch(text)),
            crate::abi::ROCK_UI_EVT_RETRY => Some(GuiEvent::Retry),
            crate::abi::ROCK_UI_EVT_CAST_BROWSE => Some(GuiEvent::CastBrowse),
            crate::abi::ROCK_UI_EVT_CAST_SELECT => Some(GuiEvent::CastSelect(text)),
            crate::abi::ROCK_UI_EVT_CAST_DISCONNECT => Some(GuiEvent::CastDisconnect),
            crate::abi::ROCK_UI_EVT_FAVOURITES_SHOW => Some(GuiEvent::FavouritesShow),
            crate::abi::ROCK_UI_EVT_FAVOURITES_PAGE => Some(GuiEvent::FavouritesPage(raw.value.max(0) as u16)),
            crate::abi::ROCK_UI_EVT_FAVOURITE_TOGGLE => Some(GuiEvent::FavouriteToggle(text)),
            crate::abi::ROCK_UI_EVT_VOICE_TOGGLE => Some(GuiEvent::VoiceToggle),
            _ => None,
        }
    } else {
        None
    }
}

pub(crate) fn gui_voice_state(state: i32) {
    unsafe { rock_gui_voice_state(state) };
}

pub(crate) fn gui_flush_events() {
    unsafe { rock_gui_flush_events() };
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
    station_name: &str,
    title: &str,
    subtitle: &str,
    volume: u8,
    muted: bool,
) {
    let st_c = cstring(station_id).ok();
    let nm_c = cstring(station_name).ok();
    let ti_c = cstring(title).ok();
    let su_c = cstring(subtitle).ok();
    unsafe {
        rock_gui_update_playback(
            status,
            st_c.as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(core::ptr::null()),
            nm_c.as_ref()
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

fn convert_stations(
    stations: &[crate::device_control::catalog::DeviceStationDto],
) -> Vec<RockGuiStationItem> {
    let count = stations.len().min(20);
    let mut items = Vec::with_capacity(count);
    for s in stations.iter().take(count) {
        let mut item = RockGuiStationItem {
            id: [0; 129],
            name: [0; 129],
            favicon_url: [0; 512],
            tags: [0; 64],
            codec: [0; 16],
            bitrate_kbps: s.bitrate_kbps.unwrap_or(0),
            is_playing: false,
            is_available: !s.tags.iter().any(|t| t == "Недоступна в каталоге" || t == "Загрузка..." || t == "Нет связи с каталогом"),
        };
        copy_str_to_buf(&s.id, &mut item.id);
        copy_str_to_buf(&s.name, &mut item.name);
        if let Some(ref url) = s.favicon_url {
            copy_str_to_buf(url, &mut item.favicon_url);
        }
        copy_str_to_buf(&s.tags.join(", "), &mut item.tags);
        if let Some(ref c) = s.codec {
            copy_str_to_buf(c, &mut item.codec);
        }
        items.push(item);
    }
    items
}

pub(crate) fn gui_catalog_begin(
    stations: &[crate::device_control::catalog::DeviceStationDto],
    next_cursor: Option<&str>,
) {
    let items = convert_stations(stations);
    let cursor_c = next_cursor.and_then(|c| cstring(c).ok());
    let cursor_ptr = cursor_c
        .as_ref()
        .map(|c| c.as_ptr())
        .unwrap_or(core::ptr::null());
    let items_ptr = if items.is_empty() {
        core::ptr::null()
    } else {
        items.as_ptr()
    };
    unsafe { rock_gui_catalog_begin(items_ptr, items.len(), cursor_ptr) };
}

pub(crate) fn gui_catalog_append(
    stations: &[crate::device_control::catalog::DeviceStationDto],
    next_cursor: Option<&str>,
) {
    let items = convert_stations(stations);
    let cursor_c = next_cursor.and_then(|c| cstring(c).ok());
    let cursor_ptr = cursor_c
        .as_ref()
        .map(|c| c.as_ptr())
        .unwrap_or(core::ptr::null());
    let items_ptr = if items.is_empty() {
        core::ptr::null()
    } else {
        items.as_ptr()
    };
    unsafe { rock_gui_catalog_append(items_ptr, items.len(), cursor_ptr) };
}

#[allow(dead_code)]
pub(crate) fn gui_set_browse_stations(
    stations: &[crate::device_control::catalog::DeviceStationDto],
    next_cursor: Option<&str>,
) {
    gui_catalog_begin(stations, next_cursor);
}

pub(crate) fn gui_set_search_results(
    stations: &[crate::device_control::catalog::DeviceStationDto],
) {
    let items = convert_stations(stations);
    let items_ptr = if items.is_empty() {
        core::ptr::null()
    } else {
        items.as_ptr()
    };
    unsafe { rock_gui_set_search_results(items_ptr, items.len()) };
}

pub(crate) fn gui_set_favourite_page(
    stations: &[crate::device_control::catalog::DeviceStationDto], total: usize, page: usize,
) {
    let items = convert_stations(stations);
    unsafe { rock_gui_set_favourite_page(items.as_ptr(), items.len(), total, page) };
}

pub(crate) fn gui_set_favourite_ids(ids: &[String], writable: bool) {
    let strings: Vec<_> = ids.iter().filter_map(|id| cstring(id).ok()).collect();
    let ptrs: Vec<_> = strings.iter().map(|s| s.as_ptr()).collect();
    unsafe { rock_gui_set_favourite_ids(ptrs.as_ptr(), ptrs.len(), writable) };
}

pub(crate) fn gui_favourites_visible() -> bool {
    unsafe { rock_gui_favourites_visible() }
}

pub(crate) fn gui_catalog_load_more_error() {
    unsafe { rock_gui_catalog_load_more_error() };
}

pub(crate) fn gui_catalog_search_busy(busy: bool) {
    unsafe { rock_gui_catalog_search_busy(busy) };
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
