unsafe extern "C" {
    fn rock_delay_ms(milliseconds: u32);
    fn rock_log_heap(bytes: usize);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {
    esp_idf_sys::link_patches();
    // Bind stdin/stdout/stderr to their POSIX fds so `println!` works; this
    // normally happens in esp-idf-sys' own app_main, which we do not use
    // because app_main lives in the C bridge.
    esp_idf_sys::restore_posix_stdio_fds();

    loop {
        unsafe {
            rock_log_heap(esp_idf_sys::esp_get_free_heap_size() as usize);
            rock_delay_ms(1_000);
        }
    }
}
