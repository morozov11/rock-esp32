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
fn dc018_advertises_only_the_local_display_surface() {
    let fixture = device_control::parse_frame(&fixture("esp32-register-client.json")).unwrap();
    assert_eq!(fixture.kind, "device.register");
    let ours = device_control::registration(
        "10000000-0000-4000-8000-000000000001",
        "2026-09-02T12:00:00Z",
        "0.1.0",
    );
    assert_eq!(
        ours["payload"]["manifest"]["roles"],
        json!(["display_surface"])
    );
    assert_eq!(
        ours["payload"]["manifest"]["capabilities"]["items"],
        json!([{"name":"display.presentation","version":1,"views":["text","now_playing","sensor_grid"],"max_items":8,"max_text_length":128}])
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
