# Rust direction

Rust is the preferred application language for this project, but the first firmware stays in C to establish a known-good hardware and ESP-IDF baseline.

Before migrating, verify these items against the versions selected for the project:

1. ESP32-P4 support in the chosen Rust toolchain.
2. Compatibility between that toolchain and the project's ESP-IDF release.
3. A usable binding or integration path for ESP-Hosted and the on-board ESP32-C6.
4. HTTPS, certificate storage, provisioning, OTA, and RockServer protocol requirements.

The likely migration boundary is to retain ESP-IDF as the hardware/network platform and move the application layer to Rust incrementally. The C Hello World should remain available as a small recovery diagnostic until Rust can build, flash, log, connect through ESP-Hosted, and complete the same HTTPS request.
