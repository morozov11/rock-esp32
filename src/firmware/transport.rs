//! Bounded WebSocket session and player/display command dispatch.
//!
//! Phase-4 layout: the former ~475-line session() is decomposed into a
//! `Session` owning the loop state, with one tick method per concern
//! (token refresh, heartbeat watchdog, GUI events via the shared
//! GuiEventSink dispatcher, player refresh, cast pump, inbound commands).
//! UX strings live in crate::presentation::ui_strings, not here.

use super::platform;
use super::update_link_indicator;
use super::GuiEventSink;
use crate::device_control::{
    self, DisplayState, Envelope, PlaybackAction, PlaybackStatus, PlayerCommand, PlayerState,
    VolumeAction,
};
use crate::presentation::ui_strings as tr;
use core::ffi::c_char;
use serde_json::Value;

// Single source (phase 5 A3): generated from main/rock_protocol_abi.h.
const HEARTBEAT_MS: u64 = crate::abi::ROCK_ABI_HEARTBEAT_MS;
const HEARTBEAT_ACK_TIMEOUT_MS: u64 = crate::abi::ROCK_ABI_HEARTBEAT_ACK_TIMEOUT_MS;

const FRAME_CAPACITY: usize = device_control::MAX_JSON_FRAME_BYTES + 1;

pub(crate) fn run(token: &str) -> Result<(), ()> {
    platform::ws_start(token)?;
    let result = session(token);
    platform::ws_stop();
    platform::relay_stop();
    update_link_indicator(false);
    platform::gui_mark_catalog_stale();
    result
}

fn send(value: &Value) -> Result<(), ()> {
    let text = serde_json::to_string(value).map_err(|_| ())?;
    if text.len() > device_control::MAX_JSON_FRAME_BYTES {
        return Err(());
    }
    platform::ws_send(&text)
}

fn message_id() -> String {
    let mut b = [0u8; 16];
    platform::random_fill(&mut b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
        b[14], b[15]
    )
}

// State revision counter for the device.state-full protocol contract.
//
// Invariant: single Rust task. Only the session task (and its synchronous
// GuiEvent handlers, which run inside the same task's drain_gui_events)
// ever writes this counter; the offline loop between sessions only reads
// freshly-reset state. There is no second concurrent writer, so the two
// atomic ops below cannot interleave — they exist to keep every step of
// the read-modify-write atomic (no torn load+store window if a reader is
// ever added) rather than to synchronize two writers.
static STATE_REVISION_COUNTER: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

fn next_state_revision(display: &DisplayState, player: &PlayerState) -> u64 {
    let base = (display.revision() + player.revision() - 1) as u32;
    // fetch_max (not fetch_add): the revision must stay >= base after a
    // display/player revision jump forward, while remaining strictly greater
    // than any previously published value — fetch_add alone could re-issue
    // an already published revision while the counter lags behind base.
    let prev = STATE_REVISION_COUNTER.fetch_max(base, core::sync::atomic::Ordering::Relaxed);
    let rev = if prev == 0 { base } else { base.max(prev + 1) };
    STATE_REVISION_COUNTER.fetch_max(rev, core::sync::atomic::Ordering::Relaxed);
    rev as u64
}

fn current_revision(display: &DisplayState, player: &PlayerState) -> u64 {
    let cur = STATE_REVISION_COUNTER.load(core::sync::atomic::Ordering::Relaxed);
    if cur == 0 {
        display.revision() + player.revision() - 1
    } else {
        cur as u64
    }
}

use std::collections::HashMap;
use std::sync::Mutex;

static CATALOG_CACHE: Mutex<Option<HashMap<String, String>>> = Mutex::new(None);

pub(crate) fn resolve_station_name(station_id: &str) -> String {
    let mut cache = CATALOG_CACHE.lock().unwrap();
    let map = cache.get_or_insert_with(|| {
        let mut m = HashMap::new();
        m.insert("st-01".into(), "Radio Maximum".into());
        m.insert("st-02".into(), "Наше Радио".into());
        m.insert("st-03".into(), "Rock FM 95.2".into());
        m.insert("st-04".into(), "Radio Ultra".into());
        m.insert("st-05".into(), "Radio Record".into());
        m
    });
    map.get(station_id)
        .cloned()
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

/// Synchronize only observed hardware state, including after an offline interval.
pub(crate) fn refresh_player(player: &mut PlayerState) -> bool {
    let (_, title) = platform::source_get_meta_title();
    refresh_player_with_cast(player, false, &title)
}

pub(crate) fn refresh_player_with_cast(
    player: &mut PlayerState,
    is_casting: bool,
    meta_title: &str,
) -> bool {
    let Some((status, station, volume, muted)) = platform::player_get_full_state() else {
        return false;
    };
    let before = player.revision();
    if !is_casting {
        if !station.is_empty() && player.station_id() != Some(station.as_str()) {
            player.apply_play_stream(&station);
        }
        match status {
            PlaybackStatus::Buffering => {
                player.apply_play_stream(&station);
            }
            PlaybackStatus::Playing => {
                player.set_playing();
            }
            PlaybackStatus::Paused => {
                player.set_paused();
            }
            PlaybackStatus::Stopped => {
                player.set_stopped();
            }
            PlaybackStatus::Error => {
                player.set_error();
            }
            PlaybackStatus::Idle => {}
        }
    }
    player.apply_volume(VolumeAction::SetLevel(volume));
    player.apply_volume(VolumeAction::SetMute(muted));
    if player.revision() != before {
        update_gui(player, meta_title);
        return true;
    }
    false
}

fn update_gui(player: &PlayerState, meta_title: &str) {
    let code = match player.status() {
        PlaybackStatus::Idle => 0,
        PlaybackStatus::Buffering => 1,
        PlaybackStatus::Playing => 2,
        PlaybackStatus::Paused => 3,
        PlaybackStatus::Stopped => 4,
        PlaybackStatus::Error => 5,
    };
    let station_id = player.station_id().unwrap_or("");
    let station_name = if !station_id.is_empty() {
        resolve_station_name(station_id)
    } else {
        String::new()
    };
    platform::gui_update_playback(
        code,
        station_id,
        &station_name,
        meta_title,
        "",
        player.volume_level(),
        player.volume_muted(),
    );
}

fn load_catalog(token: &mut String, query: Option<&str>) {
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
            if let Ok(credentials) = super::pairing::credentials() {
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
                    Err(_) => platform::gui_show_error(tr::SEARCH_BAD_FORMAT),
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
                    Err(_) => platform::gui_show_error(tr::CATALOG_BAD_FORMAT),
                }
            }
        }
        _ => platform::gui_show_error(tr::CATALOG_UNAVAILABLE),
    }
}

// Unified stream source: the board keeps ONE authorized connection to the
// RockServer station proxy; the speaker and the Cast relay consume the same
// ring, so switching outputs never reopens the upstream.
struct SourceState {
    station: Option<String>,
}

fn station_proxy_url(station_id: &str) -> Result<String, ()> {
    let path = device_control::station_stream_path(station_id).map_err(|_| ())?;
    Ok(format!("{}{}", platform::server_base_url(), path))
}

// Idempotent per station: when the source already serves this station, keep
// it (and its ring) alive for gapless output switching.
fn ensure_source(
    station_id: &str,
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    if source.station.as_deref() == Some(station_id) && platform::source_is_active() {
        // Swallowed-error policy (phase 5): a failed bearer refresh would
        // leave reconnects on the stale token — log it, keep the session.
        if !platform::source_update_bearer(token) {
            platform::log_msg("Source bearer refresh failed on active source");
        }
        return Ok(());
    }
    platform::source_stop();
    *token = super::pairing::fresh_access_token()?;
    let url = station_proxy_url(station_id)?;
    // Confidentiality: the proxy URL and bearer never appear in logs.
    if !platform::source_start(&url, token) {
        source.station = None;
        return Err(());
    }
    source.station = Some(station_id.to_string());
    Ok(())
}

fn play_local_station(
    station_id: &str,
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    ensure_source(station_id, token, source)?;
    let url = station_proxy_url(station_id)?;
    platform::player_play_stream(&url, station_id, token)
}

fn cast_play_stream(
    cast_client: &mut super::cast_client::CastClient,
    station_id: &str,
    meta_title: &str,
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    // Detach the speaker tap only: the shared source keeps running. A failed
    // detach would leave local audio playing over the Cast handoff — log it.
    if platform::player_stop().is_err() {
        platform::log_msg("Speaker detach failed before Cast playback");
    }
    ensure_source(station_id, token, source)?;
    let relay_url = platform::relay_listen().ok_or(())?;
    let content_type = platform::source_content_type_enum();
    let resolved_station = resolve_station_name(station_id);
    let title = if !meta_title.is_empty() {
        meta_title
    } else {
        &resolved_station
    };
    let res = cast_client.load_stream(&relay_url, title, content_type);
    if res.is_err() {
        return Err(());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Session: one WSS session's loop state, decomposed into tick methods.
// ---------------------------------------------------------------------------

struct Session {
    token: String,
    frame: Vec<c_char>,
    device_id: String,
    display: DisplayState,
    player: PlayerState,
    receiver_cache: device_control::ReceiverCache,
    cast_client: super::cast_client::CastClient,
    output_mode: device_control::OutputMode,
    last_stream: Option<(String, String)>,
    source: SourceState,
    pending: Option<(String, u64)>,
    retry: Option<platform::GuiEvent>,
    network_check: u64,
    next_heartbeat: u64,
    next_token_refresh: u64,
    last_heartbeat_ack: u64,
    heartbeat_sequence: u64,
    /// GuiEvent handlers cannot return errors; a failed mandatory send
    /// marks the session dead instead (checked after each drain).
    fatal: bool,
    meta_title: String,
    meta_generation: u32,
}

fn session(token: &str) -> Result<(), ()> {
    let token = token.to_string();
    send(&device_control::hello(&message_id(), &platform::now()?))?;
    let mut frame = vec![0 as c_char; FRAME_CAPACITY];
    let len = platform::ws_receive(&mut frame, 30_000);
    if len <= 0
        || device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
        })
        .map_err(|_| ())?
        .kind
            != "protocol.welcome"
    {
        return Err(());
    }
    send(&device_control::registration(
        &message_id(),
        &platform::now()?,
        env!("CARGO_PKG_VERSION"),
    ))?;

    let mut s = Session::new(token, frame)?;
    loop {
        s.tick_token_refresh();
        s.check_heartbeat_watchdog()?;
        s.tick_heartbeat()?;
        s.drain_gui_events();
        if s.fatal {
            return Err(());
        }
        s.tick_player_refresh()?;
        s.tick_cast_pump()?;
        s.tick_pending_deadline();
        s.tick_network_check();

        let len = platform::ws_receive(&mut s.frame, 50);
        if len < 0 {
            return Err(());
        }
        if len == 0 {
            continue;
        }
        s.handle_inbound(len)?;
    }
}

impl Session {
    fn new(token: String, frame: Vec<c_char>) -> Result<Self, ()> {
        let mut s = Self {
            token,
            frame,
            device_id: String::new(),
            display: DisplayState::new(),
            player: PlayerState::new(),
            receiver_cache: device_control::ReceiverCache::new(),
            cast_client: super::cast_client::CastClient::new(),
            output_mode: device_control::OutputMode::Local,
            last_stream: None,
            source: SourceState { station: None },
            pending: None,
            retry: None,
            network_check: 0,
            next_heartbeat: platform::uptime_ms() + HEARTBEAT_MS,
            next_token_refresh: platform::uptime_ms() + 10 * 60_000,
            last_heartbeat_ack: platform::uptime_ms(),
            heartbeat_sequence: 0,
            fatal: false,
            meta_title: String::new(),
            meta_generation: 0,
        };
        refresh_player_with_cast(&mut s.player, false, &s.meta_title);
        s.publish_state()?;
        platform::gui_start()?;
        update_link_indicator(true);
        s.update_gui();
        s.device_id = platform::identity_load().ok_or(())?.0;
        Ok(s)
    }

    fn update_gui(&self) {
        update_gui(&self.player, &self.meta_title);
    }

    fn publish_state(&self) -> Result<(), ()> {
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

    fn current_revision(&self) -> u64 {
        current_revision(&self.display, &self.player)
    }

    // ------------------------------------------------------------- ticks ---

    fn tick_token_refresh(&mut self) {
        if platform::uptime_ms() >= self.next_token_refresh {
            if let Ok(fresh) = super::pairing::fresh_access_token() {
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

    fn check_heartbeat_watchdog(&self) -> Result<(), ()> {
        let ack_age = platform::uptime_ms().saturating_sub(self.last_heartbeat_ack);
        if ack_age > HEARTBEAT_ACK_TIMEOUT_MS {
            platform::ws_log_heartbeat_timeout(ack_age);
            return Err(());
        }
        Ok(())
    }

    fn tick_heartbeat(&mut self) -> Result<(), ()> {
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

    fn drain_gui_events(&mut self) {
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

    fn poll_meta(&mut self) -> bool {
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

    fn tick_player_refresh(&mut self) -> Result<(), ()> {
        let player_changed = refresh_player_with_cast(
            &mut self.player,
            self.cast_client.is_connected(),
            &self.meta_title,
        );
        let meta_changed = self.poll_meta();
        if meta_changed && !player_changed {
            self.update_gui();
        }
        if player_changed {
            self.publish_state()?;
            if self.player.status() == PlaybackStatus::Error {
                platform::gui_show_error(tr::PLAYBACK_ERROR);
            }
        }
        Ok(())
    }

    fn tick_cast_pump(&mut self) -> Result<(), ()> {
        if self.cast_client.is_connected() && self.cast_client.pump().is_err() {
            if self.cast_client.reconnect().is_ok() {
                platform::cast_log("Cast connection restored after idle disconnect");
            } else {
                self.cast_client.disconnect();
                self.output_mode = device_control::OutputMode::Local;
                platform::gui_update_cast_status(false, None);
                platform::gui_show_error(tr::CAST_LINK_LOST);
                // Deterministic fallback: the shared source keeps running,
                // the speaker re-joins the ring without an upstream reconnect.
                if let Some((st_id, _)) = &self.last_stream {
                    if play_local_station(st_id, &mut self.token, &mut self.source).is_err() {
                        platform::log_msg("Local playback fallback failed after cast link lost");
                    }
                }
            }
            self.publish_state()?;
        }
        Ok(())
    }

    fn tick_pending_deadline(&mut self) {
        if self
            .pending
            .as_ref()
            .is_some_and(|(_, deadline)| platform::uptime_ms() >= *deadline)
        {
            self.pending = None;
            self.update_gui();
            platform::gui_show_error(tr::STATION_NO_RESPONSE);
        }
    }

    fn tick_network_check(&mut self) {
        if platform::uptime_ms() >= self.network_check {
            update_link_indicator(true);
            self.network_check = platform::uptime_ms() + 15000;
        }
    }

    // ---------------------------------------------------- inbound frames ---

    fn handle_inbound(&mut self, len: i32) -> Result<(), ()> {
        let Ok(inbound) = device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(self.frame.as_ptr() as *const u8, len as usize)
        }) else {
            return Ok(());
        };
        // Observability only: log the parsed envelope kind, never the frame
        // content (device.command carries stream_uri with a token).
        platform::ws_log_rx_frame(&inbound.kind);
        if inbound.kind == "device.heartbeat_ack" {
            self.last_heartbeat_ack = platform::uptime_ms();
            return Ok(());
        }
        if inbound.kind == "device.resync_requested" {
            return self.publish_state();
        }
        if inbound.kind == "command.result" || inbound.kind == "protocol.error" {
            let outcome = device_control::command_result_outcome(&inbound);
            if let Some((id, _)) = &self.pending {
                let matches =
                    matches!(outcome, Some((command_id, _)) if command_id == id.as_str())
                        || inbound.kind == "protocol.error";
                if matches {
                    let failed = !matches!(outcome, Some((_, true)));
                    self.pending = None;
                    self.update_gui();
                    if failed {
                        platform::gui_show_error(tr::STATION_REJECTED);
                    }
                }
            }
            return Ok(());
        }
        if inbound.kind != "device.command" {
            return Ok(());
        }
        let command_id = inbound.payload.get("command_id").and_then(Value::as_str);
        let body_name = inbound
            .payload
            .get("body")
            .and_then(|b| b.get("name"))
            .and_then(Value::as_str);
        if body_name == Some("display.set_view") {
            return self.exec_display_command(&inbound, command_id);
        }
        let command = match device_control::player_command(&inbound) {
            Ok(c) => c,
            Err(code) => {
                send(&device_control::error_response(
                    &message_id(),
                    &platform::now()?,
                    &inbound.message_id,
                    command_id,
                    code,
                ))?;
                return Ok(());
            }
        };
        let id = command.command_id().to_string();
        send(&device_control::command_accepted(
            &message_id(),
            &platform::now()?,
            &id,
        ))?;
        let result = self.exec_player_command(command);
        refresh_player_with_cast(&mut self.player, self.cast_client.is_connected(), &self.meta_title);
        if self.pending.as_ref().is_some_and(|(pending_id, _)| pending_id == &id) {
            self.pending = None;
            self.update_gui();
        }
        match result {
            Ok(()) => {
                self.publish_state()?;
                send(&device_control::command_succeeded(
                    &message_id(),
                    &platform::now()?,
                    &id,
                    self.current_revision(),
                ))?;
            }
            Err(()) => {
                platform::gui_show_error(tr::PLAYER_COMMAND_FAILED);
                send(&device_control::command_failed(
                    &message_id(),
                    &platform::now()?,
                    &id,
                    "playback_failed",
                    "Player operation failed",
                ))?;
            }
        }
        Ok(())
    }

    fn exec_display_command(
        &mut self,
        inbound: &Envelope,
        command_id: Option<&str>,
    ) -> Result<(), ()> {
        let command = match device_control::display_command(inbound) {
            Ok(c) => c,
            Err(code) => {
                send(&device_control::error_response(
                    &message_id(),
                    &platform::now()?,
                    &inbound.message_id,
                    command_id,
                    code,
                ))?;
                return Ok(());
            }
        };
        send(&device_control::command_accepted(
            &message_id(),
            &platform::now()?,
            &command.command_id,
        ))?;
        if self.display.apply(command.view.clone()) {
            if !crate::presentation::render(&command.view) {
                return Err(());
            }
            self.publish_state()?;
        }
        send(&device_control::command_succeeded(
            &message_id(),
            &platform::now()?,
            &command.command_id,
            self.current_revision(),
        ))?;
        Ok(())
    }

    /// Local-speaker play policy shared by the GUI and server paths: PAUSED
    /// resumes in place, but a full stop (or an rx crash) tore the shared
    /// source down with the player, so STOPPED/IDLE/ERROR can only move
    /// forward by re-opening the last station.
    fn local_play(&mut self) -> Result<(), ()> {
        match platform::player_status() {
            PlaybackStatus::Stopped | PlaybackStatus::Idle | PlaybackStatus::Error => {
                match self.last_stream.clone() {
                    Some((st_id, _)) => {
                        play_local_station(&st_id, &mut self.token, &mut self.source)
                    }
                    None => Ok(()),
                }
            }
            PlaybackStatus::Paused => platform::player_play(),
            // BUFFERING reaches PLAYING on its own; PLAYING needs nothing.
            PlaybackStatus::Buffering | PlaybackStatus::Playing => Ok(()),
        }
    }

    fn exec_player_command(&mut self, command: PlayerCommand) -> Result<(), ()> {
        match command {
            PlayerCommand::PlayStream {
                station_id,
                station_name,
                stream_uri,
                ..
            } => {
                if let Some(ref name) = station_name {
                    cache_station_name(&station_id, name);
                }
                self.meta_title.clear();
                self.meta_generation = 0;
                platform::source_clear_meta_title();
                self.last_stream = Some((station_id.clone(), stream_uri));
                if self.cast_client.is_connected() {
                    // Best-effort detach: cast_play_stream detaches again and
                    // logs a failure there.
                    let _ = platform::player_stop();
                    let res = cast_play_stream(
                        &mut self.cast_client,
                        &station_id,
                        &self.meta_title,
                        &mut self.token,
                        &mut self.source,
                    );
                    if res.is_ok() {
                        self.player.apply_play_stream(&station_id);
                        self.player.set_playing();
                        self.update_gui();
                        Ok(())
                    } else {
                        platform::cast_log(
                            "Cast play_stream failed; remaining in Cast mode without starting local audio",
                        );
                        self.player.set_stopped();
                        self.update_gui();
                        Err(())
                    }
                } else if self.player.station_id() == Some(station_id.as_str())
                    && matches!(
                        self.player.status(),
                        PlaybackStatus::Playing | PlaybackStatus::Buffering
                    )
                {
                    Ok(())
                } else {
                    play_local_station(&station_id, &mut self.token, &mut self.source)
                }
            }
            PlayerCommand::Playback { action, .. } => {
                if self.cast_client.is_connected() {
                    match action {
                        PlaybackAction::Play => {
                            if let Some((st_id, _)) = &self.last_stream {
                                if cast_play_stream(
                                    &mut self.cast_client,
                                    st_id,
                                    &self.meta_title,
                                    &mut self.token,
                                    &mut self.source,
                                )
                                .is_err()
                                {
                                    platform::log_msg("Cast resume failed; playback stays paused");
                                }
                            }
                            self.player.set_playing();
                            self.update_gui();
                            Ok(())
                        }
                        PlaybackAction::Pause => {
                            if self.cast_client.stop_media().is_err() {
                                platform::log_msg("Cast media stop failed on pause");
                            }
                            self.player.set_paused();
                            self.update_gui();
                            Ok(())
                        }
                        PlaybackAction::Stop => {
                            if self.cast_client.stop_media().is_err() {
                                platform::log_msg("Cast media stop failed on stop");
                            }
                            self.meta_title.clear();
                            self.meta_generation = 0;
                            platform::source_clear_meta_title();
                            self.player.set_stopped();
                            self.update_gui();
                            // Full stop tears the shared source down with it.
                            platform::source_stop();
                            self.source.station = None;
                            Ok(())
                        }
                    }
                } else {
                    match action {
                        PlaybackAction::Play => self.local_play(),
                        PlaybackAction::Pause => platform::player_pause(),
                        PlaybackAction::Stop => {
                            self.meta_title.clear();
                            self.meta_generation = 0;
                            platform::source_clear_meta_title();
                            self.player.set_stopped();
                            self.update_gui();
                            let res = platform::player_stop();
                            platform::source_stop();
                            self.source.station = None;
                            res
                        }
                    }
                }
            }
            PlayerCommand::Volume { action, .. } => match action {
                VolumeAction::SetLevel(level) => {
                    // Cast volume is a best-effort mirror: the local player is
                    // the authority and is re-synced by refresh_player ticks.
                    if self.cast_client.is_connected() {
                        let _ = self.cast_client.set_volume(level);
                    }
                    if platform::player_set_volume(level).is_err() {
                        platform::log_msg("Player volume set failed");
                    }
                    Ok(())
                }
                VolumeAction::Change(delta) => {
                    let new_vol =
                        (self.player.volume_level() as i32 + delta).clamp(0, 100) as u8;
                    if self.cast_client.is_connected() {
                        let _ = self.cast_client.set_volume(new_vol);
                    }
                    if platform::player_set_volume(new_vol).is_err() {
                        platform::log_msg("Player volume set failed");
                    }
                    Ok(())
                }
                VolumeAction::SetMute(muted) => platform::player_set_mute(muted),
            },
        }
    }
}

// ------------------------------------------------------ GUI event policy ---

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
                self.output_mode = device_control::OutputMode::Chromecast {
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
                        self.output_mode = device_control::OutputMode::Local;
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
        self.output_mode = device_control::OutputMode::Local;
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
