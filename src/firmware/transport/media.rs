//! Audio stream source lifecycle, local & Cast playback coordination, and player status synchronization.

use super::catalog::resolve_station_name;
use crate::device_control::{self, PlaybackStatus, PlayerState, VolumeAction};
use crate::firmware::cast_client::CastClient;
use crate::firmware::pairing;
use crate::firmware::platform;

// Unified stream source: the board keeps ONE authorized connection to the
// RockServer station proxy; the speaker and the Cast relay consume the same
// ring, so switching outputs never reopens the upstream.
pub(super) struct SourceState {
    pub(super) station: Option<String>,
}

pub(super) fn station_proxy_url(station_id: &str) -> Result<String, ()> {
    let path = device_control::station_stream_path(station_id).map_err(|_| ())?;
    Ok(format!("{}{}", platform::server_base_url(), path))
}

// Idempotent per station: when the source already serves this station, keep
// it (and its ring) alive for gapless output switching.
pub(super) fn ensure_source(
    station_id: &str,
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    if source.station.as_deref() == Some(station_id) && platform::source_is_active() {
        // Swallowed-error policy (phase 5): a failed bearer refresh would
        // leave reconnects on the stale token — log it, keep the session.
        if !platform::source_update_bearer(token) {
            platform::log_msg("Source bearer refresh failed on active source");
        }
        return Ok(());
    }
    platform::source_stop();
    *token = pairing::cached_access_token(60_000)?;
    let url = station_proxy_url(station_id)?;
    // Confidentiality: the proxy URL and bearer never appear in logs.
    if !platform::source_start(&url, token) {
        pairing::invalidate_session_cache();
        if let Ok(refreshed) = pairing::cached_access_token(60_000) {
            *token = refreshed;
            if !platform::source_start(&url, token) {
                source.station = None;
                return Err(());
            }
        } else {
            source.station = None;
            return Err(());
        }
    }
    source.station = Some(station_id.to_string());
    Ok(())
}

pub(super) fn play_local_station(
    station_id: &str,
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    ensure_source(station_id, token, source)?;
    let url = station_proxy_url(station_id)?;
    platform::player_play_stream(&url, station_id, token)
}

pub(super) fn cast_play_stream(
    cast_client: &mut CastClient,
    station_id: &str,
    meta_title: &str,
    token: &mut String,
    source: &mut SourceState,
) -> Result<(), ()> {
    // Detach the speaker tap only: the shared source keeps running. A failed
    // detach would leave local audio playing over the Cast handoff — log it.
    if platform::player_stop().is_err() {
        platform::log_msg("Speaker detach failed before Cast playback");
    }
    ensure_source(station_id, token, source)?;
    let relay_url = platform::relay_listen().ok_or(())?;
    let content_type = platform::source_content_type_enum();
    let resolved_station = resolve_station_name(station_id);
    let title = if !meta_title.is_empty() {
        meta_title
    } else {
        &resolved_station
    };
    let res = cast_client.load_stream(&relay_url, title, content_type);
    if res.is_err() {
        return Err(());
    }
    Ok(())
}

/// Synchronize only observed hardware state, including after an offline interval.
pub(crate) fn refresh_player(player: &mut PlayerState) -> bool {
    let (_, title) = platform::source_get_meta_title();
    refresh_player_with_cast(player, false, &title)
}

pub(crate) fn refresh_player_with_cast(
    player: &mut PlayerState,
    is_casting: bool,
    meta_title: &str,
) -> bool {
    let Some((status, station, volume, muted)) = platform::player_get_full_state() else {
        return false;
    };
    let before = player.revision();
    if !is_casting {
        if !station.is_empty() && player.station_id() != Some(station.as_str()) {
            player.apply_play_stream(&station);
        }
        match status {
            PlaybackStatus::Buffering => {
                player.apply_play_stream(&station);
            }
            PlaybackStatus::Playing => {
                player.set_playing();
            }
            PlaybackStatus::Paused => {
                player.set_paused();
            }
            PlaybackStatus::Stopped => {
                player.set_stopped();
            }
            PlaybackStatus::Error => {
                player.set_error();
            }
            PlaybackStatus::Idle => {}
        }
    }
    player.apply_volume(VolumeAction::SetLevel(volume));
    player.apply_volume(VolumeAction::SetMute(muted));
    if player.revision() != before {
        update_gui(player, meta_title);
        return true;
    }
    false
}

pub(super) fn update_gui(player: &PlayerState, meta_title: &str) {
    let code = match player.status() {
        PlaybackStatus::Idle => 0,
        PlaybackStatus::Buffering => 1,
        PlaybackStatus::Playing => 2,
        PlaybackStatus::Paused => 3,
        PlaybackStatus::Stopped => 4,
        PlaybackStatus::Error => 5,
    };
    let station_id = player.station_id().unwrap_or("");
    let station_name = if !station_id.is_empty() {
        resolve_station_name(station_id)
    } else {
        String::new()
    };
    platform::gui_update_playback(
        code,
        station_id,
        &station_name,
        meta_title,
        "",
        player.volume_level(),
        player.volume_muted(),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn station_proxy_url_valid() {
        assert!(station_proxy_url("station.rock_classic").is_ok());
        assert!(station_proxy_url("").is_err());
    }
}
