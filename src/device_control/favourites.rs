//! Strict, RAM-only projection of the account sync response.

use serde::Deserialize;
use std::collections::HashSet;

#[derive(Clone, Debug, Deserialize)]
pub(crate) struct Favourite {
    pub(crate) record_id: String,
    pub(crate) station_id: Option<String>,
    pub(crate) added_at: String,
    #[serde(default)]
    pub(crate) deleted_at: Option<String>,
}

#[derive(Deserialize)]
struct SyncResponse {
    server_revision: u64,
    favourites: FavouriteCollection,
}

#[derive(Deserialize)]
struct FavouriteCollection {
    records: Vec<Favourite>,
}

pub(crate) fn apply_response(
    current: &[Favourite],
    since_revision: u64,
    bytes: &[u8],
) -> Result<(u64, Vec<Favourite>), ()> {
    let response = serde_json::from_slice::<SyncResponse>(bytes).map_err(|_| ())?;
    if response.server_revision < since_revision {
        return Err(());
    }
    let mut records = if since_revision == 0 {
        Vec::new()
    } else {
        current.to_vec()
    };
    for record in response.favourites.records {
        records.retain(|old| old.record_id != record.record_id);
        if record.deleted_at.is_none()
            && record
                .station_id
                .as_ref()
                .is_some_and(|id| !id.is_empty() && id.len() <= 128)
        {
            records.push(record);
        }
    }
    if records.len() > 500 {
        return Err(());
    }
    records.sort_by(|a, b| {
        a.added_at
            .cmp(&b.added_at)
            .then_with(|| a.station_id.cmp(&b.station_id))
    });
    let mut seen = HashSet::new();
    records.retain(|r| seen.insert(r.station_id.clone()));
    Ok((response.server_revision, records))
}

pub(crate) fn display_name(name: &str) -> Option<&str> {
    if name.is_empty() {
        return None;
    }
    let mut end = name.len().min(128);
    while !name.is_char_boundary(end) {
        end -= 1;
    }
    Some(&name[..end])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn snapshot_excludes_tombstones_deduplicates_and_orders_stations() {
        let data = br#"{"server_revision":7,"favourites":{"records":[{"record_id":"a","station_id":"s2","added_at":"2026-01-02"},{"record_id":"b","deleted_at":"2026-01-03","added_at":"2026-01-01"},{"record_id":"c","station_id":"s1","added_at":"2026-01-01"},{"record_id":"d","station_id":"s1","added_at":"2026-01-04"}]},"history":{"records":[]}}"#;
        let (revision, records) = apply_response(&[], 0, data).unwrap();
        assert_eq!(revision, 7);
        assert_eq!(
            records
                .iter()
                .map(|r| r.station_id.as_deref().unwrap())
                .collect::<Vec<_>>(),
            vec!["s1", "s2"]
        );
        assert_eq!(records[0].record_id, "c");
    }

    #[test]
    fn rejects_more_than_account_limit() {
        let records = (0..501).map(|i| serde_json::json!({"record_id": i.to_string(), "station_id": format!("s{i}"), "added_at": "2026-01-01"})).collect::<Vec<_>>();
        let response = serde_json::json!({"server_revision": 1, "favourites": {"records": records}});
        assert!(apply_response(&[], 0, response.to_string().as_bytes()).is_err());
    }

    #[test]
    fn delta_updates_and_deletes_without_losing_other_stations() {
        let first = br#"{"server_revision":4,"favourites":{"records":[{"record_id":"a","station_id":"s1","added_at":"2026-01-01"},{"record_id":"b","station_id":"s2","added_at":"2026-01-02"}]}}"#;
        let (_, current) = apply_response(&[], 0, first).unwrap();
        let delta = br#"{"server_revision":6,"favourites":{"records":[{"record_id":"a","station_id":"s1","added_at":"2026-01-01","deleted_at":"2026-01-03"},{"record_id":"c","station_id":"s3","added_at":"2026-01-03"}]}}"#;
        let (revision, merged) = apply_response(&current, 4, delta).unwrap();
        assert_eq!(revision, 6);
        assert_eq!(merged.iter().map(|r| r.station_id.as_deref().unwrap()).collect::<Vec<_>>(), vec!["s2", "s3"]);
        assert!(apply_response(&merged, 6, first).is_err());
    }

    #[test]
    fn long_utf8_name_fits_display_without_splitting_character() {
        let name = format!("{}Ж", "a".repeat(127));
        assert_eq!(display_name(&name), Some("a".repeat(127).as_str()));
        assert_eq!(display_name(""), None);
    }
}
