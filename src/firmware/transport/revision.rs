//! State revision counter for the device.state-full protocol contract.

use crate::device_control::{DisplayState, PlayerState};

// Invariant: single Rust task. Only the session task (and its synchronous
// GuiEvent handlers, which run inside the same task's drain_gui_events)
// ever writes this counter; the offline loop between sessions only reads
// freshly-reset state. There is no second concurrent writer, so the two
// atomic ops below cannot interleave — they exist to keep every step of
// the read-modify-write atomic (no torn load+store window if a reader is
// ever added) rather than to synchronize two writers.
static STATE_REVISION_COUNTER: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

pub(super) fn next_state_revision(display: &DisplayState, player: &PlayerState) -> u64 {
    let base = (display.revision() + player.revision() - 1) as u32;
    // fetch_max (not fetch_add): the revision must stay >= base after a
    // display/player revision jump forward, while remaining strictly greater
    // than any previously published value — fetch_add alone could re-issue
    // an already published revision while the counter lags behind base.
    let prev = STATE_REVISION_COUNTER.fetch_max(base, core::sync::atomic::Ordering::Relaxed);
    let rev = if prev == 0 { base } else { base.max(prev + 1) };
    STATE_REVISION_COUNTER.fetch_max(rev, core::sync::atomic::Ordering::Relaxed);
    rev as u64
}

pub(super) fn current_revision(display: &DisplayState, player: &PlayerState) -> u64 {
    let cur = STATE_REVISION_COUNTER.load(core::sync::atomic::Ordering::Relaxed);
    if cur == 0 {
        display.revision() + player.revision() - 1
    } else {
        cur as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn revision_increments_monotonically() {
        let display = DisplayState::new();
        let player = PlayerState::new();
        let r1 = next_state_revision(&display, &player);
        let r2 = next_state_revision(&display, &player);
        assert!(r2 > r1);
        assert_eq!(current_revision(&display, &player), r2);
    }
}
