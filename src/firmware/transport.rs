//! Bounded WebSocket session and player/display command dispatch.

use super::platform;
use crate::device_control::{
    self, DisplayState, PlaybackAction, PlaybackStatus, PlayerCommand, PlayerState, VolumeAction,
};
use core::ffi::c_char;
use serde_json::Value;

const FRAME_CAPACITY: usize = device_control::MAX_JSON_FRAME_BYTES + 1;

pub(crate) fn run(token: &str) -> Result<(), ()> {
    platform::ws_start(token)?;
    let result = session(token);
    platform::ws_stop();
    platform::gui_connectivity(false);
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
        b[0],
        b[1],
        b[2],
        b[3],
        b[4],
        b[5],
        b[6],
        b[7],
        b[8],
        b[9],
        b[10],
        b[11],
        b[12],
        b[13],
        b[14],
        b[15]
    )
}

fn current_revision(display: &DisplayState, player: &PlayerState) -> u64 {
    display.revision() + player.revision() - 1
}

fn publish_state(display: &DisplayState, player: &PlayerState) -> Result<(), ()> {
    send(&device_control::state_full(
        &message_id(),
        &platform::now()?,
        current_revision(display, player),
        &platform::now()?,
        device_control::full_state_snapshot(player, display),
    ))
}

/// Synchronize only observed hardware state, including after an offline interval.
pub(crate) fn refresh_player(player: &mut PlayerState) -> bool {
    let Some((status, station, volume, muted)) = platform::player_get_full_state() else {
        return false;
    };
    let before = player.revision();
    if !station.is_empty() && player.station_id() != Some(station.as_str()) {
        player.apply_play_stream(&station);
    }
    match status.as_str() {
        "buffering" => {
            player.apply_play_stream(&station);
        }
        "playing" => {
            player.set_playing();
        }
        "paused" => {
            player.set_paused();
        }
        "stopped" => {
            player.set_stopped();
        }
        "error" => {
            player.set_error();
        }
        _ => {}
    }
    player.apply_volume(VolumeAction::SetLevel(volume));
    player.apply_volume(VolumeAction::SetMute(muted));
    if player.revision() != before {
        update_gui(player);
        return true;
    }
    false
}

fn update_gui(player: &PlayerState) {
    let code = match player.status() {
        PlaybackStatus::Idle => 0,
        PlaybackStatus::Buffering => 1,
        PlaybackStatus::Playing => 2,
        PlaybackStatus::Paused => 3,
        PlaybackStatus::Stopped => 4,
        PlaybackStatus::Error => 5,
    };
    platform::gui_update_playback(
        code,
        player.station_id().unwrap_or(""),
        "",
        "",
        player.volume_level(),
        player.volume_muted(),
    );
}

fn load_catalog(token: &str, query: Option<&str>) {
    let Ok(path) = device_control::catalog::catalog_path(query) else {
        platform::gui_show_error("Запрос должен содержать от 1 до 128 символов");
        return;
    };
    platform::gui_show_loading(if query.is_some() {
        "Поиск станций..."
    } else {
        "Загрузка каталога..."
    });
    match platform::get(&path, Some(token)) {
        Ok((200, bytes)) => {
            if query.is_some() {
                match device_control::parse_search_page(&bytes) {
                    Ok(page) => {
                        platform::gui_set_search_results(&page.stations);
                        if page.stations.is_empty() {
                            platform::gui_show_empty("Ничего не найдено");
                        }
                    }
                    Err(_) => platform::gui_show_error("Некорректный формат поиска"),
                }
            } else {
                match device_control::parse_catalog_page(&bytes) {
                    Ok(page) => platform::gui_set_browse_stations(
                        &page.stations,
                        page.next_cursor.as_deref(),
                    ),
                    Err(_) => platform::gui_show_error("Некорректный формат каталога"),
                }
            }
        }
        _ => platform::gui_show_error("Не удалось получить станции. Повторите запрос."),
    }
}

fn session(token: &str) -> Result<(), ()> {
    send(&device_control::hello(&message_id(), &platform::now()?))?;
    let mut frame = vec![0 as c_char; FRAME_CAPACITY];
    let len = platform::ws_receive(&mut frame, 10_000);
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
    let mut display = DisplayState::new();
    let mut player = PlayerState::new();
    refresh_player(&mut player);
    publish_state(&display, &player)?;
    platform::gui_start()?;
    platform::gui_connectivity(true);
    update_gui(&player);
    let (device_id, _) = platform::identity_load().ok_or(())?;
    let mut pending: Option<(String, u64)> = None;
    let mut retry: Option<platform::GuiEvent> = None;
    let mut network_check = 0;

    loop {
        while let Some(mut evt) = platform::gui_poll_event(0) {
            if matches!(evt, platform::GuiEvent::Retry) {
                let Some(previous) = retry.clone() else {
                    platform::gui_dismiss_state();
                    continue;
                };
                evt = previous;
            }
            match evt {
                platform::GuiEvent::PlayStation(station) => {
                    if station.is_empty() || station.len() > 128 {
                        continue;
                    }
                    let command = message_id();
                    send(&device_control::station_play_station(
                        &message_id(),
                        &platform::now()?,
                        &command,
                        &device_id,
                        &station,
                    ))?;
                    // Do not pre-apply PlayStream: the resolved server command must reach hardware.
                    pending = Some((command, platform::uptime_ms() + 30_000));
                    retry = Some(platform::GuiEvent::PlayStation(station.clone()));
                    platform::gui_set_starting_station(&station);
                }
                platform::GuiEvent::CatalogBrowse => {
                    retry = Some(platform::GuiEvent::CatalogBrowse);
                    load_catalog(token, None);
                }
                platform::GuiEvent::CatalogSearch(query) => {
                    retry = Some(platform::GuiEvent::CatalogSearch(query.clone()));
                    load_catalog(token, Some(&query));
                }
                platform::GuiEvent::PlaybackPlay => {
                    let _ = platform::player_play();
                }
                platform::GuiEvent::PlaybackPause => {
                    let _ = platform::player_pause();
                }
                platform::GuiEvent::PlaybackStop => {
                    pending = None;
                    let _ = platform::player_stop();
                }
                platform::GuiEvent::VolumeChange(delta) => {
                    let volume = (player.volume_level() as i32 + delta as i32).clamp(0, 100) as u8;
                    let _ = platform::player_set_volume(volume);
                }
                platform::GuiEvent::VolumeSet(volume) => {
                    let _ = platform::player_set_volume(volume);
                }
                platform::GuiEvent::Retry => {}
            }
        }
        if refresh_player(&mut player) {
            publish_state(&display, &player)?;
            if player.status() == PlaybackStatus::Error {
                platform::gui_show_error("Ошибка воспроизведения аудио");
            }
        }
        if pending
            .as_ref()
            .is_some_and(|(_, deadline)| platform::uptime_ms() >= *deadline)
        {
            pending = None;
            update_gui(&player);
            platform::gui_show_error("Станция не ответила. Повторите запуск.");
        }
        if platform::uptime_ms() >= network_check {
            platform::gui_connectivity(true);
            network_check = platform::uptime_ms() + 1000;
        }
        let len = platform::ws_receive(&mut frame, 50);
        if len < 0 {
            return Err(());
        }
        if len == 0 {
            continue;
        }
        let Ok(inbound) = device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
        }) else {
            continue;
        };
        if inbound.kind == "command.result" || inbound.kind == "protocol.error" {
            if let Some((id, _)) = &pending {
                let matches = inbound.payload.get("command_id").and_then(Value::as_str) == Some(id)
                    || inbound.kind == "protocol.error";
                if matches {
                    let failed =
                        inbound.payload.get("status").and_then(Value::as_str) != Some("succeeded");
                    pending = None;
                    update_gui(&player);
                    if failed {
                        platform::gui_show_error("Сервер отклонил запуск станции");
                    }
                }
            }
            continue;
        }
        if inbound.kind != "device.command" {
            continue;
        }
        let command_id = inbound.payload.get("command_id").and_then(Value::as_str);
        let body_name = inbound
            .payload
            .get("body")
            .and_then(|b| b.get("name"))
            .and_then(Value::as_str);
        if body_name == Some("display.set_view") {
            let command = match device_control::display_command(&inbound) {
                Ok(c) => c,
                Err(code) => {
                    send(&device_control::error_response(
                        &message_id(),
                        &platform::now()?,
                        &inbound.message_id,
                        command_id,
                        code,
                    ))?;
                    continue;
                }
            };
            send(&device_control::command_accepted(
                &message_id(),
                &platform::now()?,
                &command.command_id,
            ))?;
            if display.apply(command.view.clone()) {
                if !crate::presentation::render(&command.view) {
                    return Err(());
                }
                publish_state(&display, &player)?;
            }
            send(&device_control::command_succeeded(
                &message_id(),
                &platform::now()?,
                &command.command_id,
                current_revision(&display, &player),
            ))?;
            continue;
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
                continue;
            }
        };
        let id = command.command_id().to_string();
        send(&device_control::command_accepted(
            &message_id(),
            &platform::now()?,
            &id,
        ))?;
        let result = match command {
            PlayerCommand::PlayStream {
                station_id,
                stream_uri,
                ..
            } => {
                if player.station_id() == Some(station_id.as_str())
                    && matches!(
                        player.status(),
                        PlaybackStatus::Playing | PlaybackStatus::Buffering
                    )
                {
                    Ok(())
                } else {
                    platform::player_play_stream(&stream_uri, &station_id)
                }
            }
            PlayerCommand::Playback { action, .. } => match action {
                PlaybackAction::Play => platform::player_play(),
                PlaybackAction::Pause => platform::player_pause(),
                PlaybackAction::Stop => platform::player_stop(),
            },
            PlayerCommand::Volume { action, .. } => match action {
                VolumeAction::SetLevel(level) => platform::player_set_volume(level),
                VolumeAction::Change(delta) => platform::player_set_volume(
                    (player.volume_level() as i32 + delta).clamp(0, 100) as u8,
                ),
                VolumeAction::SetMute(muted) => platform::player_set_mute(muted),
            },
        };
        refresh_player(&mut player);
        if pending.as_ref().is_some_and(|(command, _)| command == &id) {
            pending = None;
            update_gui(&player);
        }
        match result {
            Ok(()) => {
                publish_state(&display, &player)?;
                send(&device_control::command_succeeded(
                    &message_id(),
                    &platform::now()?,
                    &id,
                    current_revision(&display, &player),
                ))?;
            }
            Err(()) => {
                platform::gui_show_error("Не удалось выполнить команду плеера");
                send(&device_control::command_failed(
                    &message_id(),
                    &platform::now()?,
                    &id,
                    "playback_failed",
                    "Player operation failed",
                ))?;
            }
        }
    }
}
