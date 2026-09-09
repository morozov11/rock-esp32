use serde_json::{json, Value};
use super::protocol::{Envelope, DISPLAY_SURFACE_ID};

pub const DISPLAY_MAX_ITEMS: usize = 8;
pub const DISPLAY_MAX_TEXT_LENGTH: usize = 128;

#[derive(Clone, Debug, PartialEq)]
pub enum DisplayView {
    Text(String),
    NowPlaying {
        station_id: String,
        title: String,
        subtitle: String,
    },
    SensorGrid {
        title: String,
        items: Vec<String>,
    },
}

impl DisplayView {
    pub fn name(&self) -> &'static str {
        match self {
            Self::Text(_) => "text",
            Self::NowPlaying { .. } => "now_playing",
            Self::SensorGrid { .. } => "sensor_grid",
        }
    }
}

#[derive(Debug, PartialEq)]
pub struct DisplayCommand {
    pub command_id: String,
    pub view: DisplayView,
}

fn bounded(value: &str, min: usize, max: usize) -> bool {
    let n = value.chars().count();
    (min..=max).contains(&n)
}

fn string_field<'a>(
    body: &'a serde_json::Map<String, Value>,
    name: &str,
    min: usize,
    max: usize,
) -> Result<&'a str, &'static str> {
    let value = body
        .get(name)
        .and_then(Value::as_str)
        .ok_or("invalid_payload")?;
    if bounded(value, min, max) {
        Ok(value)
    } else {
        Err("invalid_payload")
    }
}

/// Parses only the local display capability advertised by this firmware.
pub fn display_command(frame: &Envelope) -> Result<DisplayCommand, &'static str> {
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
    let surface = frame
        .payload
        .get("target")
        .and_then(Value::as_object)
        .and_then(|target| target.get("surface_id"))
        .and_then(Value::as_str);
    if surface != Some(DISPLAY_SURFACE_ID) {
        return Err("capability_not_supported");
    }
    let body = frame
        .payload
        .get("body")
        .and_then(Value::as_object)
        .ok_or("invalid_payload")?;
    let view = match body.get("name").and_then(Value::as_str) {
        Some("display.show_text") => {
            DisplayView::Text(string_field(body, "text", 1, DISPLAY_MAX_TEXT_LENGTH)?.into())
        }
        Some("display.show_view") => match body.get("view").and_then(Value::as_str) {
            Some("now_playing") => DisplayView::NowPlaying {
                station_id: string_field(body, "station_id", 1, 128)?.into(),
                title: string_field(body, "title", 1, DISPLAY_MAX_TEXT_LENGTH)?.into(),
                subtitle: {
                    let subtitle = match body.get("subtitle") {
                        Some(value) => Some(value.as_str().ok_or("invalid_payload")?),
                        None => None,
                    };
                    if subtitle.is_some_and(|s| !bounded(s, 0, DISPLAY_MAX_TEXT_LENGTH)) {
                        return Err("invalid_payload");
                    }
                    subtitle.unwrap_or("").into()
                },
            },
            Some("sensor_grid") => {
                let title = string_field(body, "title", 1, DISPLAY_MAX_TEXT_LENGTH)?.into();
                let cards = body
                    .get("items")
                    .and_then(Value::as_array)
                    .ok_or("invalid_payload")?;
                if cards.len() > DISPLAY_MAX_ITEMS {
                    return Err("invalid_payload");
                }
                let mut items = Vec::with_capacity(cards.len());
                for card in cards {
                    let card = card.as_object().ok_or("invalid_payload")?;
                    string_field(card, "entity_id", 3, 128)?;
                    let label = string_field(card, "label", 1, 64)?;
                    let value = card.get("value").ok_or("invalid_payload")?.to_string();
                    let unit = card.get("unit").and_then(Value::as_str).unwrap_or("");
                    let freshness = string_field(card, "freshness", 1, 16)?;
                    let quality = string_field(card, "quality", 1, 16)?;
                    if !bounded(unit, 0, 16)
                        || !matches!(freshness, "fresh" | "stale" | "unknown")
                        || !matches!(quality, "ok" | "degraded" | "unavailable" | "unknown")
                        || value.len() > 64
                    {
                        return Err("invalid_payload");
                    }
                    items.push(format!(
                        "{}: {} {} ({})",
                        label,
                        value.trim_matches('"'),
                        unit,
                        freshness
                    ));
                }
                DisplayView::SensorGrid { title, items }
            }
            _ => return Err("capability_not_supported"),
        },
        _ => return Err("capability_not_supported"),
    };
    Ok(DisplayCommand {
        command_id: command_id.into(),
        view,
    })
}

#[derive(Debug)]
pub struct DisplayState {
    view: Option<DisplayView>,
    revision: u64,
}

impl DisplayState {
    pub fn new() -> Self {
        Self {
            view: None,
            revision: 1,
        }
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn view_name(&self) -> &'static str {
        self.view
            .as_ref()
            .map(DisplayView::name)
            .unwrap_or("dismissed")
    }
    /// An identical replay deliberately does not redraw or create a new state revision.
    pub fn apply(&mut self, view: DisplayView) -> bool {
        if self.view.as_ref() == Some(&view) {
            return false;
        }
        self.view = Some(view);
        self.revision += 1;
        true
    }
}

pub fn display_state(state: &DisplayState) -> Value {
    json!({"display": {"surface_id": DISPLAY_SURFACE_ID, "view": state.view_name()}})
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
            payload: json!({"command_id":"60000000-0000-4000-8000-000000000001", "target":{"surface_id":"display.main"}, "body":body}),
        }
    }

    #[test]
    fn display_profile_is_bounded_and_idempotent() {
        let view = display_command(&command(json!({"name":"display.show_view","view":"sensor_grid","title":"Kitchen","items":[{"entity_id":"sensor.temperature","label":"Temperature","value":21.5,"unit":"\u{00B0}C","quality":"ok","freshness":"fresh"}]}))).unwrap().view;
        let mut state = DisplayState::new();
        assert!(state.apply(view.clone()));
        assert!(!state.apply(view));
        assert_eq!(state.revision(), 2);
        assert_eq!(state.view_name(), "sensor_grid");
    }

    #[test]
    fn display_profile_rejects_other_surfaces_and_views() {
        let mut frame = command(json!({"name":"display.show_view","view":"unknown"}));
        assert_eq!(display_command(&frame), Err("capability_not_supported"));
        frame.payload["target"]["surface_id"] = json!("display.secondary");
        assert_eq!(display_command(&frame), Err("capability_not_supported"));
    }
}
