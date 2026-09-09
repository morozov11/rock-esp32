//! Firmware composition root: initializes ESP-IDF then owns reconnect policy.

mod pairing;
mod platform;
mod transport;

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {
    esp_idf_sys::link_patches();
    esp_idf_sys::restore_posix_stdio_fds();
    if !platform::init() {
        loop {
            platform::delay(5_000)
        }
    }
    let mut failures = 0u32;
    loop {
        if let Ok(credentials) = pairing::credentials() {
            let _ = transport::run(&credentials.access_token);
        }
        failures = failures.saturating_add(1).min(5);
        if failures == 5 {
            platform::recover()
        }
        let ceiling = (1u32 << failures).min(30) * 1_000;
        let mut random = [0u8; 4];
        platform::random_fill(&mut random);
        platform::delay(u32::from_le_bytes(random) % ceiling.max(1));
    }
}
