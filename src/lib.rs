/// Generated cross-language constants (phase 5 A3); leaf module usable by
/// both `device_control` and `firmware` on host and espidf builds.
pub mod abi;

pub mod device_control;
#[cfg(target_os = "espidf")]
pub mod presentation;

#[cfg(target_os = "espidf")]
mod firmware;

#[cfg(target_os = "espidf")]
pub use firmware::rust_main;
