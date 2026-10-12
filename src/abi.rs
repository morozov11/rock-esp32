//! Cross-language ABI constants, generated at build time from
//! `main/rock_protocol_abi.h` and `components/rock_gui/include/rock_gui.h` (see build.rs).
//! Single source of truth shared with the C firmware — never edit values
//! here, change the headers.

include!(concat!(env!("OUT_DIR"), "/rock_abi_consts.rs"));
