//! Bounded Rust-to-LVGL presentation adapter.

use crate::device_control::DisplayView;
use core::ffi::c_char;
use std::ffi::CString;

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
