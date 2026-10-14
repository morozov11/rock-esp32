//! Station metadata caching and HTTP catalog retrieval.

use crate::device_control;
use crate::firmware::pairing;
use crate::firmware::platform;
use crate::presentation::ui_strings as tr;
use std::collections::HashMap;
use std::sync::Mutex;

use super::session::Session;
use super::{message_id, send, HEARTBEAT_MS};

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
        Ok(mut page) => {
            device_control::catalog::tolerate_overflow_tags(&mut page.stations);
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
        Ok(mut page) => {
            device_control::catalog::tolerate_overflow_tags(&mut page.stations);
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

/// Typed catalog routing: `Refresh` reloads the first page, `LoadMore`
/// fetches the page after a keyset cursor, `Search` replaces the visible
/// list with a single query result page (search is never cursor-paginated).
pub(super) enum CatalogRequest {
    Refresh,
    LoadMore { cursor: String },
    Search { query: String },
}

/// Serial-log helper: cursors are opaque server tokens, so cap them to keep
/// a garbage value from flooding the serial line.
pub(super) fn cursor_for_log(cursor: &str) -> &str {
    match cursor.char_indices().nth(64) {
        Some((idx, _)) => &cursor[..idx],
        None => cursor,
    }
}

impl Session {
    pub(super) fn load_catalog(&mut self, request: CatalogRequest) {
        let (query, cursor, mode) = match &request {
            CatalogRequest::Refresh => (None, None, "refresh"),
            CatalogRequest::LoadMore { cursor } => (None, Some(cursor.as_str()), "load-more"),
            CatalogRequest::Search { query } => (Some(query.as_str()), None, "search"),
        };
        let Ok(path) = device_control::catalog::catalog_path(query, cursor) else {
            if matches!(request, CatalogRequest::Search { .. }) {
                platform::gui_catalog_search_busy(false);
            }
            platform::gui_show_error(tr::CATALOG_QUERY_INVALID);
            return;
        };
        platform::log_msg(&format!(
            "catalog fetch: mode={mode} cursor='{}'",
            cursor_for_log(cursor.unwrap_or(""))
        ));
        match &request {
            CatalogRequest::Refresh => {
                platform::gui_show_loading(tr::CATALOG_LOADING);
            }
            CatalogRequest::LoadMore { .. } => {
                // Inline loading: no fullscreen overlay, pill is already active.
            }
            CatalogRequest::Search { .. } => {
                // Inline busy indicator in search bar: no blocking overlay.
                platform::gui_catalog_search_busy(true);
            }
        }
        // The fetch below is synchronous in the session tick (10 s HTTP
        // timeout, plus one 401 retry with a fresh-token round trip), so
        // handle_inbound cannot read heartbeat ACKs while it blocks. Send a
        // heartbeat up front and reopen the watchdog window around it (the
        // same handoff pattern as on_cast_select).
        let _ = send(&device_control::heartbeat(
            &message_id(),
            &platform::now().unwrap_or_default(),
            self.heartbeat_sequence,
        ));
        self.heartbeat_sequence = self.heartbeat_sequence.wrapping_add(1);
        self.next_heartbeat = platform::uptime_ms() + HEARTBEAT_MS;
        self.last_heartbeat_ack = platform::uptime_ms();
        let mut response = platform::get(&path, Some(&self.token));
        if let Ok((status, _)) = &response {
            if device_control::catalog::should_retry_catalog_with_fresh_token(*status, false) {
                // The long-lived WebSocket session outlives the access token TTL;
                // obtain a fresh session token and retry the catalog request once.
                if let Ok(credentials) = pairing::credentials() {
                    self.token = credentials.access_token;
                    response = platform::get(&path, Some(&self.token));
                }
            }
        }
        if matches!(request, CatalogRequest::Search { .. }) {
            platform::gui_catalog_search_busy(false);
        }
        match response {
            Ok((200, bytes)) if query.is_some() => {
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
            }
            Ok((200, bytes)) => {
                match device_control::parse_catalog_page(&bytes) {
                    Ok(page) => {
                        cache_catalog_stations(&page.stations);
                        match &request {
                            CatalogRequest::LoadMore { .. } => {
                                platform::gui_catalog_append(
                                    &page.stations,
                                    page.next_cursor.as_deref(),
                                );
                            }
                            _ => {
                                platform::gui_catalog_begin(
                                    &page.stations,
                                    page.next_cursor.as_deref(),
                                );
                                if page.stations.is_empty() {
                                    platform::gui_show_empty(tr::CATALOG_EMPTY);
                                }
                            }
                        }
                        // Track the cursor of the last delivered page so
                        // on_catalog_browse can drop a queued LoadMore that
                        // re-sends it. Failed fetches keep the previous
                        // value, letting Retry re-fetch the same cursor.
                        self.catalog_last_cursor = match &request {
                            CatalogRequest::LoadMore { cursor } => Some(cursor.clone()),
                            // Refresh restarts pagination from page one.
                            _ => None,
                        };
                    }
                    Err(_) => {
                        log_catalog_page_reject(&bytes);
                        match &request {
                            CatalogRequest::LoadMore { .. } => {
                                platform::gui_catalog_load_more_error();
                            }
                            _ => {
                                platform::gui_show_error(tr::CATALOG_BAD_FORMAT);
                            }
                        }
                    }
                }
            }
            _ => match &request {
                CatalogRequest::LoadMore { .. } => {
                    platform::gui_catalog_load_more_error();
                }
                _ => {
                    platform::gui_show_error(tr::CATALOG_UNAVAILABLE);
                }
            },
        }
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

    #[test]
    fn search_request_parameters_and_no_cursor() {
        let req = CatalogRequest::Search {
            query: "rock".to_string(),
        };
        let (query, cursor, mode) = match &req {
            CatalogRequest::Refresh => (None, None, "refresh"),
            CatalogRequest::LoadMore { cursor } => (None, Some(cursor.as_str()), "load-more"),
            CatalogRequest::Search { query } => (Some(query.as_str()), None, "search"),
        };
        assert_eq!(query, Some("rock"));
        assert_eq!(cursor, None);
        assert_eq!(mode, "search");
    }

    #[test]
    fn load_more_request_parameters() {
        let req = CatalogRequest::LoadMore {
            cursor: "cursor-123".to_string(),
        };
        let (query, cursor, mode) = match &req {
            CatalogRequest::Refresh => (None, None, "refresh"),
            CatalogRequest::LoadMore { cursor } => (None, Some(cursor.as_str()), "load-more"),
            CatalogRequest::Search { query } => (Some(query.as_str()), None, "search"),
        };
        assert_eq!(query, None);
        assert_eq!(cursor, Some("cursor-123"));
        assert_eq!(mode, "load-more");
    }

    #[test]
    fn refresh_request_parameters() {
        let req = CatalogRequest::Refresh;
        let (query, cursor, mode) = match &req {
            CatalogRequest::Refresh => (None, None, "refresh"),
            CatalogRequest::LoadMore { cursor } => (None, Some(cursor.as_str()), "load-more"),
            CatalogRequest::Search { query } => (Some(query.as_str()), None, "search"),
        };
        assert_eq!(query, None);
        assert_eq!(cursor, None);
        assert_eq!(mode, "refresh");
    }

    #[test]
    fn presentation_empty_strings() {
        assert_eq!(tr::CATALOG_EMPTY, "Каталог пуст");
        assert_eq!(tr::SEARCH_EMPTY, "Ничего не найдено");
        assert_eq!(tr::CATALOG_LOADING, "Загрузка каталога...");
        assert_eq!(tr::SEARCH_LOADING, "Поиск станций...");
    }
}
