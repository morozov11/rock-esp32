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
    platform::relay_stop();
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

static STATE_REVISION_COUNTER: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

fn next_state_revision(display: &DisplayState, player: &PlayerState) -> u64 {
    let base = (display.revision() + player.revision() - 1) as u32;
    let prev = STATE_REVISION_COUNTER.load(core::sync::atomic::Ordering::Relaxed);
    let rev = if prev == 0 { base } else { base.max(prev + 1) };
    STATE_REVISION_COUNTER.store(rev, core::sync::atomic::Ordering::Relaxed);
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

fn publish_state(
    display: &DisplayState,
    player: &PlayerState,
    output: &device_control::OutputMode,
) -> Result<(), ()> {
    let rev = next_state_revision(display, player);
    send(&device_control::state_full(
        &message_id(),
        &platform::now()?,
        rev,
        &platform::now()?,
        device_control::full_state_snapshot_with_output(player, display, output),
    ))
}

/// Synchronize only observed hardware state, including after an offline interval.
pub(crate) fn refresh_player(player: &mut PlayerState) -> bool {
    refresh_player_with_cast(player, false)
}

pub(crate) fn refresh_player_with_cast(player: &mut PlayerState, is_casting: bool) -> bool {
    let Some((status, station, volume, muted)) = platform::player_get_full_state() else {
        return false;
    };
    let before = player.revision();
    if !is_casting {
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

fn load_catalog(token: &mut String, query: Option<&str>) {
    let Ok(path) = device_control::catalog::catalog_path(query) else {
        platform::gui_show_error("Запрос должен содержать от 1 до 128 символов");
        return;
    };
    platform::gui_show_loading(if query.is_some() {
        "Поиск станций..."
    } else {
        "Загрузка каталога..."
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
        let _ = platform::source_update_bearer(token);
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
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    // Detach the speaker tap only: the shared source keeps running.
    let _ = platform::player_stop();
    ensure_source(station_id, token, source)?;
    let relay_url = platform::relay_listen().ok_or(())?;
    let content_type = platform::source_content_type();
    let res = cast_client.load_stream(&relay_url, station_id, &content_type);
    if res.is_err() {
        return Err(());
    }
    Ok(())
}

fn session(token: &str) -> Result<(), ()> {
    let mut token = token.to_string();
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
    let mut display = DisplayState::new();
    let mut player = PlayerState::new();
    let mut receiver_cache = device_control::ReceiverCache::new();
    let mut cast_client = super::cast_client::CastClient::new();
    let mut output_mode = device_control::OutputMode::Local;
    let mut last_stream: Option<(String, String)> = None;
    let mut source = SourceState { station: None };
    refresh_player_with_cast(&mut player, false);
    publish_state(&display, &player, &output_mode)?;
    platform::gui_start()?;
    platform::gui_connectivity(true);
    update_gui(&player);
    let (device_id, _) = platform::identity_load().ok_or(())?;
    let mut pending: Option<(String, u64)> = None;
    let mut retry: Option<platform::GuiEvent> = None;
    let mut network_check = 0;
    let mut next_heartbeat = platform::uptime_ms() + 20_000;
    let mut next_token_refresh = platform::uptime_ms() + 10 * 60_000;
    let mut last_heartbeat_ack = platform::uptime_ms();
    let mut heartbeat_sequence = 0u64;

    loop {
        if platform::uptime_ms() >= next_token_refresh {
            if let Ok(fresh) = super::pairing::fresh_access_token() {
                // The long-lived stream source reconnects with the stored
                // bearer; refresh it whenever the session token rotates.
                let _ = platform::source_update_bearer(&fresh);
                token = fresh;
                next_token_refresh = platform::uptime_ms() + 10 * 60_000;
            } else {
                next_token_refresh = platform::uptime_ms() + 60_000;
            }
        }
        let ack_age = platform::uptime_ms().saturating_sub(last_heartbeat_ack);
        if ack_age > 75_000 {
            platform::ws_log_heartbeat_timeout(ack_age);
            return Err(());
        }
        if platform::uptime_ms() >= next_heartbeat {
            send(&device_control::heartbeat(
                &message_id(),
                &platform::now()?,
                heartbeat_sequence,
            ))?;
            heartbeat_sequence = heartbeat_sequence.wrapping_add(1);
            next_heartbeat = platform::uptime_ms() + 20_000;
        }
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
                    load_catalog(&mut token, None);
                }
                platform::GuiEvent::CatalogSearch(query) => {
                    retry = Some(platform::GuiEvent::CatalogSearch(query.clone()));
                    load_catalog(&mut token, Some(&query));
                }
                platform::GuiEvent::PlaybackPlay => {
                    if cast_client.is_connected() {
                        if let Some((st_id, _)) = &last_stream {
                            if cast_play_stream(&mut cast_client, st_id, &mut token, &mut source)
                                .is_ok()
                            {
                                player.set_playing();
                                update_gui(&player);
                            }
                        }
                    } else {
                        let _ = platform::player_play();
                    }
                }
                platform::GuiEvent::PlaybackPause => {
                    if cast_client.is_connected() {
                        let _ = cast_client.stop();
                        player.set_paused();
                        update_gui(&player);
                    } else {
                        let _ = platform::player_pause();
                    }
                }
                platform::GuiEvent::PlaybackStop => {
                    pending = None;
                    if cast_client.is_connected() {
                        let _ = cast_client.stop();
                        player.set_stopped();
                        update_gui(&player);
                    }
                    let _ = platform::player_stop();
                    // Full stop tears the shared source down with it.
                    platform::source_stop();
                    source.station = None;
                }
                platform::GuiEvent::VolumeChange(delta) => {
                    let volume = (player.volume_level() as i32 + delta as i32).clamp(0, 100) as u8;
                    if cast_client.is_connected() {
                        let _ = cast_client.set_volume(volume);
                    }
                    let _ = platform::player_set_volume(volume);
                }
                platform::GuiEvent::VolumeSet(volume) => {
                    if cast_client.is_connected() {
                        let _ = cast_client.set_volume(volume);
                    }
                    let _ = platform::player_set_volume(volume);
                }
                platform::GuiEvent::CastBrowse => {
                    let found = platform::cast_discover(2500);
                    let active_list = receiver_cache.replace(found, platform::uptime_ms() / 1000);
                    platform::gui_set_cast_devices(&active_list, cast_client.receiver_id());
                    platform::gui_show_cast_menu(false);
                }
                platform::GuiEvent::CastSelect(rec_id) => {
                    let now_sec = platform::uptime_ms() / 1000;
                    if let Some(entry) = receiver_cache.get_entry(&rec_id, now_sec) {
                        platform::gui_show_cast_menu(true);
                        let local_stopped = last_stream.is_none()
                            || platform::player_stop_and_wait(12_000).is_ok();
                        if !local_stopped {
                            platform::gui_dismiss_cast_menu();
                            platform::gui_show_error("Не удалось освободить аудиоплеер");
                        } else if cast_client.connect(&entry.receiver_id, &entry.device).is_ok() {
                            output_mode = device_control::OutputMode::Chromecast {
                                receiver_id: entry.receiver_id.clone(),
                                receiver_name: entry.display_name.clone(),
                            };
                            platform::gui_update_cast_status(true, Some(&entry.display_name));
                            platform::gui_dismiss_cast_menu();

                            if let Some((st_id, _)) = &last_stream {
                                let _ = send(&device_control::heartbeat(
                                    &message_id(),
                                    &platform::now()?,
                                    heartbeat_sequence,
                                ));
                                heartbeat_sequence = heartbeat_sequence.wrapping_add(1);
                                next_heartbeat = platform::uptime_ms() + 20_000;
                                if cast_play_stream(&mut cast_client, st_id, &mut token, &mut source)
                                    .is_ok()
                                {
                                    player.apply_play_stream(st_id);
                                    player.set_playing();
                                    update_gui(&player);
                                }
                            }
                            publish_state(&display, &player, &output_mode)?;
                        } else {
                            platform::gui_dismiss_cast_menu();
                            platform::gui_show_error("Не удалось подключиться к Chromecast");
                            if let Some((st_id, _)) = &last_stream {
                                let _ = play_local_station(st_id, &mut token, &mut source);
                            }
                        }
                    }
                }
                platform::GuiEvent::CastDisconnect => {
                    if cast_client.is_connected() {
                        cast_client.disconnect();
                    }
                    output_mode = device_control::OutputMode::Local;
                    platform::gui_update_cast_status(false, None);
                    platform::gui_dismiss_cast_menu();

                    // The shared source keeps running: the speaker re-joins
                    // the same ring, no upstream reconnect, no gap.
                    if let Some((st_id, _)) = &last_stream {
                        let _ = play_local_station(st_id, &mut token, &mut source);
                    }
                    publish_state(&display, &player, &output_mode)?;
                }
                platform::GuiEvent::Retry => {}
            }
        }
        if refresh_player_with_cast(&mut player, cast_client.is_connected()) {
            publish_state(&display, &player, &output_mode)?;
            if player.status() == PlaybackStatus::Error {
                platform::gui_show_error("Ошибка воспроизведения аудио");
            }
        }
        if cast_client.is_connected() {
            if cast_client.pump().is_err() {
                if cast_client.reconnect().is_ok() {
                    platform::cast_log("Cast connection restored after idle disconnect");
                } else {
                    cast_client.disconnect();
                    output_mode = device_control::OutputMode::Local;
                    platform::gui_update_cast_status(false, None);
                    platform::gui_show_error("Связь с Chromecast потеряна");
                    // Deterministic fallback: the shared source keeps running,
                    // the speaker re-joins the ring without an upstream reconnect.
                    if let Some((st_id, _)) = &last_stream {
                        let _ = play_local_station(st_id, &mut token, &mut source);
                    }
                }
                publish_state(&display, &player, &output_mode)?;
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
            network_check = platform::uptime_ms() + 15000;
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
        // Observability only: log the parsed envelope kind, never the frame
        // content (device.command carries stream_uri with a token).
        platform::ws_log_rx_frame(&inbound.kind);
        if inbound.kind == "device.heartbeat_ack" {
            last_heartbeat_ack = platform::uptime_ms();
            continue;
        }
        if inbound.kind == "device.resync_requested" {
            publish_state(&display, &player, &output_mode)?;
            continue;
        }
        if inbound.kind == "command.result" || inbound.kind == "protocol.error" {
            let outcome = device_control::command_result_outcome(&inbound);
            if let Some((id, _)) = &pending {
                let matches = matches!(outcome, Some((command_id, _)) if command_id == id.as_str())
                    || inbound.kind == "protocol.error";
                if matches {
                    let failed = !matches!(outcome, Some((_, true)));
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
                publish_state(&display, &player, &output_mode)?;
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
                last_stream = Some((station_id.clone(), stream_uri.clone()));
                if cast_client.is_connected() {
                    let _ = platform::player_stop();
                    let res = cast_play_stream(&mut cast_client, &station_id, &mut token, &mut source);
                    if res.is_ok() {
                        player.apply_play_stream(&station_id);
                        player.set_playing();
                        update_gui(&player);
                        Ok(())
                    } else {
                        platform::cast_log("Cast play_stream failed; remaining in Cast mode without starting local audio");
                        player.set_stopped();
                        update_gui(&player);
                        Err(())
                    }
                } else {
                    if player.station_id() == Some(station_id.as_str())
                        && matches!(
                            player.status(),
                            PlaybackStatus::Playing | PlaybackStatus::Buffering
                        )
                    {
                        Ok(())
                    } else {
                        play_local_station(&station_id, &mut token, &mut source)
                    }
                }
            }
            PlayerCommand::Playback { action, .. } => {
                if cast_client.is_connected() {
                    match action {
                        PlaybackAction::Play => {
                            if let Some((st_id, _)) = &last_stream {
                                let _ = cast_play_stream(
                                    &mut cast_client,
                                    st_id,
                                    &mut token,
                                    &mut source,
                                );
                            }
                            player.set_playing();
                            update_gui(&player);
                            Ok(())
                        }
                        PlaybackAction::Pause => {
                            let _ = cast_client.stop_media();
                            player.set_paused();
                            update_gui(&player);
                            Ok(())
                        }
                        PlaybackAction::Stop => {
                            let _ = cast_client.stop_media();
                            player.set_stopped();
                            update_gui(&player);
                            // Full stop tears the shared source down with it.
                            platform::source_stop();
                            source.station = None;
                            Ok(())
                        }
                    }
                } else {
                    match action {
                        PlaybackAction::Play => platform::player_play(),
                        PlaybackAction::Pause => platform::player_pause(),
                        PlaybackAction::Stop => {
                            let res = platform::player_stop();
                            platform::source_stop();
                            source.station = None;
                            res
                        }
                    }
                }
            }
            PlayerCommand::Volume { action, .. } => match action {
                VolumeAction::SetLevel(level) => {
                    if cast_client.is_connected() {
                        let _ = cast_client.set_volume(level);
                    }
                    platform::player_set_volume(level)
                }
                VolumeAction::Change(delta) => {
                    let new_vol = (player.volume_level() as i32 + delta).clamp(0, 100) as u8;
                    if cast_client.is_connected() {
                        let _ = cast_client.set_volume(new_vol);
                    }
                    platform::player_set_volume(new_vol)
                }
                VolumeAction::SetMute(muted) => platform::player_set_mute(muted),
            },
        };
        refresh_player_with_cast(&mut player, cast_client.is_connected());
        if pending.as_ref().is_some_and(|(command, _)| command == &id) {
            pending = None;
            update_gui(&player);
        }
        match result {
            Ok(()) => {
                publish_state(&display, &player, &output_mode)?;
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
