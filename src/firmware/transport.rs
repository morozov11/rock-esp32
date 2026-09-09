//! Bounded WebSocket session and player/display command dispatch.

use super::platform;
use crate::device_control::{
    self, DisplayState, DisplayView, PlaybackAction, PlaybackStatus, PlayerCommand, PlayerState,
    VolumeAction,
};
use core::ffi::c_char;
use serde_json::Value;

const FRAME_CAPACITY: usize = device_control::MAX_JSON_FRAME_BYTES + 1;

pub(crate) fn run(token: &str) -> Result<(), ()> {
    platform::ws_start(token)?;
    let result = session();
    platform::ws_stop();
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

fn session() -> Result<(), ()> {
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
    publish_state(&display, &player)?;
    loop {
        let len = platform::ws_receive(&mut frame, 1_000);
        if len < 0 {
            return Err(());
        }

        // Poll asynchronous hardware player state changes (e.g. buffering -> playing)
        if let Some(hw_status) = platform::player_get_status() {
            if player.status() == PlaybackStatus::Buffering && hw_status == "playing" {
                player.set_playing();
                let st_id = player.station_id().unwrap_or("station").to_string();
                let view = DisplayView::NowPlaying {
                    station_id: st_id,
                    title: "PLAYING".into(),
                    subtitle: "RockCast Radio".into(),
                };
                display.apply(view.clone());
                crate::presentation::render(&view);
                publish_state(&display, &player)?;
            } else if player.status() != PlaybackStatus::Error && hw_status == "error" {
                player.set_error();
                let view = DisplayView::Text("PLAYBACK ERROR".into());
                display.apply(view.clone());
                crate::presentation::render(&view);
                publish_state(&display, &player)?;
            }
        }

        if len == 0 {
            continue;
        }

        let Ok(inbound) = device_control::parse_frame(unsafe {
            core::slice::from_raw_parts(frame.as_ptr() as *const u8, len as usize)
        }) else {
            continue;
        };
        if inbound.kind != "device.command" {
            continue;
        }

        let body_name = inbound
            .payload
            .get("body")
            .and_then(|b| b.get("name"))
            .and_then(Value::as_str);
        let command_id = inbound.payload.get("command_id").and_then(Value::as_str);

        if body_name == Some("display.set_view") {
            let command = match device_control::display_command(&inbound) {
                Ok(command) => command,
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
            let changed = display.apply(command.view.clone());
            if changed && !crate::presentation::render(&command.view) {
                return Err(());
            }
            if changed {
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

        // Handle player commands (station.play_stream, playback.*, volume.*)
        let command = match device_control::player_command(&inbound) {
            Ok(command) => command,
            Err(code) => {
                if let Some(cmd_id) = command_id {
                    send(&device_control::command_failed(
                        &message_id(),
                        &platform::now()?,
                        cmd_id,
                        code,
                        code,
                    ))?;
                } else {
                    send(&device_control::error_response(
                        &message_id(),
                        &platform::now()?,
                        &inbound.message_id,
                        None,
                        code,
                    ))?;
                }
                continue;
            }
        };

        // Immediate command acceptance
        send(&device_control::command_accepted(
            &message_id(),
            &platform::now()?,
            command.command_id(),
        ))?;

        match command {
            PlayerCommand::PlayStream {
                command_id,
                station_id,
                stream_uri,
            } => {
                if !player.apply_play_stream(&station_id) {
                    // Idempotent duplicate: already playing/buffering this station
                    send(&device_control::command_succeeded(
                        &message_id(),
                        &platform::now()?,
                        &command_id,
                        current_revision(&display, &player),
                    ))?;
                } else {
                    let view = DisplayView::NowPlaying {
                        station_id: station_id.clone(),
                        title: "BUFFERING".into(),
                        subtitle: "RockCast Radio".into(),
                    };
                    display.apply(view.clone());
                    crate::presentation::render(&view);

                    match platform::player_play_stream(&stream_uri, &station_id) {
                        Ok(()) => {
                            publish_state(&display, &player)?;
                            send(&device_control::command_succeeded(
                                &message_id(),
                                &platform::now()?,
                                &command_id,
                                current_revision(&display, &player),
                            ))?;
                        }
                        Err(()) => {
                            player.set_error();
                            let err_view = DisplayView::Text("PLAYBACK ERROR".into());
                            display.apply(err_view.clone());
                            crate::presentation::render(&err_view);
                            publish_state(&display, &player)?;
                            send(&device_control::command_failed(
                                &message_id(),
                                &platform::now()?,
                                &command_id,
                                "playback_failed",
                                "Stream playback initiation failed",
                            ))?;
                        }
                    }
                }
            }
            PlayerCommand::Playback { command_id, action } => match action {
                PlaybackAction::Play => {
                    if player.status() == PlaybackStatus::Playing {
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else if platform::player_play().is_ok() {
                        player.apply_playback(action);
                        let st_id = player.station_id().unwrap_or("station").to_string();
                        let view = DisplayView::NowPlaying {
                            station_id: st_id,
                            title: "PLAYING".into(),
                            subtitle: "RockCast Radio".into(),
                        };
                        display.apply(view.clone());
                        crate::presentation::render(&view);
                        publish_state(&display, &player)?;
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else {
                        send(&device_control::command_failed(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            "playback_failed",
                            "Playback resume failed",
                        ))?;
                    }
                }
                PlaybackAction::Pause => {
                    if player.status() == PlaybackStatus::Paused {
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else if platform::player_pause().is_ok() {
                        player.apply_playback(action);
                        let st_id = player.station_id().unwrap_or("station").to_string();
                        let view = DisplayView::NowPlaying {
                            station_id: st_id,
                            title: "PAUSED".into(),
                            subtitle: "".into(),
                        };
                        display.apply(view.clone());
                        crate::presentation::render(&view);
                        publish_state(&display, &player)?;
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else {
                        send(&device_control::command_failed(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            "playback_failed",
                            "Playback pause failed",
                        ))?;
                    }
                }
                PlaybackAction::Stop => {
                    if player.status() == PlaybackStatus::Stopped {
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else if platform::player_stop().is_ok() {
                        player.apply_playback(action);
                        let view = DisplayView::Text("STOPPED".into());
                        display.apply(view.clone());
                        crate::presentation::render(&view);
                        publish_state(&display, &player)?;
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else {
                        send(&device_control::command_failed(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            "playback_failed",
                            "Playback stop failed",
                        ))?;
                    }
                }
            },
            PlayerCommand::Volume { command_id, action } => match action {
                VolumeAction::SetLevel(level) => {
                    if player.volume_level() == level {
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else if platform::player_set_volume(level).is_ok() {
                        player.apply_volume(action);
                        publish_state(&display, &player)?;
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else {
                        send(&device_control::command_failed(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            "hardware_error",
                            "Failed to set volume level",
                        ))?;
                    }
                }
                VolumeAction::Change(delta) => {
                    let new_level = (player.volume_level() as i32 + delta).clamp(0, 100) as u8;
                    if player.volume_level() == new_level {
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else if platform::player_set_volume(new_level).is_ok() {
                        player.apply_volume(action);
                        publish_state(&display, &player)?;
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else {
                        send(&device_control::command_failed(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            "hardware_error",
                            "Failed to change volume",
                        ))?;
                    }
                }
                VolumeAction::SetMute(muted) => {
                    if player.volume_muted() == muted {
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else if platform::player_set_mute(muted).is_ok() {
                        player.apply_volume(action);
                        publish_state(&display, &player)?;
                        send(&device_control::command_succeeded(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            current_revision(&display, &player),
                        ))?;
                    } else {
                        send(&device_control::command_failed(
                            &message_id(),
                            &platform::now()?,
                            &command_id,
                            "hardware_error",
                            "Failed to set mute state",
                        ))?;
                    }
                }
            },
        }
    }
}
