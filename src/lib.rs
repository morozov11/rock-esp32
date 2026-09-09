pub mod device_control;

#[cfg(target_os = "espidf")]
mod firmware;

#[cfg(target_os = "espidf")]
pub use firmware::rust_main;
