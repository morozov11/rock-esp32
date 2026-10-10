//! Firmware composition root: initializes ESP-IDF then owns reconnect policy.

mod cast_client;
mod pairing;
/// pub(crate): presentation.rs reuses the single `cstring` helper instead of
/// keeping its own CString conversion copy (phase 5).
pub(crate) mod platform;
mod transport;

/// Link indicator policy (phase 4, moved out of the FFI layer): "Wi-Fi up"
/// is derived here from the raw RSSI sentinel (-128 = no association), and
/// "server up" is meaningless without a Wi-Fi link.
pub(crate) fn update_link_indicator(server_up: bool) {
    let rssi = platform::wifi_rssi();
    let wifi_up = rssi != -128;
    platform::gui_update_connectivity(rssi, wifi_up, server_up && wifi_up);
}

/// The one exhaustive GuiEvent match (phase 4): the offline loop below and
/// the session loop in transport.rs both route their events through
/// `dispatch_gui_event`; each side implements its own policy per variant.
pub(crate) trait GuiEventSink {
    fn on_play_station(&mut self, station: String);
    fn on_playback_play(&mut self);
    fn on_playback_pause(&mut self);
    fn on_playback_stop(&mut self);
    fn on_volume_change(&mut self, delta: i16);
    fn on_catalog_browse(&mut self);
    fn on_catalog_search(&mut self, query: String);
    fn on_retry(&mut self);
    fn on_cast_browse(&mut self);
    fn on_cast_select(&mut self, receiver_id: String);
    fn on_cast_disconnect(&mut self);

    fn dispatch_gui_event(&mut self, evt: platform::GuiEvent) {
        use platform::GuiEvent;
        match evt {
            GuiEvent::PlayStation(station) => self.on_play_station(station),
            GuiEvent::PlaybackPlay => self.on_playback_play(),
            GuiEvent::PlaybackPause => self.on_playback_pause(),
            GuiEvent::PlaybackStop => self.on_playback_stop(),
            GuiEvent::VolumeChange(delta) => self.on_volume_change(delta),
            GuiEvent::CatalogBrowse => self.on_catalog_browse(),
            GuiEvent::CatalogSearch(query) => self.on_catalog_search(query),
            GuiEvent::Retry => self.on_retry(),
            GuiEvent::CastBrowse => self.on_cast_browse(),
            GuiEvent::CastSelect(receiver_id) => self.on_cast_select(receiver_id),
            GuiEvent::CastDisconnect => self.on_cast_disconnect(),
        }
    }
}

/// Offline backoff policy: transport controls act on the local player,
/// anything needing the server shows the offline overlay, Retry aborts the
/// backoff so the session retries immediately.
///
/// Swallowed-error policy (phase 5): every `let _ =` below is deliberately
/// best-effort. There is no session to report to, the GUI shows the offline
/// overlay already, and the online session re-syncs real hardware state via
/// refresh_player once the connection is back — so logging these offline
/// failures would only add noise.
struct OfflineHandler {
    exit_backoff: bool,
}

impl GuiEventSink for OfflineHandler {
    fn on_play_station(&mut self, _station: String) {
        platform::gui_show_offline(crate::presentation::ui_strings::SERVER_LINK_LOST);
    }
    fn on_playback_play(&mut self) {
        let _ = platform::player_play();
    }
    fn on_playback_pause(&mut self) {
        let _ = platform::player_pause();
    }
    fn on_playback_stop(&mut self) {
        let _ = platform::player_stop();
    }
    fn on_volume_change(&mut self, delta: i16) {
        let vol = platform::player_get_full_state().map(|s| s.2).unwrap_or(70);
        let new_vol = (vol as i32 + delta as i32).clamp(0, 100) as u8;
        let _ = platform::player_set_volume(new_vol);
    }
    fn on_catalog_browse(&mut self) {
        platform::gui_show_offline(crate::presentation::ui_strings::SERVER_LINK_LOST);
    }
    fn on_catalog_search(&mut self, _query: String) {
        platform::gui_show_offline(crate::presentation::ui_strings::SERVER_LINK_LOST);
    }
    fn on_retry(&mut self) {
        platform::gui_dismiss_state();
        self.exit_backoff = true;
    }
    fn on_cast_browse(&mut self) {}
    fn on_cast_select(&mut self, _receiver_id: String) {}
    fn on_cast_disconnect(&mut self) {}
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {
    esp_idf_sys::link_patches();
    esp_idf_sys::restore_posix_stdio_fds();

    // Start Native Station GUI on boot
    // Boot-path best-effort (phase 5 swallowed-error policy): a failed GUI
    // start is retried by the session (Session::new calls gui_start again),
    // and a failed platform init surfaces immediately after as a missing
    // Wi-Fi/server connection in the same loop.
    let _ = platform::gui_start();
    update_link_indicator(false);
    let initial_volume = platform::player_get_full_state()
        .map(|state| state.2)
        .unwrap_or(35);
    platform::gui_update_playback(0, "", "", "", initial_volume, false);

    let _ = platform::init();
    let mut local_player = crate::device_control::PlayerState::new();
    update_link_indicator(false);

    let mut failures = 0u32;
    loop {
        if platform::wifi_ready() && !platform::server_base_url().is_empty() {
            if let Ok(credentials) = pairing::credentials() {
                failures = 0;
                // A dropped session is the normal reconnect cycle here, not a
                // swallowed failure: the loop backs off and retries below.
                let _ = transport::run(&credentials.access_token);
            } else {
                failures = failures.saturating_add(1).min(5);
            }
        }
        let ceiling = (1u32 << failures).min(30) * 1_000;
        let mut random = [0u8; 4];
        platform::random_fill(&mut random);
        let backoff = (u32::from_le_bytes(random) % ceiling.max(1)).max(1_000);
        let steps = backoff / 50;
        let mut wifi_was_ready = platform::wifi_ready();
        'backoff: for _ in 0..steps {
            let mut handler = OfflineHandler { exit_backoff: false };
            while let Some(evt) = platform::gui_poll_event(0) {
                handler.dispatch_gui_event(evt);
                if handler.exit_backoff {
                    break;
                }
            }
            if handler.exit_backoff {
                break 'backoff;
            }
            transport::refresh_player(&mut local_player);
            let wifi_ready_now = platform::wifi_ready();
            if !wifi_was_ready && wifi_ready_now {
                // Wi-Fi just reassociated: retry the server session immediately
                // instead of sleeping through the remaining backoff.
                failures = 0;
                break 'backoff;
            }
            wifi_was_ready = wifi_ready_now;
            update_link_indicator(false);
            platform::delay(50);
        }
    }
}
