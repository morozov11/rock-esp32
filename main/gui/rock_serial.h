#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the serial diagnostics console task (USB-JTAG, Core 0): parses the
 * CMD: protocol and dispatches to the GUI, player and platform. The wire
 * format is a contract of the tools python scripts — do not change commands
 * or response lines. Called by rock_gui_start() once the GUI widgets exist.
 */
int rock_serial_start(void);

#ifdef __cplusplus
}
#endif
