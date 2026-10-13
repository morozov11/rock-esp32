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
    let heartbeat = device_control::heartbeat(
        "10000000-0000-4000-8000-000000000001",
        "2026-09-02T12:00:20Z",
        7,
    );
    assert_eq!(heartbeat["type"], "device.heartbeat");
    assert_eq!(heartbeat["payload"], json!({"sequence": 7}));
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
        json!(["display_surface", "player", "controller"])
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
            station_name: None,
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
        device_control::player_command(&make_frame(
            json!({"name": "volume.set_volume", "level": 0})
        ))
        .unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::SetLevel(0),
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(
            json!({"name": "volume.set_volume", "level": 100})
        ))
        .unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::SetLevel(100),
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(
            json!({"name": "volume.set_volume", "level": 101})
        )),
        Err("invalid_payload")
    );

    // Volume change_volume
    assert_eq!(
        device_control::player_command(&make_frame(
            json!({"name": "volume.change_volume", "delta": 5})
        ))
        .unwrap(),
        device_control::PlayerCommand::Volume {
            command_id: "60000000-0000-4000-8000-000000000002".into(),
            action: device_control::VolumeAction::Change(5),
        }
    );
    assert_eq!(
        device_control::player_command(&make_frame(
            json!({"name": "volume.change_volume", "delta": 0})
        )),
        Err("invalid_payload")
    );
    assert_eq!(
        device_control::player_command(&make_frame(
            json!({"name": "volume.change_volume", "delta": 105})
        )),
        Err("invalid_payload")
    );

    // Volume set_mute
    assert_eq!(
        device_control::player_command(&make_frame(
            json!({"name": "volume.set_mute", "muted": true})
        ))
        .unwrap(),
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

#[test]
fn device_station_dto_and_catalog_page_parsing_and_boundaries() {
    // 1. Valid page from RS-2 catalog without stream_url
    let valid_json = json!({
        "request_id": "req-1234-abcd",
        "stations": [
            {
                "id": "station.rock_classic",
                "name": "Rock Classic Radio",
                "tags": ["rock", "classic"],
                "health": "healthy",
                "codec": "mp3",
                "bitrate_kbps": 128,
                "extra_unknown_server_field": "tolerated"
            },
            {
                "id": "station.metal",
                "name": "Heavy Metal FM",
                "tags": ["metal"],
                "health": "unknown"
            }
        ],
        "next_cursor": "station.metal"
    });

    let bytes = serde_json::to_vec(&valid_json).unwrap();
    let page = device_control::parse_catalog_page(&bytes).unwrap();
    assert_eq!(page.request_id, "req-1234-abcd");
    assert_eq!(page.stations.len(), 2);
    assert_eq!(page.stations[0].id, "station.rock_classic");
    assert_eq!(page.stations[0].name, "Rock Classic Radio");
    assert_eq!(page.stations[0].tags, vec!["rock", "classic"]);
    assert_eq!(page.stations[0].health, "healthy");
    assert_eq!(page.stations[0].codec.as_deref(), Some("mp3"));
    assert_eq!(page.stations[0].bitrate_kbps, Some(128));
    assert_eq!(page.next_cursor.as_deref(), Some("station.metal"));

    // 2. Reject oversized catalog (> 20 stations)
    let mut oversized_stations = Vec::new();
    for i in 0..21 {
        oversized_stations.push(json!({
            "id": format!("station.{}", i),
            "name": format!("Station {}", i),
            "health": "healthy"
        }));
    }
    let oversized_json = json!({
        "request_id": "req-oversized",
        "stations": oversized_stations
    });
    let oversized_bytes = serde_json::to_vec(&oversized_json).unwrap();
    assert_eq!(
        device_control::parse_catalog_page(&oversized_bytes),
        Err(device_control::CatalogError::Oversized)
    );

    // 3. Reject malformed station metadata (e.g. invalid health or empty id)
    let bad_station_json = json!({
        "request_id": "req-bad",
        "stations": [
            {
                "id": "",
                "name": "Missing ID",
                "health": "healthy"
            }
        ]
    });
    let bad_bytes = serde_json::to_vec(&bad_station_json).unwrap();
    assert_eq!(
        device_control::parse_catalog_page(&bad_bytes),
        Err(device_control::CatalogError::Malformed)
    );

    // 4. Over-tagged stations (the browser catalog has no tag cap): the page
    // parses and only the first MAX_STATION_TAGS tags survive.
    let overflow_tags: Vec<String> = (0..22).map(|i| format!("tag-{i}")).collect();
    let overtagged_json = json!({
        "request_id": "req-overtagged",
        "stations": [
            {
                "id": "station.rb-heavy",
                "name": "Over-tagged Radio",
                "tags": overflow_tags,
                "health": "unknown"
            }
        ]
    });
    let overtagged_bytes = serde_json::to_vec(&overtagged_json).unwrap();
    let page = device_control::parse_catalog_page(&overtagged_bytes).unwrap();
    assert_eq!(page.stations[0].tags.len(), 10);
    assert_eq!(page.stations[0].tags[0], "tag-0");
    assert_eq!(page.stations[0].tags[9], "tag-9");

    // 4b. Same tolerance on the search page.
    let overtagged_search = json!({
        "request_id": "req-search-overtagged",
        "stations": [
            {
                "id": "station.rb-search",
                "name": "Over-tagged Search Hit",
                "tags": overflow_tags,
                "health": "unknown"
            }
        ]
    });
    let search_bytes = serde_json::to_vec(&overtagged_search).unwrap();
    let search_page = device_control::parse_search_page(&search_bytes).unwrap();
    assert_eq!(search_page.stations[0].tags.len(), 10);
}

#[test]
fn device_search_page_parsing_and_boundaries() {
    let search_json = json!({
        "request_id": "req-search-5678",
        "stations": [
            {
                "id": "station.alternative",
                "name": "Alternative Rock",
                "health": "healthy"
            }
        ]
    });
    let bytes = serde_json::to_vec(&search_json).unwrap();
    let page = device_control::parse_search_page(&bytes).unwrap();
    assert_eq!(page.request_id, "req-search-5678");
    assert_eq!(page.stations.len(), 1);
    assert_eq!(page.stations[0].id, "station.alternative");
}

#[test]
fn station_play_station_envelope_construction() {
    let envelope = device_control::station_play_station(
        "10000000-0000-4000-8000-000000000010",
        "2026-09-11T12:00:00Z",
        "70000000-0000-4000-8000-000000000001",
        "40000000-0000-4000-8000-000000000001",
        "station.rock_classic",
    );

    assert_eq!(envelope["protocol_version"], 1);
    assert_eq!(envelope["type"], "device.command");
    assert_eq!(
        envelope["message_id"],
        "10000000-0000-4000-8000-000000000010"
    );
    assert_eq!(envelope["sent_at"], "2026-09-11T12:00:00Z");
    assert_eq!(
        envelope["payload"]["command_id"],
        "70000000-0000-4000-8000-000000000001"
    );
    assert_eq!(
        envelope["payload"]["target"]["device_id"],
        "40000000-0000-4000-8000-000000000001"
    );
    assert_eq!(envelope["payload"]["body"]["name"], "station.play_station");
    assert_eq!(
        envelope["payload"]["body"]["station_id"],
        "station.rock_classic"
    );
}

#[test]
fn catalog_load_refreshes_expired_session_token_exactly_once() {
    use device_control::catalog::should_retry_catalog_with_fresh_token;
    // A 401 from an expired access token triggers exactly one refresh+retry.
    assert!(should_retry_catalog_with_fresh_token(401, false));
    assert!(!should_retry_catalog_with_fresh_token(401, true));
    // Every other status is terminal: success, forbidden identity,
    // server errors, transport-level failures reported as status 0.
    for status in [200u16, 403, 404, 429, 500, 502, 0] {
        assert!(
            !should_retry_catalog_with_fresh_token(status, false),
            "status {status}"
        );
    }
}

#[test]
fn station_play_command_result_resolution_variants() {
    let received = device_control::parse_frame(&fixture("station-command-received-server.json")).unwrap();
    let accepted = device_control::parse_frame(&fixture("station-command-accepted-rockcast.json")).unwrap();
    let succeeded = device_control::parse_frame(&fixture("station-command-succeeded-rockcast.json")).unwrap();
    let failed = device_control::parse_frame(&fixture("station-command-failed-rockcast.json")).unwrap();

    // Intermediate acknowledgements must not resolve a pending play request.
    assert_eq!(received.kind, "command.received");
    assert_eq!(accepted.kind, "command.accepted");
    assert!(device_control::command_result_outcome(&received).is_none());
    assert!(device_control::command_result_outcome(&accepted).is_none());

    // A successful result resolves its command_id as success.
    let (id, ok) = device_control::command_result_outcome(&succeeded).unwrap();
    assert_eq!(id, "60000000-0000-4000-8000-000000000002");
    assert!(ok);

    // A failed result (e.g. station unavailable) resolves as failure,
    // carrying the same command_id the device compares against its pending one.
    let (id, ok) = device_control::command_result_outcome(&failed).unwrap();
    assert_eq!(id, "60000000-0000-4000-8000-000000000003");
    assert!(!ok);

    // A duplicate/late result for an already resolved command parses the same
    // way; the caller ignores it because the pending slot was already cleared.
    let duplicate =
        device_control::parse_frame(&fixture("duplicate-command-result-server.json")).unwrap();
    assert_eq!(
        device_control::command_result_outcome(&duplicate),
        Some(("60000000-0000-4000-8000-000000000002", true))
    );
}

#[test]
fn catalog_browse_and_search_pages_cover_load_variants() {
    // Canonical RockServer catalog response parses with its pagination cursor.
    let canonical = device_control::parse_catalog_page(&fixture("device-catalog-response.json")).unwrap();
    assert_eq!(canonical.stations.len(), 3);
    assert_eq!(canonical.stations[0].id, "station-rock-001");
    assert_eq!(canonical.stations[0].codec.as_deref(), Some("AAC"));
    assert_eq!(canonical.next_cursor.as_deref(), Some("station-news-001"));

    // Last browse page: no next_cursor at all.
    let last_page = json!({
        "request_id": "req-last",
        "stations": [{"id": "station.last", "name": "Last Station", "health": "healthy"}]
    });
    let page =
        device_control::parse_catalog_page(&serde_json::to_vec(&last_page).unwrap()).unwrap();
    assert_eq!(page.next_cursor, None);

    // Empty search result parses to zero stations (GUI shows "Ничего не найдено").
    let empty = json!({"request_id": "req-empty", "stations": []});
    let search =
        device_control::parse_search_page(&serde_json::to_vec(&empty).unwrap()).unwrap();
    assert!(search.stations.is_empty());

    // Search page with unknown extra server fields is tolerated.
    let extra = json!({
        "request_id": "req-extra",
        "extra_field": true,
        "stations": [{"id": "station.hit", "name": "Hits", "health": "healthy", "rank": 7}]
    });
    let search =
        device_control::parse_search_page(&serde_json::to_vec(&extra).unwrap()).unwrap();
    assert_eq!(search.stations[0].id, "station.hit");
}

#[test]
fn catalog_limits_and_utf8_query_are_enforced() {
    use device_control::catalog::*;
    let station = json!({"id":"x", "name":"Радио", "health":"healthy"});
    let page = |station: serde_json::Value| json!({"request_id":"r", "stations":[station]});
    let parses =
        |value: &serde_json::Value| parse_catalog_page(&serde_json::to_vec(value).unwrap()).is_ok();
    assert!(parses(&page(station.clone())));
    for (field, good, bad) in [
        ("id", json!("x".repeat(128)), json!("x".repeat(129))),
        ("name", json!("x".repeat(128)), json!("x".repeat(129))),
        (
            "homepage_url",
            json!("x".repeat(512)),
            json!("x".repeat(513)),
        ),
        ("codec", json!("x".repeat(32)), json!("x".repeat(33))),
        ("health", json!("unknown"), json!("bad")),
        ("bitrate_kbps", json!(1), json!(0)),
    ] {
        let mut valid = station.clone();
        valid[field] = good;
        assert!(parses(&page(valid)), "valid {field}");
        let mut invalid = station.clone();
        invalid[field] = bad;
        assert!(!parses(&page(invalid)), "invalid {field}");
    }
    // Over-tag tolerance (2026-10-13): the browser-side catalog has no tag
    // cap, so extra tags are truncated and over-long tags dropped instead of
    // failing the page.
    {
        let mut tolerant = station.clone();
        let mut many_tags = vec![json!("x".repeat(200))];
        many_tags.extend(vec![json!("t"); 25]);
        tolerant["tags"] = json!(many_tags);
        let parsed =
            parse_catalog_page(&serde_json::to_vec(&page(tolerant)).unwrap()).unwrap();
        assert_eq!(parsed.stations[0].tags.len(), 9);
        assert_eq!(parsed.stations[0].tags[0], "t");
    }
    for field in ["id", "name", "health"] {
        let mut invalid = station.clone();
        invalid[field] = json!("a\u{0}b");
        assert!(!parses(&page(invalid)));
    }
    let mut valid = json!({"request_id":"x".repeat(64), "stations":vec![station.clone();20], "next_cursor":"x".repeat(512)});
    assert!(parses(&valid));
    valid["next_cursor"] = json!("x".repeat(513));
    assert!(!parses(&valid));
    valid["next_cursor"] = json!(null);
    valid["request_id"] = json!("x".repeat(65));
    assert!(!parses(&valid));
    assert_eq!(
        parse_search_page(&vec![b' '; MAX_CATALOG_BYTES + 1]),
        Err(CatalogError::Oversized)
    );
    assert_eq!(
        parse_catalog_page(&vec![b' '; MAX_CATALOG_BYTES + 1]),
        Err(CatalogError::Oversized)
    );
    assert!(
        parse_search_page(
            &serde_json::to_vec(&json!({"request_id":"r", "stations":vec![station;21]})).unwrap()
        )
        .is_err()
    );
    assert!(parse_catalog_page(b"{").is_err());
    assert!(catalog_path(None).unwrap().ends_with("limit=20"));
    assert!(
        catalog_path(Some(" рок & jazz? "))
            .unwrap()
            .ends_with("q=%D1%80%D0%BE%D0%BA%20%26%20jazz%3F")
    );
    assert!(catalog_path(Some(&"я".repeat(128))).is_ok());
    assert!(catalog_path(Some(&"я".repeat(129))).is_err());
    assert!(catalog_path(Some(" \t")).is_err());
    assert!(catalog_path(Some("a\nb")).is_err());
    assert_eq!(
        station_stream_path("джаз/24").unwrap(),
        "/api/v1/stations/%D0%B4%D0%B6%D0%B0%D0%B7%2F24/stream"
    );
    assert!(station_stream_path("").is_err());
    assert!(station_stream_path("a\nb").is_err());
    assert!(station_stream_path("..").is_err());
}

#[test]
fn re17_cast_proto_roundtrip_and_framing() {
    use rock_esp32::device_control::cast_proto::*;
    let msg = CastMessage::string(
        "sender-0",
        "receiver-0",
        "urn:x-cast:com.google.cast.tp.connection",
        r#"{"type":"CONNECT"}"#,
    );
    let framed = msg.encode_framed();
    assert!(framed.len() > 4);
    let payload_len = u32::from_be_bytes(framed[0..4].try_into().unwrap()) as usize;
    assert_eq!(payload_len, framed.len() - 4);

    let decoded = CastMessage::decode(&framed[4..]).unwrap();
    assert_eq!(decoded.source_id, "sender-0");
    assert_eq!(decoded.destination_id, "receiver-0");
    assert_eq!(decoded.namespace, "urn:x-cast:com.google.cast.tp.connection");
    assert_eq!(
        decoded.payload,
        Payload::String(r#"{"type":"CONNECT"}"#.to_string())
    );

    let challenge = encode_auth_challenge();
    assert_eq!(challenge, vec![0x0a, 0x00]);
}

#[test]
fn re17_receiver_cache_ttl_and_sanitization() {
    use rock_esp32::device_control::*;
    let mut cache = ReceiverCache::new();
    let devices = vec![
        DiscoveredReceiver {
            key: "uuid-1".into(),
            name: "Kitchen \u{0007}Speaker".into(),
            model: "Audio".into(),
            host: "192.168.1.50".into(),
            port: 8009,
        },
        DiscoveredReceiver {
            key: "uuid-2".into(),
            name: "   ".into(),
            model: "CC".into(),
            host: "192.168.1.51".into(),
            port: 8009,
        },
    ];
    let list = cache.replace(devices, 100);
    assert_eq!(list.len(), 2);
    assert_eq!(list[0].display_name, "Chromecast");
    assert_eq!(list[1].display_name, "Kitchen Speaker");

    assert_eq!(safe_display_name(""), "Chromecast");
    assert_eq!(safe_display_name("Room 1\0\r\n"), "Room 1");

    assert!(cache.get_fresh(&list[0].receiver_id, 130).is_some());
    assert!(cache.get_fresh(&list[0].receiver_id, 161).is_none());
}

#[test]
fn re17_full_state_snapshot_with_output_conforms_to_contract() {
    use rock_esp32::device_control::*;
    let mut player = PlayerState::new();
    player.apply_play_stream("station.rock");
    player.set_playing();
    player.apply_volume(VolumeAction::SetLevel(75));

    let display = DisplayState::new();

    let local_snap = full_state_snapshot_with_output(&player, &display, &OutputMode::Local);
    assert_eq!(local_snap["playback"]["status"], "playing");
    assert_eq!(local_snap["playback"]["station_id"], "station.rock");
    assert_eq!(local_snap["volume"]["level"], 75);
    assert_eq!(local_snap["output"]["mode"], "local");
    assert!(local_snap["output"].get("receiver_id").is_none());

    let cast_mode = OutputMode::Chromecast {
        receiver_id: "rc-0001".into(),
        receiver_name: "Living Room TV".into(),
    };
    let cast_snap = full_state_snapshot_with_output(&player, &display, &cast_mode);
    assert_eq!(cast_snap["output"]["mode"], "chromecast");
    assert_eq!(cast_snap["output"]["receiver_id"], "rc-0001");
}

#[test]
fn re17_protocol_message_constructors_and_responses() {
    use rock_esp32::device_control::*;
    let conn = msg_connect("receiver-0");
    assert_eq!(conn.destination_id, "receiver-0");
    assert_eq!(conn.namespace, NS_CONNECTION);

    let launch = msg_launch_dmr(1);
    assert_eq!(launch.namespace, NS_RECEIVER);
    assert!(match &launch.payload {
        cast_proto::Payload::String(s) => s.contains(DEFAULT_MEDIA_RECEIVER),
        _ => false,
    });

    let load = msg_load_stream(
        2,
        "transport-1",
        "session-1",
        "http://example.com/live.mp3",
        "Rock FM",
        "audio/wav",
    );
    assert_eq!(load.namespace, NS_MEDIA);
    assert!(match &load.payload {
        cast_proto::Payload::String(s) => {
            s.contains("http://example.com/live.mp3")
                && s.contains("Rock FM")
                && s.contains("\"contentType\":\"audio/wav\"")
        }
        _ => false,
    });

    // rockcast parity: LOAD retries walk the content-type candidate list,
    // derived from the shared source-content-type enum (one mapping point).
    assert_eq!(
        SourceContentType::Wav.cast_load_candidates(),
        ["audio/wav", "audio/mpeg"]
    );
    assert_eq!(
        SourceContentType::Mpeg.cast_load_candidates(),
        ["audio/mpeg", "audio/wav"]
    );
    assert_eq!(SourceContentType::Mpeg.served_mime(), "audio/mpeg");
    assert_eq!(SourceContentType::Aac.served_mime(), "audio/wav");
    let load_mpeg = msg_load_stream(
        4,
        "transport-1",
        "session-1",
        "http://example.com/live.mp3",
        "Rock FM",
        SourceContentType::Wav.cast_load_candidates()[1],
    );
    assert!(match &load_mpeg.payload {
        cast_proto::Payload::String(s) => s.contains("\"contentType\":\"audio/mpeg\""),
        _ => false,
    });

    // Silent-live-stream nudge is a plain media PLAY for the loaded session.
    let nudge = msg_play(5, "transport-1", 7);
    assert_eq!(nudge.namespace, NS_MEDIA);
    assert!(match &nudge.payload {
        cast_proto::Payload::String(s) => {
            s.contains("\"type\":\"PLAY\"") && s.contains("\"mediaSessionId\":7")
        }
        _ => false,
    });

    let vol = msg_set_volume(3, 0.5);
    assert!(match &vol.payload {
        cast_proto::Payload::String(s) => s.contains("0.5"),
        _ => false,
    });

    let ping = msg_ping();
    let pong = msg_pong();
    assert_eq!(ping.namespace, NS_HEARTBEAT);
    assert_eq!(pong.namespace, NS_HEARTBEAT);
}
