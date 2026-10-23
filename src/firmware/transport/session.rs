//! Session state and periodic tick orchestrator.

use super::media::{self, refresh_player_with_cast, SourceState};
use super::catalog::CatalogRequest;
use super::revision::{current_revision, next_state_revision};
use super::{message_id, send, HEARTBEAT_ACK_TIMEOUT_MS, HEARTBEAT_MS};
use crate::device_control::{self, DisplayState, OutputMode, PlayerState};
use crate::firmware::cast_client::CastClient;
use crate::firmware::pairing;
use crate::firmware::platform;
use crate::firmware::update_link_indicator;
use crate::firmware::GuiEventSink;
use crate::presentation::ui_strings as tr;
use core::ffi::c_char;
use std::collections::HashMap;

pub(super) struct Session {
    pub(super) token: String,
    pub(super) frame: Vec<c_char>,
    pub(super) device_id: String,
    pub(super) display: DisplayState,
    pub(super) player: PlayerState,
    pub(super) receiver_cache: device_control::ReceiverCache,
    pub(super) cast_client: CastClient,
    pub(super) output_mode: OutputMode,
    pub(super) last_stream: Option<(String, String)>,
    pub(super) source: SourceState,
    pub(super) pending: Option<(String, u64)>,
    /// Flag indicating that a pending station connection was cancelled by user
    /// stop. Invariant: only meaningful while connecting a station; reset
    /// whenever `pending` is set (new play) or cleared (command resolution,
    /// timeout, or consumed by `PlayStream`).
    pub(super) pending_cancel: bool,
    pub(super) retry: Option<platform::GuiEvent>,
    /// Cursor of the last successfully delivered browse page. A queued
    /// LoadMore re-sending that same cursor is a stale duplicate (no data
    /// update since) and is dropped in `on_catalog_browse`.
    pub(super) catalog_last_cursor: Option<String>,
    pub(super) favourites: Vec<crate::device_control::favourites::Favourite>,
    pub(super) favourite_loaded: bool,
    pub(super) favourite_revision: u64,
    pub(super) favourite_page: usize,
    pub(super) next_favourite_sync: u64,
    pub(super) favourite_meta: HashMap<String, super::favourites::FavouriteMeta>,
    pub(super) next_favourite_meta: u64,
    pub(super) network_check: u64,
    pub(super) next_heartbeat: u64,
    pub(super) next_token_refresh: u64,
    pub(super) last_heartbeat_ack: u64,
    pub(super) heartbeat_sequence: u64,
    /// GuiEvent handlers cannot return errors; a failed mandatory send
    /// marks the session dead instead (checked after each drain).
    pub(super) fatal: bool,
    pub(super) meta_title: String,
    pub(super) meta_generation: u32,
    voice_last_state: i32,
    voice_message_until: u64,
}

impl Session {
    pub(super) fn new(token: String, frame: Vec<c_char>) -> Result<Self, ()> {
        let mut s = Self {
            token,
            frame,
            device_id: String::new(),
            display: DisplayState::new(),
            player: PlayerState::new(),
            receiver_cache: device_control::ReceiverCache::new(),
            cast_client: CastClient::new(),
            output_mode: OutputMode::Local,
            last_stream: None,
            source: SourceState { station: None },
            pending: None,
            pending_cancel: false,
            retry: None,
            catalog_last_cursor: None,
            favourites: Vec::new(),
            favourite_loaded: false,
            favourite_revision: 0,
            favourite_page: 0,
            next_favourite_sync: 0,
            favourite_meta: HashMap::new(),
            next_favourite_meta: 0,
            network_check: 0,
            next_heartbeat: platform::uptime_ms() + HEARTBEAT_MS,
            next_token_refresh: platform::uptime_ms() + 10 * 60_000,
            last_heartbeat_ack: platform::uptime_ms(),
            heartbeat_sequence: 0,
            fatal: false,
            meta_title: String::new(),
            meta_generation: 0,
            voice_last_state: 0,
            voice_message_until: 0,
        };
        refresh_player_with_cast(&mut s.player, false, &s.meta_title);
        s.publish_state()?;
        platform::gui_start()?;
        update_link_indicator(true);
        s.update_gui();
        s.device_id = platform::identity_load().ok_or(())?.0;
        Ok(s)
    }

    pub(super) fn update_gui(&self) {
        media::update_gui(&self.player, &self.meta_title);
    }

    pub(super) fn publish_state(&self) -> Result<(), ()> {
        let rev = next_state_revision(&self.display, &self.player);
        send(&device_control::state_full(
            &message_id(),
            &platform::now()?,
            rev,
            &platform::now()?,
            device_control::full_state_snapshot_with_output(
                &self.player,
                &self.display,
                &self.output_mode,
            ),
        ))
    }

    pub(super) fn current_revision(&self) -> u64 {
        current_revision(&self.display, &self.player)
    }

    pub(super) fn tick_token_refresh(&mut self) {
        if platform::uptime_ms() >= self.next_token_refresh {
            if let Ok(fresh) = pairing::cached_access_token(60_000) {
                // The long-lived stream source reconnects with the stored
                // bearer; refresh it whenever the session token rotates.
                // A silent failure here desyncs the source token from the
                // session token until the next rotation — log it.
                if !platform::source_update_bearer(&fresh) {
                    platform::log_msg("Source bearer refresh failed on token rotation");
                }
                self.token = fresh;
                self.next_token_refresh = platform::uptime_ms() + 10 * 60_000;
            } else {
                self.next_token_refresh = platform::uptime_ms() + 60_000;
            }
        }
    }

    pub(super) fn check_heartbeat_watchdog(&self) -> Result<(), ()> {
        let ack_age = platform::uptime_ms().saturating_sub(self.last_heartbeat_ack);
        if ack_age > HEARTBEAT_ACK_TIMEOUT_MS {
            platform::ws_log_heartbeat_timeout(ack_age);
            return Err(());
        }
        Ok(())
    }

    pub(super) fn tick_heartbeat(&mut self) -> Result<(), ()> {
        if platform::uptime_ms() >= self.next_heartbeat {
            send(&device_control::heartbeat(
                &message_id(),
                &platform::now()?,
                self.heartbeat_sequence,
            ))?;
            self.heartbeat_sequence = self.heartbeat_sequence.wrapping_add(1);
            self.next_heartbeat = platform::uptime_ms() + HEARTBEAT_MS;
        }
        Ok(())
    }

    pub(super) fn drain_gui_events(&mut self) {
        while let Some(mut evt) = platform::gui_poll_event(0) {
            if matches!(evt, platform::GuiEvent::Retry) {
                let Some(previous) = self.retry.clone() else {
                    platform::gui_dismiss_state();
                    continue;
                };
                evt = previous;
            }
            self.dispatch_gui_event(evt);
            if self.fatal {
                return;
            }
        }
    }

    pub(super) fn poll_meta(&mut self) -> bool {
        if !platform::source_is_active() {
            if !self.meta_title.is_empty() {
                self.meta_title.clear();
                self.meta_generation = 0;
                return true;
            }
            return false;
        }
        let generation = platform::source_meta_generation();
        if generation != self.meta_generation {
            let (new_gen, title) = platform::source_get_meta_title();
            self.meta_generation = new_gen;
            if self.meta_title != title {
                self.meta_title = title;
                return true;
            }
        }
        false
    }

    pub(super) fn tick_player_refresh(&mut self) -> Result<(), ()> {
        let meta_changed = self.poll_meta();
        let player_changed = refresh_player_with_cast(
            &mut self.player,
            self.cast_client.is_connected(),
            &self.meta_title,
        );
        if meta_changed && !player_changed {
            self.update_gui();
        }
        if player_changed {
            self.publish_state()?;
            // A background stream death is a LOCAL UI state (rockcast plan
            // RE-6): the always-visible ticker and the Now Playing status
            // render it inline. No full-screen overlay here — it used to
            // pop over Browse/Search and read as "catalog is broken".
        }
        Ok(())
    }

    pub(super) fn tick_cast_pump(&mut self) -> Result<(), ()> {
        if self.cast_client.is_connected() && self.cast_client.pump().is_err() {
            if self.cast_client.reconnect().is_ok() {
                platform::cast_log("Cast connection restored after idle disconnect");
            } else {
                self.cast_client.disconnect();
                self.output_mode = OutputMode::Local;
                platform::gui_update_cast_status(false, None);
                platform::gui_show_error(tr::CAST_LINK_LOST);
                // Deterministic fallback: the shared source keeps running,
                // the speaker re-joins the ring without an upstream reconnect.
                if let Some((st_id, _)) = &self.last_stream {
                    if media::play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                        platform::log_msg("Local playback fallback failed after cast link lost");
                    }
                }
            }
            self.publish_state()?;
        }
        Ok(())
    }

    pub(super) fn tick_pending_deadline(&mut self) {
        if self
            .pending
            .as_ref()
            .is_some_and(|(_, deadline)| platform::uptime_ms() >= *deadline)
        {
            self.pending = None;
            self.pending_cancel = false;
            self.update_gui();
            platform::gui_show_error(tr::STATION_NO_RESPONSE);
        }
    }

    pub(super) fn tick_network_check(&mut self) {
        if platform::uptime_ms() >= self.network_check {
            update_link_indicator(true);
            self.network_check = platform::uptime_ms() + 15000;
        }
    }

    pub(super) fn tick_voice(&mut self) {
        let state = platform::voice_state();
        if state != self.voice_last_state {
            platform::gui_voice_state(state);
            platform::log_msg(&format!("voice state={state}"));
            self.voice_last_state = state;
            self.voice_message_until = if state >= 4 { platform::uptime_ms() + 5000 } else { 0 };
            if state == 4 {
                if let Some(transcript) = platform::voice_take_transcript() {
                    let query = device_control::catalog::clean_voice_query(&transcript);
                    self.load_catalog(CatalogRequest::Search { query });
                }
            }
        } else if self.voice_message_until != 0 && platform::uptime_ms() >= self.voice_message_until {
            platform::gui_voice_state(0);
            self.voice_message_until = 0;
        }
    }
}
