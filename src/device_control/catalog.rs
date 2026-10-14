use serde::{Deserialize, Serialize};

pub const MAX_CATALOG_PAGE_STATIONS: usize = 20;
pub const MAX_STATION_ID_LEN: usize = 128;
pub const MAX_STATION_NAME_LEN: usize = 128;
pub const MAX_STATION_TAGS: usize = 10;
pub const MAX_TAG_LEN: usize = 64;
pub const MAX_SEARCH_QUERY_LEN: usize = 128;
pub const MAX_CATALOG_BYTES: usize = 65_536;

pub const MAX_CATALOG_CURSOR_LEN: usize = 512;

fn percent_encode_into(path: &mut String, value: &str) {
    use std::fmt::Write;
    for b in value.bytes() {
        if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
            path.push(b as char);
        } else {
            write!(path, "%{b:02X}").unwrap();
        }
    }
}

/// Percent-encode UTF-8 query values, never treating user text as URL syntax.
///
/// The cursor is an opaque keyset token from the server's `next_cursor`
/// (browse only; search is never paginated by cursor).
pub fn catalog_path(
    query: Option<&str>,
    cursor: Option<&str>,
) -> Result<String, CatalogError> {
    match query {
        Some(_) if cursor.is_some() => return Err(CatalogError::Malformed),
        Some(q) => {
            let q = q.trim();
            if q.is_empty()
                || q.chars().count() > MAX_SEARCH_QUERY_LEN
                || q.chars().any(char::is_control)
            {
                return Err(CatalogError::Malformed);
            }
            let mut path = String::from("/api/v1/device-control/catalog/search?limit=20&q=");
            percent_encode_into(&mut path, q);
            Ok(path)
        }
        None => {
            let mut path = String::from("/api/v1/device-control/catalog/stations?limit=20");
            if let Some(c) = cursor.filter(|c| !c.is_empty()) {
                if c.len() > MAX_CATALOG_CURSOR_LEN || c.chars().any(char::is_control) {
                    return Err(CatalogError::Malformed);
                }
                path.push_str("&cursor=");
                percent_encode_into(&mut path, c);
            }
            Ok(path)
        }
    }
}

/// Browser and native players use the same authenticated RockServer relay.
pub fn station_stream_path(station_id: &str) -> Result<String, CatalogError> {
    if station_id.is_empty() || station_id.len() > MAX_STATION_ID_LEN || station_id == "." || station_id == ".." || station_id.chars().any(char::is_control) {
        return Err(CatalogError::Malformed);
    }
    let mut path = String::from("/api/v1/stations/");
    use std::fmt::Write;
    for b in station_id.bytes() {
        if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
            path.push(b as char);
        } else {
            write!(path, "%{b:02X}").unwrap();
        }
    }
    path.push_str("/stream");
    Ok(path)
}

fn valid_text(s: &str, max: usize) -> bool {
    s.len() <= max && !s.chars().any(char::is_control)
}

/// Metadata for a single radio station returned by RockServer device-facing catalog.
///
/// NOTE: By RS-1/RS-2 contract, DeviceStationDto NEVER exposes `stream_url`.
/// Extra or unknown fields in JSON are safely tolerated.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceStationDto {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub homepage_url: Option<String>,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub language: Option<String>,
    #[serde(default)]
    pub country_code: Option<String>,
    #[serde(default)]
    pub codec: Option<String>,
    #[serde(default)]
    pub bitrate_kbps: Option<u32>,
    pub health: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceCatalogPageDto {
    pub request_id: String,
    pub stations: Vec<DeviceStationDto>,
    #[serde(default)]
    pub next_cursor: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceSearchPageDto {
    pub request_id: String,
    pub stations: Vec<DeviceStationDto>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum CatalogError {
    Malformed,
    Oversized,
}

impl DeviceStationDto {
    pub fn is_valid(&self) -> bool {
        if self.id.is_empty() || !valid_text(&self.id, MAX_STATION_ID_LEN) {
            return false;
        }
        if self.name.is_empty() || !valid_text(&self.name, MAX_STATION_NAME_LEN) {
            return false;
        }
        if self.tags.len() > MAX_STATION_TAGS {
            return false;
        }
        for tag in &self.tags {
            if !valid_text(tag, MAX_TAG_LEN) {
                return false;
            }
        }
        if let Some(ref hp) = self.homepage_url {
            if !valid_text(hp, 512) {
                return false;
            }
        }
        if self.codec.as_ref().is_some_and(|s| !valid_text(s, 32))
            || self.language.as_ref().is_some_and(|s| !valid_text(s, 128))
            || self
                .country_code
                .as_ref()
                .is_some_and(|s| !valid_text(s, 8))
        {
            return false;
        }
        if self.bitrate_kbps == Some(0) {
            return false;
        }
        matches!(self.health.as_str(), "healthy" | "degraded" | "unknown")
    }
}

/// RS-2 over-tag tolerance: the browser-side catalog has no tag cap, and a
/// single over-tagged station must not fail the whole page (2026-10-13: a
/// "black" search returned stations with 11 and 22 tags and the device showed
/// "Некорректный формат поиска"). Extra or over-long tags are dropped like
/// unknown JSON fields; everything else stays strictly validated.
fn tolerate_overflow_tags(stations: &mut [DeviceStationDto]) {
    for station in stations.iter_mut() {
        station.tags.truncate(MAX_STATION_TAGS);
        station.tags.retain(|tag| valid_text(tag, MAX_TAG_LEN));
    }
}

/// Parses and validates one bounded catalog browse page from RS-2.
/// Deterministically rejects pages exceeding MAX_CATALOG_PAGE_STATIONS (<= 20)
/// or containing malformed station metadata; overflow tags are truncated
/// (see `tolerate_overflow_tags`).
pub fn parse_catalog_page(bytes: &[u8]) -> Result<DeviceCatalogPageDto, CatalogError> {
    if bytes.len() > MAX_CATALOG_BYTES {
        return Err(CatalogError::Oversized);
    }
    let mut page: DeviceCatalogPageDto =
        serde_json::from_slice(bytes).map_err(|_| CatalogError::Malformed)?;
    if page.stations.len() > MAX_CATALOG_PAGE_STATIONS {
        return Err(CatalogError::Oversized);
    }
    if page.request_id.is_empty() || !valid_text(&page.request_id, 64) {
        return Err(CatalogError::Malformed);
    }
    if let Some(ref cursor) = page.next_cursor {
        if cursor.is_empty() || !valid_text(cursor, 512) {
            return Err(CatalogError::Malformed);
        }
    }
    tolerate_overflow_tags(&mut page.stations);
    for station in &page.stations {
        if !station.is_valid() {
            return Err(CatalogError::Malformed);
        }
    }
    Ok(page)
}

/// Parses and validates one bounded catalog search page from RS-2.
/// Overflow tags are truncated (see `tolerate_overflow_tags`).
pub fn parse_search_page(bytes: &[u8]) -> Result<DeviceSearchPageDto, CatalogError> {
    if bytes.len() > MAX_CATALOG_BYTES {
        return Err(CatalogError::Oversized);
    }
    let mut page: DeviceSearchPageDto =
        serde_json::from_slice(bytes).map_err(|_| CatalogError::Malformed)?;
    if page.stations.len() > MAX_CATALOG_PAGE_STATIONS {
        return Err(CatalogError::Oversized);
    }
    if page.request_id.is_empty() || !valid_text(&page.request_id, 64) {
        return Err(CatalogError::Malformed);
    }
    tolerate_overflow_tags(&mut page.stations);
    for station in &page.stations {
        if !station.is_valid() {
            return Err(CatalogError::Malformed);
        }
    }
    Ok(page)
}

/// A long-lived WebSocket session outlives the HTTP access token TTL, so a
/// catalog request can be rejected with 401 mid-session. Retry exactly once
/// after obtaining a fresh session token; every other status is terminal.
pub fn should_retry_catalog_with_fresh_token(status: u16, already_retried: bool) -> bool {
    status == 401 && !already_retried
}
