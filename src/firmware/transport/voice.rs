//! Voice stream results: ranked search pages with autoplay of the first
//! station and intent echoes of executed commands.

use super::catalog::{cache_catalog_stations, CatalogRequest};
use super::session::Session;
use crate::device_control;
use crate::device_control::catalog::DeviceStationDto;
use crate::firmware::platform;
use crate::firmware::GuiEventSink;
use crate::presentation::ui_strings as tr;

impl Session {
    pub(super) fn on_voice_result(&mut self, frame: &str) {
        let Ok(value) = serde_json::from_str::<serde_json::Value>(frame) else {
            platform::log_msg("voice result: malformed json");
            return;
        };
        let intent = value.get("intent").and_then(|v| v.as_str()).unwrap_or("");
        let status = value.get("status").and_then(|v| v.as_str()).unwrap_or("");
        if !status.is_empty() {
            // Executed by the server through the same pipeline as buttons;
            // the resulting command frames update playback state elsewhere.
            platform::log_msg(&format!("voice command intent={intent} status={status}"));
            return;
        }
        if intent.is_empty() {
            // Legacy transcript-only result (pre-device-branch server):
            // keep searching the catalog from the transcript ourselves.
            if let Some(transcript) = value.get("transcript").and_then(|v| v.as_str()) {
                let query = device_control::catalog::clean_voice_query(transcript);
                self.load_catalog(CatalogRequest::Search { query });
            }
            return;
        }
        if intent != "play_radio" {
            platform::log_msg(&format!("voice result: unexpected intent={intent}"));
            return;
        }
        let Ok(mut stations) = serde_json::from_value::<Vec<DeviceStationDto>>(
            value.get("stations").cloned().unwrap_or_default(),
        ) else {
            platform::log_msg("voice result: stations shape rejected");
            return;
        };
        device_control::catalog::tolerate_overflow_tags(&mut stations);
        stations.retain(|s| s.is_valid());
        if stations.is_empty() {
            platform::gui_show_empty(tr::SEARCH_EMPTY);
            return;
        }
        cache_catalog_stations(&stations);
        platform::gui_set_search_results(&stations);
        // RockMobile semantics: the search itself starts the first hit, the
        // same path as pressing the first catalog row.
        let selected = serde_json::from_value::<DeviceStationDto>(
            value.get("selected_station").cloned().unwrap_or_default(),
        )
        .ok()
        .filter(|s| s.is_valid())
        .or_else(|| stations.first().cloned());
        if let Some(station) = selected {
            platform::log_msg(&format!(
                "voice search autoplay id='{}' results={}",
                station.id,
                stations.len()
            ));
            self.on_play_station(station.id);
        }
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    #[test]
    fn command_status_has_priority_over_intent() {
        let frame = json!({"type":"result","status":"succeeded","intent":"stop"}).to_string();
        let value: serde_json::Value = serde_json::from_str(&frame).unwrap();
        let intent = value.get("intent").and_then(|v| v.as_str()).unwrap_or("");
        let status = value.get("status").and_then(|v| v.as_str()).unwrap_or("");
        assert_eq!((intent, status), ("stop", "succeeded"));
    }

    #[test]
    fn search_frame_fields_decode() {
        let frame = json!({
            "type":"result","intent":"play_radio","transcript":"рок",
            "selected_station":{"id":"st-1","name":"Rock Radio"},
            "stations":[{"id":"st-1","name":"Rock Radio"}]
        })
        .to_string();
        let value: serde_json::Value = serde_json::from_str(&frame).unwrap();
        let stations: Vec<DeviceStationDto> =
            serde_json::from_value(value.get("stations").cloned().unwrap()).unwrap();
        assert_eq!(stations.len(), 1);
        assert_eq!(stations[0].id, "st-1");
        let selected: DeviceStationDto = serde_json::from_value(
            value.get("selected_station").cloned().unwrap(),
        )
        .unwrap();
        assert_eq!(selected.id, "st-1");
    }
}
