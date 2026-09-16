//! Firmware composition root: initializes ESP-IDF then owns reconnect policy.

mod pairing;
mod platform;
mod transport;

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {
    esp_idf_sys::link_patches();
    esp_idf_sys::restore_posix_stdio_fds();

    // Start Native Station GUI on boot
    let _ = platform::gui_start();
    platform::gui_connectivity(false);
    let initial_volume = platform::player_get_full_state()
        .map(|state| state.2)
        .unwrap_or(35);
    platform::gui_update_playback(0, "", "", "", initial_volume, false);

    let _ = platform::init();
    let mut local_player = crate::device_control::PlayerState::new();
    platform::gui_connectivity(false);

    let mut failures = 0u32;
    loop {
        if platform::wifi_ready() && !platform::server_base_url().is_empty() {
            if let Ok(credentials) = pairing::credentials() {
                failures = 0;
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
        'backoff: for _ in 0..steps {
            while let Some(evt) = platform::gui_poll_event(0) {
                match evt {
                    platform::GuiEvent::VolumeChange(delta) => {
                        let vol = platform::player_get_full_state().map(|s| s.2).unwrap_or(70);
                        let new_vol = (vol as i32 + delta as i32).clamp(0, 100) as u8;
                        let _ = platform::player_set_volume(new_vol);
                    }
                    platform::GuiEvent::VolumeSet(vol) => {
                        let _ = platform::player_set_volume(vol);
                    }
                    platform::GuiEvent::PlaybackPlay => {
                        let _ = platform::player_play();
                    }
                    platform::GuiEvent::PlaybackPause => {
                        let _ = platform::player_pause();
                    }
                    platform::GuiEvent::PlaybackStop => {
                        let _ = platform::player_stop();
                    }
                    platform::GuiEvent::CatalogBrowse
                    | platform::GuiEvent::CatalogSearch(_)
                    | platform::GuiEvent::PlayStation(_) => {
                        platform::gui_show_offline("Связь с сервером потеряна");
                    }
                    platform::GuiEvent::Retry => {
                        platform::gui_dismiss_state();
                        break 'backoff;
                    }
                }
            }
            transport::refresh_player(&mut local_player);
            platform::gui_connectivity(false);
            platform::delay(50);
        }
    }
}
