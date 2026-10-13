//! Inbound WebSocket message handling and command dispatch.

use super::catalog::cache_station_name;
use super::media::{cast_play_stream, play_local_station, refresh_player_with_cast};
use super::session::Session;
use super::{message_id, send};
use crate::device_control::{
    self, Envelope, PlaybackAction, PlaybackStatus, PlayerCommand, VolumeAction,
};
use crate::firmware::platform;
use crate::presentation::ui_strings as tr;
use serde_json::Value;

impl Session {
    pub(super) fn handle_inbound(&mut self, len: i32) -> Result<(), ()> {
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
    pub(super) fn local_play(&mut self) -> Result<(), ()> {
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
