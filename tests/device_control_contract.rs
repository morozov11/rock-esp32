use rock_esp32::device_control::{self, CommandHandler, Dispatcher, FrameError};
use serde_json::{Value, json};
use std::path::PathBuf;

fn fixture(name: &str) -> Vec<u8> {
    let root = std::env::var_os("ROCKSERVER_ROOT")
        .map(PathBuf::from)
        .or_else(|| {
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .parent()
                .map(|p| p.join("rockserver"))
        })
        .expect("set ROCKSERVER_ROOT to the RockServer checkout");
    std::fs::read(root.join("tests/fixtures/device-control/v1").join(name))
        .expect("canonical RockServer fixture must be readable")
}

#[test]
fn consumes_canonical_hello_and_welcome() {
    let hello = device_control::parse_frame(&fixture("hello-client.json")).unwrap();
    let welcome = device_control::parse_frame(&fixture("welcome-server.json")).unwrap();
    assert_eq!(hello.kind, "protocol.hello");
    assert_eq!(welcome.payload["limits"]["max_json_frame_bytes"], 65_536);
    assert_eq!(welcome.payload["limits"]["heartbeat_interval_seconds"], 20);
}

#[test]
fn re05_advertises_truthful_player_and_display_surface() {
    let ours = device_control::registration(
        "10000000-0000-4000-8000-000000000001",
        "2026-09-02T12:00:00Z",
        "0.1.0",
    );
    assert_eq!(
        ours["payload"]["manifest"]["roles"],
        json!(["display_surface", "player"])
    );
    let items = &ours["payload"]["manifest"]["capabilities"]["items"];
    assert_eq!(
        items[0],
        json!({"name":"media.playback","version":1,"actions":["play","pause","stop"]})
    );
    assert_eq!(
        items[1],
        json!({"name":"media.station","version":1,"sources":["rockserver_catalog"]})
    );
    assert_eq!(
        items[2],
        json!({"name":"media.volume","version":1,"minimum":0,"maximum":100,"step":1,"mute":true})
    );
    assert_eq!(
        items[3],
        json!({"name":"display.presentation","version":1,"views":["text","now_playing","sensor_grid"],"max_items":8,"max_text_length":128})
    );
    assert_eq!(
        ours["payload"]["manifest"]["surfaces"][0]["surface_id"],
        "display.main"
    );
}

#[test]
fn generic_state_serialization_matches_the_canonical_envelope() {
    let canonical = device_control::parse_frame(&fixture("esp32-state-full-client.json")).unwrap();
    let ours = device_control::state_full(
        "10000000-0000-4000-8000-000000000007",
        "2026-09-02T12:01:20Z",
        7,
        "2026-09-02T12:01:19Z",
        canonical.payload["snapshot"]["state"].clone(),
    );
    assert_eq!(ours["type"], "device.state_full");
    assert_eq!(ours["payload"], canonical.payload);
}

#[test]
fn rejects_canonical_invalid_frame_and_local_oversize_without_panicking() {
    assert_eq!(
        device_control::parse_frame(&fixture("invalid-frame-missing-message-id.json")).unwrap_err(),
        FrameError::Malformed
    );
    assert_eq!(
        device_control::parse_frame(&vec![b' '; device_control::MAX_JSON_FRAME_BYTES + 1])
            .unwrap_err(),
        FrameError::Oversized
    );
}

struct StationHandler;
impl CommandHandler for StationHandler {
    fn capability(&self) -> &'static str {
        "station."
    }
    fn handle(&mut self, body: &Value) -> Result<Value, &'static str> {
        Ok(json!({"station_id": body["station_id"]}))
    }
}

#[test]
fn generic_dispatch_routes_known_and_rejects_unknown_command() {
    let known = device_control::parse_frame(&fixture("station-command-server.json")).unwrap();
    let unknown = device_control::parse_frame(&fixture("unknown-command-client.json")).unwrap();
    let mut station = StationHandler;
    let mut handlers: [&mut dyn CommandHandler; 1] = [&mut station];
    let mut dispatcher = Dispatcher::new(&mut handlers);
    assert_eq!(
        dispatcher.dispatch(&known).unwrap()["station_id"],
        "station.jazz_fixture"
    );
    assert_eq!(
        dispatcher.dispatch(&unknown),
        Err("capability_not_supported")
    );
}

#[test]
fn pairing_link_matches_browser_fragment_contract_and_qr_is_bounded() {
    let request = device_control::PairingRequest {
        pairing_request_id: "request".into(),
        desktop_token: "desktop-token-1234".into(),
        approval_secret: "approval-secret-1234".into(),
        short_code: "AB12CD34".into(),
        verification_phrase: "AMBER-DAWN".into(),
    };
    let link = request.deep_link("https://rock.example/");
    assert_eq!(
        link,
        "https://rock.example/?code=AB12CD34#secret=approval-secret-1234"
    );
    let (width, modules) = device_control::qr_matrix(&link).unwrap();
    assert!(width <= 177);
    assert_eq!(modules.len(), usize::from(width) * usize::from(width));
}

#[test]
fn station_play_stream_command_parsing_and_boundaries() {
    let make_frame = |body: Value| device_control::Envelope {
        protocol_version: 1,
        message_id: "10000000-0000-4000-8000-000000000001".into(),
        kind: "device.command".into(),
        sent_at: "2026-09-09T00:00:00Z".into(),
        payload: json!({
            "command_id": "60000000-0000-4000-8000-000000000001",
            "body": body
        }),
    };

    // Valid public HTTP/HTTPS stream URIs
    let valid = make_frame(json!({
        "name": "station.play_stream",
        "source": "rockserver_catalog",
        "station_id": "station.rock_classic",
        "stream_uri": "https://stream.rockcast.example:8443/live.mp3?token=abc"
    }));
    let cmd = device_control::player_command(&valid).unwrap();
    assert_eq!(
        cmd,
        device_control::PlayerCommand::PlayStream {
            command_id: "60000000-0000-4000-8000-000000000001".into(),
            station_id: "station.rock_classic".into(),
            stream_uri: "https://stream.rockcast.example:8443/live.mp3?token=abc".into(),
        }
    );

    // Source must be rockserver_catalog
    let invalid_source = make_frame(json!({
        "name": "station.play_stream",
        "source": "untrusted_source",
        "station_id": "station.rock_classic",
        "stream_uri": "https://stream.rockcast.example/live.mp3"
    }));
    assert_eq!(
        device_control::player_command(&invalid_source),
        Err("capability_not_supported")
    );

    // SSRF / forbidden destination rejection
    let forbidden_cases = [
        "http://127.0.0.1/stream.mp3",
        "http://127.0.0.2:8000/stream.mp3",
        "http://localhost/stream.mp3",
        "http://10.0.1.2/stream.mp3",
        "http://192.168.1.100/stream.mp3",
        "http://172.16.0.5/stream.mp3",
        "http://169.254.169.254/latest/meta-data/",
        "http://100.64.0.1/stream.mp3",
        "http://224.0.0.1/stream.mp3",
        "http://[::1]/stream.mp3",
        "http://[fe80::1]/stream.mp3",
        "http://[fc00::1]/stream.mp3",
        "http://[::ffff:127.0.0.1]/stream.mp3",
        "http://2130706433/stream.mp3",
        "ftp://stream.example.com/audio.mp3",
        "https://user:pass@stream.example.com/live.mp3",
        "https://stream.example.com/live.mp3#frag",
        "https://stream.example.com:0/live.mp3",
        "https://stream.example.com:70000/live.mp3",
    ];
    for uri in forbidden_cases {
        let bad = make_frame(json!({
            "name": "station.play_stream",
            "source": "rockserver_catalog",
            "station_id": "station.rock_classic",
            "stream_uri": uri
        }));
        assert_eq!(
            device_control::player_command(&bad),
            Err("invalid_payload"),
            "expected rejection for {}",
            uri
        );
    }

    // URI length bound (> 2048 rejected)
    let long_path = "a".repeat(2050);
    let oversized = make_frame(json!({
        "name": "station.play_stream",
        "source": "rockserver_catalog",
        "station_id": "station.rock_classic",
        "stream_uri": format!("https://example.com/{}", long_path)
    }));
    assert_eq!(
        device_control::player_command(&oversized),
        Err("invalid_payload")
    );
}

#[test]
fn playback_and_volume_command_parsing_and_bounds() {
    let make_frame = |body: Value| device_control::Envelope {
        protocol_version: 1,
        message_id: "10000000-0000-4000-8000-000000000001".into(),
        kind: "device.command".into(),
        sent_at: "2026-09-09T00:00:00Z".into(),
        payload: json!({
            "command_id": "60000000-0000-4000-8000-000000000002",
            "body": body
        }),
    };

    // Playback actions
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "playback.play"}))).unwrap(),
        device_control::PlayerCommand::Playback {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::PlaybackAction::Play,
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "playback.pause"}))).unwrap(),
        device_control::PlayerCommand::Playback {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::PlaybackAction::Pause,
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "playback.stop"}))).unwrap(),
        device_control::PlayerCommand::Playback {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::PlaybackAction::Stop,
        }
    );
    // next / previous are explicitly not advertised or supported
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "playback.next"}))),
        Err("capability_not_supported")
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "playback.previous"}))),
        Err("capability_not_supported")
    );

    // Volume set_volume
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.set_volume", "level": 0}))).unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::SetLevel(0),
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.set_volume", "level": 100}))).unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::SetLevel(100),
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.set_volume", "level": 101}))),
        Err("invalid_payload")
    );

    // Volume change_volume
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.change_volume", "delta": 5}))).unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::Change(5),
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.change_volume", "delta": 0}))),
        Err("invalid_payload")
    );
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.change_volume", "delta": 105}))),
        Err("invalid_payload")
    );

    // Volume set_mute
    assert_eq!(
        device_control::player_command(&make_frame(json!({"name": "volume.set_mute", "muted": true}))).unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::SetMute(true),
        }
    );
}

#[test]
fn player_state_idempotent_replay_and_snapshot_serialization() {
    let mut player = device_control::PlayerState::new();
    let display = device_control::DisplayState::new();

    // Baseline snapshot matches canonical schema
    let snapshot = device_control::full_state_snapshot(&player, &display);
    assert_eq!(
        snapshot,
        json!({
            "playback": { "status": "idle", "station_id": null },
            "volume": { "level": 35, "muted": false },
            "display": { "surface_id": "display.main", "view": "dismissed" }
        })
    );

    // Initial play_stream increases revision
    assert!(player.apply_play_stream("station.jazz"));
    assert_eq!(player.revision(), 2);
    assert_eq!(player.status(), device_control::PlaybackStatus::Buffering);
    assert_eq!(player.station_id(), Some("station.jazz"));

    // Idempotent duplicate play_stream does NOT change state or increment revision
    assert!(!player.apply_play_stream("station.jazz"));
    assert_eq!(player.revision(), 2);

    // Transition to playing
    assert!(player.set_playing());
    assert_eq!(player.revision(), 3);
    assert_eq!(player.status(), device_control::PlaybackStatus::Playing);

    // Pause then duplicate pause
    assert!(player.apply_playback(device_control::PlaybackAction::Pause));
    assert_eq!(player.revision(), 4);
    assert_eq!(player.status(), device_control::PlaybackStatus::Paused);
    assert!(!player.apply_playback(device_control::PlaybackAction::Pause));
    assert_eq!(player.revision(), 4);

    // Volume set and duplicate set
    assert!(player.apply_volume(device_control::VolumeAction::SetLevel(50)));
    assert_eq!(player.revision(), 5);
    assert_eq!(player.volume_level(), 50);
    assert!(!player.apply_volume(device_control::VolumeAction::SetLevel(50)));
    assert_eq!(player.revision(), 5);
}

#[test]
fn stream_uri_never_leaks_into_logs_or_state_or_errors() {
    let secret_stream_uri = "https://private-cdn.example.com:8443/secret_token_12345/stream.mp3";
    let frame = device_control::Envelope {
        protocol_version: 1,
        message_id: "10000000-0000-4000-8000-000000000001".into(),
        kind: "device.command".into(),
        sent_at: "2026-09-09T00:00:00Z".into(),
        payload: json!({
            "command_id": "60000000-0000-4000-8000-000000000001",
            "body": {
                "name": "station.play_stream",
                "source": "rockserver_catalog",
                "station_id": "station.top40",
                "stream_uri": secret_stream_uri
            }
        }),
    };

    let cmd = device_control::player_command(&frame).unwrap();
    if let device_control::PlayerCommand::PlayStream { station_id, .. } = cmd {
        let mut player = device_control::PlayerState::new();
        player.apply_play_stream(&station_id);
        let display = device_control::DisplayState::new();
        let state = device_control::full_state_snapshot(&player, &display);

        // Verify secret URI does not appear in state snapshot
        let serialized_state = serde_json::to_string(&state).unwrap();
        assert!(!serialized_state.contains(secret_stream_uri));
        assert!(!serialized_state.contains("secret_token"));
        assert!(!serialized_state.contains("private-cdn"));

        // Verify error response does not leak stream_uri
        let err = device_control::error_response(
            "10000000-0000-4000-8000-000000000002",
            "2026-09-09T00:00:01Z",
            &frame.message_id,
            Some("60000000-0000-4000-8000-000000000001"),
            "playback_failed",
        );
        let serialized_err = serde_json::to_string(&err).unwrap();
        assert!(!serialized_err.contains(secret_stream_uri));

        // Verify command_failed does not leak stream_uri
        let cmd_failed = device_control::command_failed(
            "10000000-0000-4000-8000-000000000003",
            "2026-09-09T00:00:01Z",
            "60000000-0000-4000-8000-000000000001",
            "stream_connect_failed",
            "Failed to connect to stream server",
        );
        let serialized_cmd_failed = serde_json::to_string(&cmd_failed).unwrap();
        assert!(!serialized_cmd_failed.contains(secret_stream_uri));
    } else {
        panic!("expected PlayStream command");
    }
}

