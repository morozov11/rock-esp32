# Rust direction

The application is a Rust static library built against `esp-idf-sys` and linked into the ESP-IDF 6.1 project. ESP-IDF still owns startup, logging, timing, image generation, and flashing; `main/main.c` is a narrow FFI bridge (`app_main` lives in C, `rust_main` in Rust).

Build chain: `idf.py build` drives `cargo build --target riscv32imafc-esp-espidf -Zbuild-std=std,panic_abort` (nightly toolchain pinned in `rust-toolchain.toml`) through the `main/rust-build.bat` wrapper, then links the resulting static library into the app image. Everything is built and flashed with `idf.py`; no `espup`, `ldproxy`, or `cargo-espflash` is needed.

`esp-idf-sys` is vendored under `third_party/esp-idf-sys` at upstream rev `0ffca86` ("Compat with ESP-IDF 6.1+", master as of 2026-09-08) because the cmake driver it uses when built from `idf.py` does not yet work against ESP-IDF 6.1 out of the box. The local patches (sysroot detection for picolibc, disabled experimental libc checks, opaque `stat` in the lstat patch) are described in `third_party/esp-idf-sys/PROVENANCE.md`. Drop the vendored copy once an upstream release supports the idf.py-driven build on ESP-IDF 6.1.

Notes for this ESP32-P4 v1.3 board:

- current bare-metal `esp-hal` support for ESP32-P4 targets chip revisions v3.x and newer, so ESP-IDF remains the foundation;
- `binstart` is disabled in the `esp-idf-sys` dependency: `app_main` is the C bridge's, and the Rust library exports `rust_main` instead;
- bindgen parses headers with the include directories of the `main` component, so `main/CMakeLists.txt` lists in `PRIV_REQUIRES` every component whose headers the bindings cover;
- at the ESP-Hosted milestone, add its components to `PRIV_REQUIRES` the same way and revisit the high-level `esp-idf-svc` crate if its ESP-IDF 6.1 support has landed by then.
