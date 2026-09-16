use serde::{Deserialize, Serialize};

pub const MAX_CATALOG_PAGE_STATIONS: usize = 20;
pub const MAX_STATION_ID_LEN: usize = 128;
pub const MAX_STATION_NAME_LEN: usize = 128;
pub const MAX_STATION_TAGS: usize = 10;
pub const MAX_TAG_LEN: usize = 64;
pub const MAX_SEARCH_QUERY_LEN: usize = 128;
pub const MAX_CATALOG_BYTES: usize = 65_536;

/// Percent-encode UTF-8 query values, never treating user text as URL syntax.
pub fn catalog_path(query: Option<&str>) -> Result<String, CatalogError> {
    match query {
        None => Ok("/api/v1/device-control/catalog/stations?limit=20".into()),
        Some(q) => {
            let q = q.trim();
            if q.is_empty()
                || q.chars().count() > MAX_SEARCH_QUERY_LEN
                || q.chars().any(char::is_control)
            {
                return Err(CatalogError::Malformed);
            }
            let mut path = String::from("/api/v1/device-control/catalog/search?limit=20&q=");
            use std::fmt::Write;
            for b in q.bytes() {
                if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
                    path.push(b as char);
                } else {
                    write!(path, "%{b:02X}").unwrap();
                }
            }
            Ok(path)
        }
    }
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

/// Parses and validates one bounded catalog browse page from RS-2.
/// Deterministically rejects pages exceeding MAX_CATALOG_PAGE_STATIONS (<= 20)
/// or containing malformed station metadata.
pub fn parse_catalog_page(bytes: &[u8]) -> Result<DeviceCatalogPageDto, CatalogError> {
    if bytes.len() > MAX_CATALOG_BYTES {
        return Err(CatalogError::Oversized);
    }
    let page: DeviceCatalogPageDto =
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
    for station in &page.stations {
        if !station.is_valid() {
            return Err(CatalogError::Malformed);
        }
    }
    Ok(page)
}

/// Parses and validates one bounded catalog search page from RS-2.
pub fn parse_search_page(bytes: &[u8]) -> Result<DeviceSearchPageDto, CatalogError> {
    if bytes.len() > MAX_CATALOG_BYTES {
        return Err(CatalogError::Oversized);
    }
    let page: DeviceSearchPageDto =
        serde_json::from_slice(bytes).map_err(|_| CatalogError::Malformed)?;
    if page.stations.len() > MAX_CATALOG_PAGE_STATIONS {
        return Err(CatalogError::Oversized);
    }
    if page.request_id.is_empty() || !valid_text(&page.request_id, 64) {
        return Err(CatalogError::Malformed);
    }
    for station in &page.stations {
        if !station.is_valid() {
            return Err(CatalogError::Malformed);
        }
    }
    Ok(page)
}
