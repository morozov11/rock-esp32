//! Bounded Rust-to-LVGL presentation adapter.

use crate::device_control::DisplayView;
use core::ffi::c_char;
use std::ffi::CString;

/// User-facing strings surfaced by the session transport (phase 4): the
/// transport decides WHICH state to show, the wording lives here next to the
/// rest of the presentation layer. Kept verbatim from the former inline
/// literals — the GUI texts are part of the product contract.
pub mod ui_strings {
    // Catalog / search
    pub const CATALOG_QUERY_INVALID: &str = "Запрос должен содержать от 1 до 128 символов";
    pub const SEARCH_LOADING: &str = "Поиск станций...";
    pub const CATALOG_LOADING: &str = "Загрузка каталога...";
    pub const SEARCH_EMPTY: &str = "Ничего не найдено";
    pub const SEARCH_BAD_FORMAT: &str = "Некорректный формат поиска";
    pub const CATALOG_BAD_FORMAT: &str = "Некорректный формат каталога";
    pub const CATALOG_UNAVAILABLE: &str = "Не удалось получить станции. Повторите запрос.";
    // Playback
    pub const PLAYBACK_ERROR: &str = "Ошибка воспроизведения аудио";
    pub const STATION_NO_RESPONSE: &str = "Станция не ответила. Повторите запуск.";
    pub const STATION_REJECTED: &str = "Сервер отклонил запуск станции";
    pub const PLAYER_COMMAND_FAILED: &str = "Не удалось выполнить команду плеера";
    // Cast / output switching
    pub const CAST_PLAYER_BUSY: &str = "Не удалось освободить аудиоплеер";
    pub const CAST_CONNECT_FAILED: &str = "Не удалось подключиться к Chromecast";
    pub const CAST_LINK_LOST: &str = "Связь с Chromecast потеряна";
    pub const SERVER_LINK_LOST: &str = "Связь с сервером потеряна";
}

unsafe extern "C" {
    fn rock_ui_show_text(text: *const c_char) -> bool;
    fn rock_ui_show_now_playing(
        station_id: *const c_char,
        title: *const c_char,
        subtitle: *const c_char,
    ) -> bool;
    fn rock_ui_show_sensor_grid(title: *const c_char, items: *const c_char) -> bool;
}

fn cstring(value: &str) -> Option<CString> {
    CString::new(value).ok()
}

/// Renders a previously validated view.  This is the only LVGL presentation
/// boundary available to the protocol/transport layer.
pub fn render(view: &DisplayView) -> bool {
    match view {
        DisplayView::Text(text) => {
            cstring(text).is_some_and(|text| unsafe { rock_ui_show_text(text.as_ptr()) })
        }
        DisplayView::NowPlaying {
            station_id,
            title,
            subtitle,
        } => match (cstring(station_id), cstring(title), cstring(subtitle)) {
            (Some(station_id), Some(title), Some(subtitle)) => unsafe {
                rock_ui_show_now_playing(station_id.as_ptr(), title.as_ptr(), subtitle.as_ptr())
            },
            _ => false,
        },
        DisplayView::SensorGrid { title, items } => {
            match (cstring(title), cstring(&items.join("\n"))) {
                (Some(title), Some(items)) => unsafe {
                    rock_ui_show_sensor_grid(title.as_ptr(), items.as_ptr())
                },
                _ => false,
            }
        }
    }
}
