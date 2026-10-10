pub mod cast;
pub mod cast_proto;
pub mod catalog;
pub mod dispatch;
pub mod display;
pub mod pairing;
pub mod player;
pub mod protocol;

/// Shared length-bound check for inbound protocol strings (phase 5 dedup:
/// was duplicated in player.rs and display.rs). Counts chars, not bytes.
pub(crate) fn bounded(value: &str, min: usize, max: usize) -> bool {
    let n = value.chars().count();
    (min..=max).contains(&n)
}

pub use cast::*;
pub use cast_proto::*;
pub use catalog::*;
pub use dispatch::*;
pub use display::*;
pub use pairing::*;
pub use player::*;
pub use protocol::*;
