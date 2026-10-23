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

/// Every user-visible volume change funnels through here: hardware first,
/// then the best-effort NVS persist so the last level survives a reboot.
/// Volume ownership stays put: the player is the hardware authority, the
/// platform stores the bytes, and this composition root decides when to
/// persist. Cast mirrors are the callers' concern (best-effort, local is
/// the authority).
pub(crate) fn apply_user_volume(volume: u8) {
    if platform::player_set_volume(volume).is_err() {
        platform::log_msg("Player volume set failed");
    }
    let _ = platform::settings_volume_save(volume);
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
    fn on_catalog_browse(&mut self, cursor: String);
    fn on_catalog_search(&mut self, query: String);
    fn on_retry(&mut self);
    fn on_cast_browse(&mut self);
    fn on_cast_select(&mut self, receiver_id: String);
    fn on_cast_disconnect(&mut self);
    fn on_favourites_show(&mut self);
    fn on_favourites_page(&mut self, page: u16);
    fn on_favourite_toggle(&mut self, station: String);
    fn on_voice_toggle(&mut self);

    fn dispatch_gui_event(&mut self, evt: platform::GuiEvent) {
        use platform::GuiEvent;
        match evt {
            GuiEvent::PlayStation(station) => self.on_play_station(station),
            GuiEvent::PlaybackPlay => self.on_playback_play(),
            GuiEvent::PlaybackPause => self.on_playback_pause(),
            GuiEvent::PlaybackStop => self.on_playback_stop(),
            GuiEvent::VolumeChange(delta) => self.on_volume_change(delta),
            GuiEvent::CatalogBrowse(cursor) => self.on_catalog_browse(cursor),
            GuiEvent::CatalogSearch(query) => self.on_catalog_search(query),
            GuiEvent::Retry => self.on_retry(),
            GuiEvent::CastBrowse => self.on_cast_browse(),
            GuiEvent::CastSelect(receiver_id) => self.on_cast_select(receiver_id),
            GuiEvent::CastDisconnect => self.on_cast_disconnect(),
            GuiEvent::FavouritesShow => self.on_favourites_show(),
            GuiEvent::FavouritesPage(page) => self.on_favourites_page(page),
            GuiEvent::FavouriteToggle(station) => self.on_favourite_toggle(station),
            GuiEvent::VoiceToggle => self.on_voice_toggle(),
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
        // Offline, the only applicable play is resuming PAUSED: restarting
        // after a full stop needs the station proxy bearer, i.e. the server.
        // Say so instead of a silent no-op.
        use crate::device_control::PlaybackStatus as HwStatus;
        match platform::player_status() {
            HwStatus::Paused => {
                let _ = platform::player_play();
            }
            // BUFFERING reaches PLAYING on its own; PLAYING needs nothing.
            HwStatus::Buffering | HwStatus::Playing => {}
            _ => platform::gui_show_offline(crate::presentation::ui_strings::SERVER_LINK_LOST),
        }
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
        apply_user_volume(new_vol);
    }
    fn on_catalog_browse(&mut self, _cursor: String) {
        platform::gui_show_offline(crate::presentation::ui_strings::SERVER_LINK_LOST);
    }
    fn on_catalog_search(&mut self, _query: String) {
        platform::gui_show_offline(crate::presentation::ui_strings::SERVER_LINK_LOST);
    }
    fn on_retry(&mut self) {
        platform::gui_dismiss_state();
        self.exit_backoff = true;
    }
    fn on_cast_browse(&mut self) {
        // mDNS/subnet scanning needs no server: refresh the menu so the
        // spinner does not run forever while offline. Selecting still needs
        // the server (stream bearer), which on_cast_select reports.
        let found = platform::cast_discover(2500);
        let mut cache = crate::device_control::ReceiverCache::new();
        let active_list = cache.replace(found, platform::uptime_ms() / 1000);
        platform::gui_set_cast_devices(&active_list, None);
        platform::gui_show_cast_menu(false);
    }
    fn on_cast_select(&mut self, _receiver_id: String) {
        // Casting needs the server-side station proxy (auth bearer): dismiss
        // the menu state the tap left behind and say so.
        platform::gui_dismiss_cast_menu();
        platform::gui_show_error(crate::presentation::ui_strings::SERVER_LINK_LOST);
    }
    fn on_cast_disconnect(&mut self) {}
    fn on_favourites_show(&mut self) { platform::gui_show_offline("Избранное недоступно"); }
    fn on_favourites_page(&mut self, _page: u16) { platform::gui_show_offline("Избранное недоступно"); }
    fn on_favourite_toggle(&mut self, _station: String) { platform::gui_show_offline("Избранное недоступно"); }
    fn on_voice_toggle(&mut self) { platform::gui_voice_state(9); }
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
    // The boot volume seed reads hardware state, which app_main has already
    // restored from NVS — so the GUI shows the persisted level from the
    // first frame, long before the network session exists.
    let initial_volume = platform::player_get_full_state()
        .map(|state| state.2)
        .unwrap_or(35);
    platform::gui_update_playback(0, "", "", "", "", initial_volume, false);

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
