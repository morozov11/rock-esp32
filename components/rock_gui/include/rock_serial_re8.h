#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Handle one CMD:RE8:* serial command (capture, wake control, forensic
 *  dump). Returns true when the command was recognized; a rejection log is
 *  still a recognized command. Log formats are a contract of tools/re8_*.py.
 */
bool re8_serial_handle(const char *cmd);

#ifdef __cplusplus
}
#endif
