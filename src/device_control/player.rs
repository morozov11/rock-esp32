use serde_json::{json, Value};
use super::protocol::{validate_stream_uri, Envelope, DISPLAY_SURFACE_ID};
use super::display::DisplayState;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaybackStatus {
    Idle,
    Buffering,
    Playing,
    Paused,
    Stopped,
    Error,
}

impl PlaybackStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::Buffering => "buffering",
            Self::Playing => "playing",
            Self::Paused => "paused",
            Self::Stopped => "stopped",
            Self::Error => "error",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaybackAction {
    Play,
    Pause,
    Stop,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VolumeAction {
    SetLevel(u8),
    Change(i32),
    SetMute(bool),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PlayerCommand {
    PlayStream {
        command_id: String,
        station_id: String,
        stream_uri: String,
    },
    Playback {
        command_id: String,
        action: PlaybackAction,
    },
    Volume {
        command_id: String,
        action: VolumeAction,
    },
}

impl PlayerCommand {
    pub fn command_id(&self) -> &str {
        match self {
            PlayerCommand::PlayStream { command_id, .. } => command_id,
            PlayerCommand::Playback { command_id, .. } => command_id,
            PlayerCommand::Volume { command_id, .. } => command_id,
        }
    }
}

fn bounded(value: &str, min: usize, max: usize) -> bool {
    let n = value.chars().count();
    (min..=max).contains(&n)
}

/// Parses resolved stream, playback, and volume commands targeted at this player.
pub fn player_command(frame: &Envelope) -> Result<PlayerCommand, &'static str> {
    if frame.kind != "device.command" {
        return Err("unsupported_message_type");
    }
    let command_id = frame
        .payload
        .get("command_id")
        .and_then(Value::as_str)
        .ok_or("invalid_payload")?;
    if !bounded(command_id, 1, 128) {
        return Err("invalid_payload");
    }
    let body = frame
        .payload
        .get("body")
        .and_then(Value::as_object)
        .ok_or("invalid_payload")?;
    let name = body
        .get("name")
        .and_then(Value::as_str)
        .ok_or("invalid_payload")?;

    match name {
        "station.play_stream" => {
            let source = body
                .get("source")
                .and_then(Value::as_str)
                .ok_or("invalid_payload")?;
            if source != "rockserver_catalog" {
                return Err("capability_not_supported");
            }
            let station_id = body
                .get("station_id")
                .and_then(Value::as_str)
                .ok_or("invalid_payload")?;
            if !bounded(station_id, 1, 128) {
                return Err("invalid_payload");
            }
            let stream_uri = body
                .get("stream_uri")
                .and_then(Value::as_str)
                .ok_or("invalid_payload")?;
            validate_stream_uri(stream_uri).map_err(|_| "invalid_payload")?;
            Ok(PlayerCommand::PlayStream {
                command_id: command_id.into(),
                station_id: station_id.into(),
                stream_uri: stream_uri.into(),
            })
        }
        "playback.play" => Ok(PlayerCommand::Playback {
            command_id: command_id.into(),
            action: PlaybackAction::Play,
        }),
        "playback.pause" => Ok(PlayerCommand::Playback {
            command_id: command_id.into(),
            action: PlaybackAction::Pause,
        }),
        "playback.stop" => Ok(PlayerCommand::Playback {
            command_id: command_id.into(),
            action: PlaybackAction::Stop,
        }),
        "playback.next" | "playback.previous" => Err("capability_not_supported"),
        "volume.set_volume" => {
            let level = body
                .get("level")
                .and_then(Value::as_u64)
                .ok_or("invalid_payload")?;
            if level > 100 {
                return Err("invalid_payload");
            }
            Ok(PlayerCommand::Volume {
                command_id: command_id.into(),
                action: VolumeAction::SetLevel(level as u8),
            })
        }
        "volume.change_volume" => {
            let delta = body
                .get("delta")
                .and_then(Value::as_i64)
                .ok_or("invalid_payload")?;
            if !(-100..=100).contains(&delta) || delta == 0 {
                return Err("invalid_payload");
            }
            Ok(PlayerCommand::Volume {
                command_id: command_id.into(),
                action: VolumeAction::Change(delta as i32),
            })
        }
        "volume.set_mute" => {
            let muted = body
                .get("muted")
                .and_then(Value::as_bool)
                .ok_or("invalid_payload")?;
            Ok(PlayerCommand::Volume {
                command_id: command_id.into(),
                action: VolumeAction::SetMute(muted),
            })
        }
        _ => Err("capability_not_supported"),
    }
}

#[derive(Debug)]
pub struct PlayerState {
    status: PlaybackStatus,
    station_id: Option<String>,
    volume_level: u8,
    volume_muted: bool,
    revision: u64,
}

impl PlayerState {
    pub fn new() -> Self {
        Self {
            status: PlaybackStatus::Idle,
            station_id: None,
            volume_level: 35,
            volume_muted: false,
            revision: 1,
        }
    }

    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn status(&self) -> PlaybackStatus {
        self.status
    }

    pub fn station_id(&self) -> Option<&str> {
        self.station_id.as_deref()
    }

    pub fn volume_level(&self) -> u8 {
        self.volume_level
    }

    pub fn volume_muted(&self) -> bool {
        self.volume_muted
    }

    /// Idempotent transition to buffering a stream. Returns false if already playing or buffering that station.
    pub fn apply_play_stream(&mut self, station_id: &str) -> bool {
        if matches!(self.status, PlaybackStatus::Buffering | PlaybackStatus::Playing)
            && self.station_id.as_deref() == Some(station_id)
        {
            return false;
        }
        self.status = PlaybackStatus::Buffering;
        self.station_id = Some(station_id.to_string());
        self.revision += 1;
        true
    }

    pub fn set_playing(&mut self) -> bool {
        if self.status == PlaybackStatus::Playing {
            return false;
        }
        self.status = PlaybackStatus::Playing;
        self.revision += 1;
        true
    }

    pub fn set_paused(&mut self) -> bool {
        if self.status == PlaybackStatus::Paused {
            return false;
        }
        self.status = PlaybackStatus::Paused;
        self.revision += 1;
        true
    }

    pub fn set_stopped(&mut self) -> bool {
        if self.status == PlaybackStatus::Stopped {
            return false;
        }
        self.status = PlaybackStatus::Stopped;
        self.revision += 1;
        true
    }

    pub fn set_error(&mut self) -> bool {
        if self.status == PlaybackStatus::Error {
            return false;
        }
        self.status = PlaybackStatus::Error;
        self.revision += 1;
        true
    }

    pub fn apply_playback(&mut self, action: PlaybackAction) -> bool {
        match action {
            PlaybackAction::Play => {
                if self.status == PlaybackStatus::Playing {
                    false
                } else {
                    self.status = PlaybackStatus::Playing;
                    self.revision += 1;
                    true
                }
            }
            PlaybackAction::Pause => {
                if self.status == PlaybackStatus::Paused {
                    false
                } else {
                    self.status = PlaybackStatus::Paused;
                    self.revision += 1;
                    true
                }
            }
            PlaybackAction::Stop => {
                if self.status == PlaybackStatus::Stopped {
                    false
                } else {
                    self.status = PlaybackStatus::Stopped;
                    self.revision += 1;
                    true
                }
            }
        }
    }

    pub fn apply_volume(&mut self, action: VolumeAction) -> bool {
        match action {
            VolumeAction::SetLevel(level) => {
                if self.volume_level == level {
                    false
                } else {
                    self.volume_level = level;
                    self.revision += 1;
                    true
                }
            }
            VolumeAction::Change(delta) => {
                let new_level = (self.volume_level as i32 + delta).clamp(0, 100) as u8;
                if self.volume_level == new_level {
                    false
                } else {
                    self.volume_level = new_level;
                    self.revision += 1;
                    true
                }
            }
            VolumeAction::SetMute(muted) => {
                if self.volume_muted == muted {
                    false
                } else {
                    self.volume_muted = muted;
                    self.revision += 1;
                    true
                }
            }
        }
    }
}

pub fn full_state_snapshot(player: &PlayerState, display: &DisplayState) -> Value {
    json!({
        "playback": {
            "status": player.status().as_str(),
            "station_id": player.station_id(),
        },
        "volume": {
            "level": player.volume_level(),
            "muted": player.volume_muted(),
        },
        "display": {
            "surface_id": DISPLAY_SURFACE_ID,
            "view": display.view_name(),
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command(body: Value) -> Envelope {
        Envelope {
            protocol_version: 1,
            message_id: "10000000-0000-4000-8000-000000000001".into(),
            kind: "device.command".into(),
            sent_at: "2026-09-09T00:00:00Z".into(),
            payload: json!({
                "command_id": "60000000-0000-4000-8000-000000000001",
                "body": body
            }),
        }
    }

    #[test]
    fn parses_valid_play_stream() {
        let frame = command(json!({
            "name": "station.play_stream",
            "source": "rockserver_catalog",
            "station_id": "station.rock_classic",
            "stream_uri": "https://stream.example.com:8443/live.mp3"
        }));
        let cmd = player_command(&frame).unwrap();
        assert_eq!(
            cmd,
            PlayerCommand::PlayStream {
                command_id: "60000000-0000-4000-8000-000000000001".into(),
                station_id: "station.rock_classic".into(),
                stream_uri: "https://stream.example.com:8443/live.mp3".into(),
            }
        );
    }

    #[test]
    fn rejects_forbidden_stream_uris() {
        let loopback = command(json!({
            "name": "station.play_stream",
            "source": "rockserver_catalog",
            "station_id": "station.rock",
            "stream_uri": "http://127.0.0.1/live.mp3"
        }));
        assert_eq!(player_command(&loopback), Err("invalid_payload"));

        let private_ip = command(json!({
            "name": "station.play_stream",
            "source": "rockserver_catalog",
            "station_id": "station.rock",
            "stream_uri": "http://192.168.1.50:8000/stream"
        }));
        assert_eq!(player_command(&private_ip), Err("invalid_payload"));

        let fragment = command(json!({
            "name": "station.play_stream",
            "source": "rockserver_catalog",
            "station_id": "station.rock",
            "stream_uri": "https://stream.example.com/live#foo"
        }));
        assert_eq!(player_command(&fragment), Err("invalid_payload"));
    }

    #[test]
    fn parses_volume_and_bounds() {
        let set_vol = command(json!({"name": "volume.set_volume", "level": 75}));
        assert_eq!(
            player_command(&set_vol).unwrap(),
            PlayerCommand::Volume {
                command_id: "60000000-0000-4000-8000-000000000001".into(),
                action: VolumeAction::SetLevel(75),
            }
        );

        let over_vol = command(json!({"name": "volume.set_volume", "level": 101}));
        assert_eq!(player_command(&over_vol), Err("invalid_payload"));

        let chg_vol = command(json!({"name": "volume.change_volume", "delta": -10}));
        assert_eq!(
            player_command(&chg_vol).unwrap(),
            PlayerCommand::Volume {
                command_id: "60000000-0000-4000-8000-000000000001".into(),
                action: VolumeAction::Change(-10),
            }
        );

        let zero_delta = command(json!({"name": "volume.change_volume", "delta": 0}));
        assert_eq!(player_command(&zero_delta), Err("invalid_payload"));
    }

    #[test]
    fn player_state_idempotence() {
        let mut state = PlayerState::new();
        assert_eq!(state.revision(), 1);
        assert_eq!(state.volume_level(), 35);
        assert_eq!(state.status(), PlaybackStatus::Idle);

        assert!(state.apply_play_stream("station.rock"));
        assert_eq!(state.revision(), 2);
        assert_eq!(state.status(), PlaybackStatus::Buffering);

        // Same stream while buffering/playing -> idempotent, returns false
        assert!(!state.apply_play_stream("station.rock"));
        assert_eq!(state.revision(), 2);

        assert!(state.set_playing());
        assert_eq!(state.revision(), 3);
        assert!(!state.set_playing());
        assert_eq!(state.revision(), 3);

        assert!(state.apply_volume(VolumeAction::SetLevel(50)));
        assert_eq!(state.revision(), 4);
        assert!(!state.apply_volume(VolumeAction::SetLevel(50)));
        assert_eq!(state.revision(), 4);
    }
}