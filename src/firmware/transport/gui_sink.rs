//! GUI event handling and user action dispatch (`GuiEventSink` implementation for `Session`).

use super::catalog::load_catalog;
use super::media::{cast_play_stream, play_local_station};
use super::session::Session;
use super::{message_id, send, HEARTBEAT_MS};
use crate::device_control::{self, OutputMode};
use crate::firmware::platform;
use crate::firmware::GuiEventSink;
use crate::presentation::ui_strings as tr;

impl GuiEventSink for Session {
    fn on_play_station(&mut self, station: String) {
        if station.is_empty() || station.len() > 128 {
            return;
        }
        self.meta_title.clear();
        self.meta_generation = 0;
        platform::source_clear_meta_title();
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
        self.retry = Some(platform::GuiEvent::PlayStation(station.clone()));
        platform::gui_set_starting_station(&station);
        self.update_gui();
    }

    fn on_playback_play(&mut self) {
        if self.cast_client.is_connected() {
            if let Some((st_id, _)) = &self.last_stream {
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
            }
        } else if self.local_play().is_err() {
            platform::log_msg("Player resume failed");
        }
    }

    fn on_playback_pause(&mut self) {
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
        self.meta_title.clear();
        self.meta_generation = 0;
        platform::source_clear_meta_title();
        self.pending = None;
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
        if platform::player_set_volume(volume).is_err() {
            platform::log_msg("Player volume set failed");
        }
    }

    fn on_catalog_browse(&mut self) {
        self.retry = Some(platform::GuiEvent::CatalogBrowse);
        load_catalog(&mut self.token, None);
    }

    fn on_catalog_search(&mut self, query: String) {
        self.retry = Some(platform::GuiEvent::CatalogSearch(query.clone()));
        load_catalog(&mut self.token, Some(&query));
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
                    if play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                        platform::log_msg("Local playback fallback failed after cast connect failed");
                    }
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
        platform::gui_dismiss_cast_menu();

        // The shared source keeps running: the speaker re-joins the same
        // ring, no upstream reconnect, no gap.
        if let Some((st_id, _)) = &self.last_stream {
            if play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                platform::log_msg("Local playback fallback failed after cast disconnect");
            }
        }
        self.update_gui();
        if self.publish_state().is_err() {
            self.fatal = true;
        }
    }
}
