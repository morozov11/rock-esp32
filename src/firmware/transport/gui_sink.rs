//! GUI event handling and user action dispatch (`GuiEventSink` implementation for `Session`).

use super::catalog::{cursor_for_log, CatalogRequest};
use super::media::{cast_play_stream, play_local_station};
use super::session::Session;
use super::{message_id, send, HEARTBEAT_MS};
use crate::device_control::{self, OutputMode};
use crate::firmware::pairing;
use crate::firmware::platform;
use crate::firmware::GuiEventSink;
use crate::presentation::ui_strings as tr;

impl GuiEventSink for Session {
    fn on_voice_toggle(&mut self) {
        if (1..=3).contains(&platform::voice_state()) {
            // Other device requests can rotate the token and revoke the one
            // held by this long-lived control WSS without closing that WSS.
            if let Ok(fresh) = pairing::fresh_access_token() {
                if !platform::source_update_bearer(&fresh) {
                    platform::log_msg("Source bearer refresh failed before voice");
                }
                self.token = fresh;
            } else {
                platform::log_msg("Voice token refresh failed; trying current token");
            }
            let _ = platform::voice_start(&self.token);
        }
    }
    fn on_favourites_show(&mut self) { self.show_favourites(0); }

    fn on_favourites_page(&mut self, page: u16) { self.show_favourites(page as usize); }

    fn on_favourite_toggle(&mut self, station: String) { self.toggle_favourite(&station); }

    fn on_play_station(&mut self, station: String) {
        platform::voice_release_pause();
        if station.is_empty() || station.len() > 128 {
            return;
        }
        if self.source.station.as_deref() != Some(station.as_str())
            || !platform::source_is_active()
        {
            self.meta_title.clear();
            self.meta_generation = 0;
            platform::source_clear_meta_title();
        }
        let command = message_id();
        let Ok(now) = platform::now() else {
            self.fatal = true;
            return;
        };
        // Do not pre-apply PlayStream: the resolved server command must
        // reach hardware.
        if send(&device_control::station_play_station(
            &message_id(),
            &now,
            &command,
            &self.device_id,
            &station,
        ))
        .is_err()
        {
            self.fatal = true;
            return;
        }
        self.pending = Some((command, platform::uptime_ms() + 30_000));
        self.pending_cancel = false;
        self.retry = Some(platform::GuiEvent::PlayStation(station.clone()));
        platform::gui_set_starting_station(&station);
        self.update_gui();
    }

    fn on_playback_play(&mut self) {
        platform::voice_release_pause();
        if self.pending.is_some() || self.pending_cancel {
            platform::log_msg("Playback play ignored: station connecting");
            return;
        }
        if self.cast_client.is_connected() {
            if let Some((st_id, _)) = &self.last_stream {
                self.last_heartbeat_ack = platform::uptime_ms();
                if cast_play_stream(
                    &mut self.cast_client,
                    st_id,
                    &self.meta_title,
                    &mut self.token,
                    &mut self.source,
                )
                .is_ok()
                {
                    self.player.set_playing();
                    self.update_gui();
                } else {
                    platform::log_msg("Cast resume failed; playback stays paused");
                }
                self.last_heartbeat_ack = platform::uptime_ms();
            }
        } else {
            self.last_heartbeat_ack = platform::uptime_ms();
            let res = self.local_play();
            self.last_heartbeat_ack = platform::uptime_ms();
            if res.is_err() {
                platform::log_msg("Player resume failed");
            }
        }
    }

    fn on_playback_pause(&mut self) {
        platform::voice_release_pause();
        if self.pending.is_some() || self.pending_cancel {
            platform::log_msg("Playback pause ignored: station connecting");
            return;
        }
        if self.cast_client.is_connected() {
            // Media-level STOP keeps the Cast control session alive so a
            // later resume can LOAD again; the full receiver teardown lives
            // in on_cast_disconnect. Best-effort send (see server path).
            if self.cast_client.stop_media().is_err() {
                platform::log_msg("Cast media stop failed on pause");
            }
            self.player.set_paused();
            self.update_gui();
        } else if platform::player_pause().is_err() {
            platform::log_msg("Player pause failed");
        }
    }

    fn on_playback_stop(&mut self) {
        platform::voice_release_pause();
        self.meta_title.clear();
        self.meta_generation = 0;
        platform::source_clear_meta_title();
        if self.pending.is_some() {
            self.pending_cancel = true;
            self.pending = None;
            self.retry = None;
            platform::log_msg("Station connecting cancelled by user stop");
        } else {
            self.pending = None;
            self.pending_cancel = false;
        }
        if self.cast_client.is_connected() {
            // Same session-preserving teardown as the server path: stop the
            // media, keep the Cast connection for stop→play.
            if self.cast_client.stop_media().is_err() {
                platform::log_msg("Cast media stop failed on stop");
            }
            self.player.set_stopped();
            self.update_gui();
        } else {
            self.player.set_stopped();
            self.update_gui();
        }
        if platform::player_stop().is_err() {
            platform::log_msg("Player stop failed");
        }
        // Full stop tears the shared source down with it.
        platform::source_stop();
        self.source.station = None;
    }

    fn on_volume_change(&mut self, delta: i16) {
        let volume = (self.player.volume_level() as i32 + delta as i32).clamp(0, 100) as u8;
        // Cast volume is a best-effort mirror of the local volume state.
        if self.cast_client.is_connected() {
            let _ = self.cast_client.set_volume(volume);
        }
        crate::firmware::apply_user_volume(volume);
    }

    fn on_catalog_browse(&mut self, cursor: String) {
        if cursor.is_empty() {
            // C today always sends an empty cursor (e.g. the "Каталог"
            // button): empty means first-page refresh, which is what every
            // existing browse event keeps doing.
            self.retry = Some(platform::GuiEvent::CatalogBrowse(cursor));
            self.load_catalog(CatalogRequest::Refresh);
            return;
        }
        // The fetch runs synchronously in the session tick, so by the time
        // the next queued event drains, a LoadMore re-sending the cursor of
        // the last delivered page is a stale duplicate of a request that
        // already completed — fast scrolling can queue several.
        if self.catalog_last_cursor.as_deref() == Some(cursor.as_str()) {
            platform::log_msg(&format!(
                "catalog load-more dropped as duplicate: cursor='{}'",
                cursor_for_log(&cursor)
            ));
            return;
        }
        self.retry = Some(platform::GuiEvent::CatalogBrowse(cursor.clone()));
        self.load_catalog(CatalogRequest::LoadMore { cursor });
    }

    fn on_catalog_search(&mut self, query: String) {
        self.retry = Some(platform::GuiEvent::CatalogSearch(query.clone()));
        self.load_catalog(CatalogRequest::Search { query });
    }

    fn on_retry(&mut self) {
        // Retry events are re-injected into their remembered action in
        // drain_gui_events before dispatch; a bare Retry never reaches here.
    }

    fn on_cast_browse(&mut self) {
        let found = platform::cast_discover(2500);
        let active_list = self.receiver_cache.replace(found, platform::uptime_ms() / 1000);
        platform::gui_set_cast_devices(&active_list, self.cast_client.receiver_id());
        platform::gui_show_cast_menu(false);
    }

    fn on_cast_select(&mut self, rec_id: String) {
        let now_sec = platform::uptime_ms() / 1000;
        if let Some(entry) = self.receiver_cache.get_entry(&rec_id, now_sec) {
            platform::gui_show_cast_menu(true);
            let local_stopped = self.last_stream.is_none()
                || platform::player_stop_and_wait(12_000).is_ok();
            if !local_stopped {
                platform::gui_dismiss_cast_menu();
                platform::gui_show_error(tr::CAST_PLAYER_BUSY);
            } else if self
                .cast_client
                .connect(&entry.receiver_id, &entry.device)
                .is_ok()
            {
                self.output_mode = OutputMode::Chromecast {
                    receiver_id: entry.receiver_id.clone(),
                    receiver_name: entry.display_name.clone(),
                };
                platform::gui_update_cast_status(true, Some(&entry.display_name));
                platform::gui_flush_events();
                platform::gui_dismiss_cast_menu();

                if let Some((st_id, _)) = &self.last_stream {
                    // Send a heartbeat before the potentially slow Cast LOAD
                    // so the server-side watchdog does not fire mid-handoff.
                    // A failed timestamp here only skews one heartbeat.
                    let _ = send(&device_control::heartbeat(
                        &message_id(),
                        &platform::now().unwrap_or_default(),
                        self.heartbeat_sequence,
                    ));
                    self.heartbeat_sequence = self.heartbeat_sequence.wrapping_add(1);
                    self.next_heartbeat = platform::uptime_ms() + HEARTBEAT_MS;
                    // ACKs are only read by handle_inbound, which cannot run
                    // while this handler blocks for up to ~70 s — reset the
                    // watchdog window on both sides of the handoff.
                    self.last_heartbeat_ack = platform::uptime_ms();
                    if cast_play_stream(
                        &mut self.cast_client,
                        st_id,
                        &self.meta_title,
                        &mut self.token,
                        &mut self.source,
                    )
                    .is_ok()
                    {
                        self.player.apply_play_stream(st_id);
                        self.player.set_playing();
                        self.update_gui();
                    } else {
                        // LOAD failed after a successful connect: roll the
                        // output back to the speaker and resume local playback
                        // instead of a dead Cast mode (mirror of the
                        // connect-failure path below).
                        platform::cast_log("Cast LOAD failed after connect; falling back to local");
                        self.cast_client.disconnect();
                        self.output_mode = OutputMode::Local;
                        platform::gui_update_cast_status(false, None);
                        platform::gui_show_error(tr::CAST_START_FAILED);
                        if play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                            platform::log_msg("Local fallback failed after cast load failure");
                        }
                    }
                    self.last_heartbeat_ack = platform::uptime_ms();
                } else {
                    // Connected with nothing queued: catalog selections route
                    // to the active Cast output, so point the user there.
                    platform::gui_show_empty(tr::CAST_PICK_STATION);
                }
                if self.publish_state().is_err() {
                    self.fatal = true;
                }
            } else {
                platform::gui_dismiss_cast_menu();
                platform::gui_show_error(tr::CAST_CONNECT_FAILED);
                if let Some((st_id, _)) = &self.last_stream {
                    self.last_heartbeat_ack = platform::uptime_ms();
                    if play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                        platform::log_msg("Local playback fallback failed after cast connect failed");
                    }
                    self.last_heartbeat_ack = platform::uptime_ms();
                }
            }
        } else {
            // Expired cache entry (60 s TTL): the menu already closed itself
            // on tap, so rescan and reopen with the fresh list — the menu's
            // own empty-list status covers a found-nothing outcome.
            platform::cast_log("Cast selection cache expired; rescanning");
            platform::gui_show_cast_menu(true);
            let found = platform::cast_discover(2500);
            let active_list = self.receiver_cache.replace(found, platform::uptime_ms() / 1000);
            platform::gui_set_cast_devices(&active_list, self.cast_client.receiver_id());
        }
    }

    fn on_cast_disconnect(&mut self) {
        if self.cast_client.is_connected() {
            self.cast_client.disconnect();
        }
        self.output_mode = OutputMode::Local;
        platform::gui_update_cast_status(false, None);
        platform::gui_flush_events();
        platform::gui_dismiss_cast_menu();

        // The shared source keeps running: the speaker re-joins the same
        // ring, no upstream reconnect, no gap.
        if let Some((st_id, _)) = &self.last_stream {
            self.last_heartbeat_ack = platform::uptime_ms();
            if play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                platform::log_msg("Local playback fallback failed after cast disconnect");
            }
            self.last_heartbeat_ack = platform::uptime_ms();
        }
        self.update_gui();
        if self.publish_state().is_err() {
            self.fatal = true;
        }
    }
}
