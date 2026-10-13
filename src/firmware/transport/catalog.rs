//! Station metadata caching and HTTP catalog retrieval.

use crate::device_control;
use crate::firmware::pairing;
use crate::firmware::platform;
use crate::presentation::ui_strings as tr;
use std::collections::HashMap;
use std::sync::Mutex;

static CATALOG_CACHE: Mutex<Option<HashMap<String, String>>> = Mutex::new(None);

/// Station names come from the server only: catalog/search pages and the
/// PlayStream command populate the cache; unknown ids fall back to the id.
pub(crate) fn resolve_station_name(station_id: &str) -> String {
    let cache = CATALOG_CACHE.lock().unwrap();
    cache
        .as_ref()
        .and_then(|map| map.get(station_id).cloned())
        .unwrap_or_else(|| station_id.to_string())
}

pub(crate) fn cache_catalog_stations(stations: &[device_control::catalog::DeviceStationDto]) {
    let mut cache = CATALOG_CACHE.lock().unwrap();
    let map = cache.get_or_insert_with(HashMap::new);
    for s in stations {
        map.insert(s.id.clone(), s.name.clone());
    }
}

pub(crate) fn cache_station_name(station_id: &str, station_name: &str) {
    let mut cache = CATALOG_CACHE.lock().unwrap();
    let map = cache.get_or_insert_with(HashMap::new);
    map.insert(station_id.to_string(), station_name.to_string());
}

/// The server answers 200 with well-formed JSON, so a rejected page is only
/// observable here: log whether the JSON shape failed or which station
/// violated the RS-2 field contract before the error overlay covers it.
fn log_search_page_reject(bytes: &[u8]) {
    match serde_json::from_slice::<device_control::catalog::DeviceSearchPageDto>(bytes) {
        Ok(page) => {
            let mut bad = 0;
            for (i, s) in page.stations.iter().enumerate() {
                if !s.is_valid() {
                    bad += 1;
                    platform::log_msg(&format!(
                        "search reject: station[{i}] id='{}' name_len={} tags={} codec={:?} bitrate={:?} health='{}'",
                        s.id, s.name.len(), s.tags.len(), s.codec, s.bitrate_kbps, s.health
                    ));
                }
            }
            if bad == 0 {
                platform::log_msg(&format!(
                    "search reject: page-level (request_id_len={} stations={})",
                    page.request_id.len(),
                    page.stations.len()
                ));
            }
        }
        Err(e) => platform::log_msg(&format!("search reject: shape: {e}")),
    }
}

fn log_catalog_page_reject(bytes: &[u8]) {
    match serde_json::from_slice::<device_control::catalog::DeviceCatalogPageDto>(bytes) {
        Ok(page) => {
            let mut bad = 0;
            for (i, s) in page.stations.iter().enumerate() {
                if !s.is_valid() {
                    bad += 1;
                    platform::log_msg(&format!(
                        "catalog reject: station[{i}] id='{}' name_len={} tags={} codec={:?} bitrate={:?} health='{}'",
                        s.id, s.name.len(), s.tags.len(), s.codec, s.bitrate_kbps, s.health
                    ));
                }
            }
            if bad == 0 {
                platform::log_msg(&format!(
                    "catalog reject: page-level (request_id_len={} stations={} cursor_len={})",
                    page.request_id.len(),
                    page.stations.len(),
                    page.next_cursor.as_deref().map_or(0, str::len)
                ));
            }
        }
        Err(e) => platform::log_msg(&format!("catalog reject: shape: {e}")),
    }
}

pub(super) fn load_catalog(token: &mut String, query: Option<&str>) {
    let Ok(path) = device_control::catalog::catalog_path(query) else {
        platform::gui_show_error(tr::CATALOG_QUERY_INVALID);
        return;
    };
    platform::gui_show_loading(if query.is_some() {
        tr::SEARCH_LOADING
    } else {
        tr::CATALOG_LOADING
    });
    let mut response = platform::get(&path, Some(token));
    if let Ok((status, _)) = &response {
        if device_control::catalog::should_retry_catalog_with_fresh_token(*status, false) {
            // The long-lived WebSocket session outlives the access token TTL;
            // obtain a fresh session token and retry the catalog request once.
            if let Ok(credentials) = pairing::credentials() {
                *token = credentials.access_token;
                response = platform::get(&path, Some(token));
            }
        }
    }
    match response {
        Ok((200, bytes)) => {
            if query.is_some() {
                match device_control::parse_search_page(&bytes) {
                    Ok(page) => {
                        cache_catalog_stations(&page.stations);
                        platform::gui_set_search_results(&page.stations);
                        if page.stations.is_empty() {
                            platform::gui_show_empty(tr::SEARCH_EMPTY);
                        }
                    }
                    Err(_) => {
                        log_search_page_reject(&bytes);
                        platform::gui_show_error(tr::SEARCH_BAD_FORMAT);
                    }
                }
            } else {
                match device_control::parse_catalog_page(&bytes) {
                    Ok(page) => {
                        cache_catalog_stations(&page.stations);
                        platform::gui_set_browse_stations(
                            &page.stations,
                            page.next_cursor.as_deref(),
                        )
                    }
                    Err(_) => {
                        log_catalog_page_reject(&bytes);
                        platform::gui_show_error(tr::CATALOG_BAD_FORMAT);
                    }
                }
            }
        }
        _ => platform::gui_show_error(tr::CATALOG_UNAVAILABLE),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cache_and_resolve_station_name() {
        cache_station_name("test-st-1", "My Test Radio");
        assert_eq!(resolve_station_name("test-st-1"), "My Test Radio");
        assert_eq!(resolve_station_name("unknown-station"), "unknown-station");
    }
}
