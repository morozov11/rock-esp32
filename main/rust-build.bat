@echo off
rem Wrapper for the cargo invocation driven by main/CMakeLists.txt.
rem The esp-clang-libs package that provides libclang.dll is not on the PATH
rem set by the ESP-IDF activation profile; prepend its bin dir so the DLL
rem loader resolves libclang's sibling DLLs (libLLVM, libc++, ...).
rem RUST_CLANG_BIN, RUST_CARGO_ARGS and CARGO_TARGET_DIR are set by the
rem ExternalProject BUILD_COMMAND in main/CMakeLists.txt.
set "PATH=%RUST_CLANG_BIN%;%PATH%"
cargo build %RUST_CARGO_ARGS%
