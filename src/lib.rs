use core::ffi::c_char;

unsafe extern "C" {
    fn rock_delay_ms(milliseconds: u32);
    fn rock_log_heap(bytes: usize);
    // Clock UI facade from main/display_bsp.c: takes the LVGL port lock
    // internally and returns false when it was not acquired in time.
    fn rock_ui_clock_set_text(text: *const c_char) -> bool;
    // Microseconds since boot from the IDF esp_timer; declared here instead
    // of using std::time so the firmware keeps its std-free link footprint
    // (build-std for espidf drags in fs shims like realpath otherwise).
    fn esp_timer_get_time() -> i64;
}

/// Render whole seconds since boot as `HH:MM:SS\0` (wraps at 24 h) into a
/// fixed buffer; no allocation, suitable for direct FFI handoff.
fn uptime_clock(elapsed_secs: u64) -> [u8; 9] {
    let secs_of_day = (elapsed_secs % 86_400) as u32;
    let (hours, mins, secs) = (
        secs_of_day / 3600,
        (secs_of_day % 3600) / 60,
        secs_of_day % 60,
    );
    // Start from the literal template so the ':' separators can never be
    // forgotten; only the digit positions are overwritten below.
    let mut out = *b"00:00:00\0";
    out[0] = b'0' + (hours / 10) as u8;
    out[1] = b'0' + (hours % 10) as u8;
    out[3] = b'0' + (mins / 10) as u8;
    out[4] = b'0' + (mins % 10) as u8;
    out[6] = b'0' + (secs / 10) as u8;
    out[7] = b'0' + (secs % 10) as u8;
    out
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {
    esp_idf_sys::link_patches();
    // Bind stdin/stdout/stderr to their POSIX fds so `println!` works; this
    // normally happens in esp-idf-sys' own app_main, which we do not use
    // because app_main lives in the C bridge.
    esp_idf_sys::restore_posix_stdio_fds();

    // No Wi-Fi/NTP yet, so the clock starts at 00:00:00 on boot; the
    // display bring-up only proves the LVGL pipeline ticks.
    let mut tick: u32 = 0;
    loop {
        unsafe {
            let secs = (esp_timer_get_time() / 1_000_000) as u64;
            let text = uptime_clock(secs);
            // A dropped frame on a missed lock is fine; the next tick redraws.
            rock_ui_clock_set_text(text.as_ptr() as *const c_char);
            if tick % 5 == 0 {
                rock_log_heap(esp_idf_sys::esp_get_free_heap_size() as usize);
            }
        }
        tick = tick.wrapping_add(1);
        unsafe { rock_delay_ms(200) };
    }
}

#[cfg(test)]
mod tests {
    use super::uptime_clock;

    #[test]
    fn clock_formats_hours_minutes_seconds() {
        assert_eq!(uptime_clock(0), *b"00:00:00\0");
        assert_eq!(uptime_clock(3_723), *b"01:02:03\0");
        assert_eq!(uptime_clock(86_399), *b"23:59:59\0");
    }

    #[test]
    fn clock_wraps_after_a_day() {
        assert_eq!(uptime_clock(86_400), *b"00:00:00\0");
    }
}
