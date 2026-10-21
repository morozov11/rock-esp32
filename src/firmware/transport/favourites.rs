//! Account-owned favourites. The device keeps only a RAM snapshot.

use super::catalog::{cache_station_name, resolve_station_name};
use super::session::Session;
use super::{message_id, send, HEARTBEAT_MS};
use crate::device_control::catalog::DeviceStationDto;
use crate::device_control::favourites::{apply_response, display_name};
use crate::firmware::{pairing, platform};
use serde_json::{json, Value};

const SYNC_INTERVAL_MS: u64 = 60_000;
const RETRY_INTERVAL_MS: u64 = 30_000;
const PAGE_SIZE: usize = 20;
const WINDOW_STEP: usize = 10;

pub(super) enum FavouriteMeta {
    Found(DeviceStationDto),
    Missing,
    RetryAt(u64),
}

fn placeholder(id: &str, status: &str) -> DeviceStationDto {
    DeviceStationDto {
        id: id.to_string(),
        name: resolve_station_name(id),
        homepage_url: None,
        tags: vec![status.to_string()],
        language: None,
        country_code: None,
        codec: None,
        bitrate_kbps: None,
        health: "unknown".to_string(),
    }
}

impl Session {
    fn sync_favourites(&mut self, change: Option<Value>) -> Result<(), u16> {
        let mut body = json!({"since_revision": self.favourite_revision});
        if let Some(change) = change {
            body["favourites"] = change;
        }
        // HTTP blocks the session task; renew its heartbeat window around the request.
        let _ = send(&crate::device_control::heartbeat(
            &message_id(),
            &platform::now().unwrap_or_default(),
            self.heartbeat_sequence,
        ));
        self.heartbeat_sequence = self.heartbeat_sequence.wrapping_add(1);
        self.next_heartbeat = platform::uptime_ms() + HEARTBEAT_MS;
        self.last_heartbeat_ack = platform::uptime_ms();
        let mut response = platform::post_sync(&body, &self.token).map_err(|_| 0u16)?;
        if response.0 == 401 {
            if let Ok(token) = pairing::fresh_access_token() {
                self.token = token;
                response = platform::post_sync(&body, &self.token).map_err(|_| 0u16)?;
            }
        }
        self.last_heartbeat_ack = platform::uptime_ms();
        if response.0 != 200 {
            return Err(response.0);
        }
        let (revision, records) =
            apply_response(&self.favourites, self.favourite_revision, &response.1)
                .map_err(|_| 0u16)?;
        self.favourites = records;
        self.favourite_revision = revision;
        self.favourite_loaded = true;
        self.next_favourite_sync = platform::uptime_ms() + SYNC_INTERVAL_MS;
        self.publish_favourite_ids(true);
        Ok(())
    }

    fn publish_favourite_ids(&self, writable: bool) {
        let ids: Vec<String> = self
            .favourites
            .iter()
            .filter_map(|r| r.station_id.clone())
            .collect();
        platform::gui_set_favourite_ids(&ids, writable);
    }

    pub(super) fn tick_favourites(&mut self) {
        if platform::uptime_ms() < self.next_favourite_sync {
            return;
        }
        if let Err(status) = self.sync_favourites(None) {
            self.next_favourite_sync = platform::uptime_ms() + RETRY_INTERVAL_MS;
            self.publish_favourite_ids(false);
            platform::log_msg(&format!("favourites sync failed: status={status}"));
        } else if platform::gui_favourites_visible() {
            self.show_favourites(self.favourite_page);
        }
    }

    pub(super) fn show_favourites(&mut self, page: usize) {
        if !self.favourite_loaded || platform::uptime_ms() >= self.next_favourite_sync {
            if self.sync_favourites(None).is_err() {
                self.publish_favourite_ids(false);
                if !self.favourite_loaded {
                    platform::gui_show_offline("Избранное недоступно");
                    return;
                }
            }
        }
        self.favourite_page = page.min(self.favourites.len().saturating_sub(PAGE_SIZE).div_ceil(WINDOW_STEP));
        self.present_favourites();
    }

    fn present_favourites(&self) {
        let mut stations = Vec::new();
        let start = self.favourite_page.saturating_mul(WINDOW_STEP);
        let ids: Vec<String> = self
            .favourites
            .iter()
            .skip(start)
            .take(PAGE_SIZE)
            .filter_map(|record| record.station_id.clone())
            .collect();
        for id in ids {
            let station = match self.favourite_meta.get(&id) {
                Some(FavouriteMeta::Found(station)) => station.clone(),
                Some(FavouriteMeta::Missing) => placeholder(&id, "Недоступна в каталоге"),
                Some(FavouriteMeta::RetryAt(_)) => placeholder(&id, "Нет связи с каталогом"),
                None => placeholder(&id, "Загрузка..."),
            };
            stations.push(station);
        }
        platform::gui_set_favourite_page(&stations, self.favourites.len(), self.favourite_page);
    }

    pub(super) fn tick_favourite_metadata(&mut self) {
        let now = platform::uptime_ms();
        if now < self.next_favourite_meta || !platform::gui_favourites_visible() {
            return;
        }
        let next = self
            .favourites
            .iter()
            .skip(self.favourite_page.saturating_mul(WINDOW_STEP))
            .take(PAGE_SIZE)
            .filter_map(|r| r.station_id.as_deref())
            .find(|id| match self.favourite_meta.get(*id) {
                None => true,
                Some(FavouriteMeta::RetryAt(at)) => *at <= now,
                _ => false,
            })
            .map(str::to_string);
        let Some(id) = next else {
            return;
        };
        // One bounded GET per tick leaves the control socket responsive.
        let _ = send(&crate::device_control::heartbeat(
            &message_id(),
            &platform::now().unwrap_or_default(),
            self.heartbeat_sequence,
        ));
        self.heartbeat_sequence = self.heartbeat_sequence.wrapping_add(1);
        self.next_heartbeat = now + HEARTBEAT_MS;
        self.last_heartbeat_ack = now;
        let result = self.fetch_favourite_station(&id);
        self.last_heartbeat_ack = platform::uptime_ms();
        self.next_favourite_meta = platform::uptime_ms() + 500;
        self.favourite_meta.insert(
            id.clone(),
            match result {
                Ok(Some(station)) => FavouriteMeta::Found(station),
                Ok(None) => FavouriteMeta::Missing,
                Err(status) => {
                    platform::log_msg(&format!("favourite metadata failed id={id} status={status}"));
                    FavouriteMeta::RetryAt(platform::uptime_ms() + RETRY_INTERVAL_MS)
                }
            },
        );
        self.present_favourites();
    }

    fn fetch_favourite_station(&self, id: &str) -> Result<Option<DeviceStationDto>, u16> {
        let mut path = String::from("/api/v1/catalog/stations/");
        for byte in id.bytes() {
            if byte.is_ascii_alphanumeric() || b"-._~".contains(&byte) {
                path.push(byte as char);
            } else {
                path.push_str(&format!("%{byte:02X}"));
            }
        }
        match platform::get(&path, Some(&self.token)) {
            Ok((404, _)) => Ok(None),
            Ok((200, bytes)) => {
                let value = serde_json::from_slice::<Value>(&bytes).map_err(|_| 0u16)?;
                if value["id"].as_str() == Some(id) {
                    if let Some(name) = value["name"].as_str().and_then(display_name) {
                        cache_station_name(id, name);
                        return Ok(Some(DeviceStationDto {
                            id: id.to_string(),
                            name: name.to_string(),
                            homepage_url: None,
                            tags: value["tags"]
                                .as_array()
                                .map(|tags| {
                                    tags.iter()
                                        .filter_map(|t| t.as_str().map(str::to_string))
                                        .take(10)
                                        .collect()
                                })
                                .unwrap_or_default(),
                            language: None,
                            country_code: None,
                            codec: value["codec"].as_str().map(str::to_string),
                            bitrate_kbps: value["bitrate_kbps"]
                                .as_u64()
                                .and_then(|n| u32::try_from(n).ok()),
                            health: "unknown".to_string(),
                        }));
                    }
                }
                Err(0)
            }
            Ok((status, _)) => Err(status),
            Err(()) => Err(0),
        }
    }

    pub(super) fn toggle_favourite(&mut self, station_id: &str) {
        if station_id.is_empty() || station_id.len() > 128 {
            return;
        }
        if self.sync_favourites(None).is_err() {
            platform::gui_show_error("Избранное не синхронизировано");
            return;
        }
        let old = self
            .favourites
            .iter()
            .find(|r| r.station_id.as_deref() == Some(station_id));
        let deleting = old.is_some();
        if !deleting && self.favourites.len() >= 500 {
            platform::gui_show_error("Лимит избранного: 500 станций");
            return;
        }
        let Ok(now) = platform::now() else {
            platform::gui_show_error("Не удалось определить время");
            return;
        };
        let change = if let Some(record) = old {
            json!({"deletes": [{"record_id": record.record_id, "updated_at": now}]})
        } else {
            json!({"upserts": [{"record_id": message_id(), "station_id": station_id,
                                  "added_at": now, "updated_at": now}]})
        };
        match self.sync_favourites(Some(change)) {
            Ok(())
                if self
                    .favourites
                    .iter()
                    .any(|r| r.station_id.as_deref() == Some(station_id))
                    != deleting =>
            {
                if platform::gui_favourites_visible() {
                    self.show_favourites(self.favourite_page);
                }
            }
            Ok(()) => platform::gui_show_error("Изменение избранного отклонено сервером"),
            Err(429 | 503) => platform::gui_show_error("Сервер занят. Повторите позже"),
            Err(401 | 403) => platform::gui_show_error("Нужен вход в аккаунт"),
            Err(_) => platform::gui_show_error("Не удалось изменить избранное"),
        }
    }
}
