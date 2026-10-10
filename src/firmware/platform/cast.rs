//! Cast discovery conversion: raw C scan results (including the staged
//! eureka_info document) into domain receivers.

use super::ffi::*;
use super::cstring;
use serde_json::Value;
use std::ffi::CStr;

// Decode the raw eureka_info document staged by the C subnet scanner
// (cast_bsp.c fetch_eureka_info). Mirrors the former C hand-parsers:
// name; model_name else manufacturer; ssdp_udn else mac_address. Returns
// only the fields present (or none when the document is empty/undecodable);
// callers keep their fallbacks otherwise.
fn eureka_overrides(raw: &[u8]) -> (Option<String>, Option<String>, Option<String>) {
    if raw.is_empty() {
        return (None, None, None);
    }
    let v: Value = match serde_json::from_slice(raw) {
        Ok(v) => v,
        Err(_) => return (None, None, None),
    };
    let field = |keys: &[&str]| -> Option<String> {
        keys.iter()
            .find_map(|k| v.get(k).and_then(|x| x.as_str()))
            .filter(|s| !s.is_empty())
            .map(String::from)
    };
    (
        field(&["name"]),
        field(&["model_name", "manufacturer"]),
        field(&["ssdp_udn", "mac_address"]),
    )
}

pub(crate) fn cast_discover(timeout_ms: u32) -> Vec<crate::device_control::cast::DiscoveredReceiver> {
    // Heap, not stack: 16 widened (eureka_json-bearing) records do not fit
    // the session task's stack — same pattern as the HTTP response buffers.
    let mut raw: Vec<RockCastDeviceRaw> = vec![
        RockCastDeviceRaw {
            id: [0; 65],
            name: [0; 129],
            model: [0; 65],
            host: [0; 65],
            port: 0,
            eureka_json: [0; 1024],
        };
        16
    ];
    let count = unsafe { rock_cast_discover(timeout_ms, raw.as_mut_ptr(), raw.len()) };
    if count <= 0 {
        return Vec::new();
    }
    let count = (count as usize).min(raw.len());
    let mut out = Vec::with_capacity(count);
    for item in &raw[..count] {
        let id = unsafe { CStr::from_ptr(item.id.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let name = unsafe { CStr::from_ptr(item.name.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let model = unsafe { CStr::from_ptr(item.model.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let host = unsafe { CStr::from_ptr(item.host.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let port = item.port;
        let (eureka_name, eureka_model, eureka_id) =
            eureka_overrides(unsafe { CStr::from_ptr(item.eureka_json.as_ptr()) }.to_bytes());
        let name = eureka_name.unwrap_or(name);
        let model = eureka_model.unwrap_or(model);
        let id = eureka_id.unwrap_or(id);
        if !host.is_empty() {
            // Receiver identity policy lives in the domain module
            // (DiscoveredReceiver::from_discovery), not in this FFI layer.
            out.push(
                crate::device_control::cast::DiscoveredReceiver::from_discovery(
                    id, name, model, host, port,
                ),
            );
        }
    }
    out
}

pub(crate) fn cast_log(msg: &str) {
    if let Ok(c) = cstring(msg) {
        unsafe { rock_cast_log(c.as_ptr()) };
    }
}
