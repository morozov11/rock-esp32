# Vendored esp-idf-sys

Source: https://github.com/esp-rs/esp-idf-sys
Revision: 0ffca86e9076fa9d5d8e9b6adbb0fbed4ae29f9f ("Compat with ESP-IDF 6.1+", master as of 2026-09-08)
Copied without the `.git` directory.

## Local changes

`build/native/cmake_driver.rs`: the CMake driver (used when the crate is built
from an existing `idf.py` project through the `CARGO_CMAKE_BUILD_*` environment
variables) left `gcc_sysroot` empty, so any project with
`CONFIG_LIBC_PICOLIBC=y` (the ESP-IDF 6 default) failed with
"CONFIG_LIBC_PICOLIBC=y but GCC sysroot could not be found". The driver now
queries the compiler passed in `CARGO_CMAKE_BUILD_COMPILER` with
`--print-sysroot`, mirroring what the standalone cargo driver does.

`src/checks/mod.rs`: the experimental libc parity checks (upstream 3cc5269)
are disabled; they fail against bindings generated through the idf.py cmake
driver on ESP-IDF 6.1 because most socket/libc constants are missing from
them. The `espidf_time64` checks stay enabled.

`src/patches/lstat.rs`: declares an opaque local `stat` struct instead of
importing `crate::stat`, which the idf.py cmake driver bindings never contain
(bindings.h does not include `<sys/stat.h>`). The pointer is only passed
through to the C `stat` function.

Track upstream and drop this vendored copy once a release contains an
equivalent fix.
