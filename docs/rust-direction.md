# Rust direction

The application is a Rust static library built against `esp-idf-sys` and linked into the ESP-IDF 6.1 project. ESP-IDF still owns startup, logging, timing, image generation, and flashing; `main/main.c` is a narrow FFI bridge (`app_main` lives in C, `rust_main` in Rust).

Build chain: `idf.py build` drives `cargo build --target riscv32imafc-esp-espidf -Zbuild-std=std,panic_abort` (nightly toolchain pinned in `rust-toolchain.toml`) through the `main/rust-build.bat` wrapper, then links the resulting static library into the app image. Everything is built and flashed with `idf.py`; no `espup`, `ldproxy`, or `cargo-espflash` is needed.

`esp-idf-sys` is vendored under `third_party/esp-idf-sys` at upstream rev `0ffca86` ("Compat with ESP-IDF 6.1+", master as of 2026-09-08) because the cmake driver it uses when built from `idf.py` does not yet work against ESP-IDF 6.1 out of the box. The local patches (sysroot detection for picolibc, disabled experimental libc checks, opaque `stat` in the lstat patch) are described in `third_party/esp-idf-sys/PROVENANCE.md`. Drop the vendored copy once an upstream release supports the idf.py-driven build on ESP-IDF 6.1.

Notes for this ESP32-P4 v1.3 board:

- current bare-metal `esp-hal` support for ESP32-P4 targets chip revisions v3.x and newer, so ESP-IDF remains the foundation;
- `binstart` is disabled in the `esp-idf-sys` dependency: `app_main` is the C bridge's, and the Rust library exports `rust_main` instead;
- bindgen parses headers with the include directories of the `main` component, so `main/CMakeLists.txt` lists in `PRIV_REQUIRES` every component whose headers the bindings cover;
- ESP-Hosted 3.0.7 and `esp_websocket_client` are now integrated through the C
  platform facade and listed in `PRIV_REQUIRES`; keep protocol/state ownership in
  Rust and do not introduce a second Rust TLS stack. Revisit the high-level
  `esp-idf-svc` crate only when its ESP-IDF 6.1 support makes this boundary
  materially simpler;
- the display GUI stack is confirmed (owner decision, 2026-09-08): LVGL v9 through the C `esp_lvgl_port` component from the ESP Component Registry. Rust owns protocol, state and presentation mapping; the thin LVGL binding layer is written in this repository rather than depending on the immature third-party v9 binding crates. `esp_lcd` (RGB panel) is initialized on the C side; GT911 touch initialization is DC-018 work and will use the same narrow C facade. Slint (paid embedded license) and embedded-graphics (too low-level, provisioning-screen only) were evaluated and excluded.
