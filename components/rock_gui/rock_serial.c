// Serial diagnostics console: reads CMD: lines from the USB-JTAG port and
// dispatches them to the GUI, player and platform. Extracted from rock_gui.c
// (phase 3) so the GUI module owns no I/O transport. The command set and the
// exact log/response formats are a contract of the tools python scripts —
// keep verbatim.

#include "rock_serial.h"
#include "control_platform.h"
#include "rock_player.h"
#include "rock_gui.h"
#include "rock_screenshot.h"
#include "audio_spectrum.h"

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "rock-serial";

static void serial_cmd_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t jtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&jtag_cfg);
    bool jtag_installed = (err == ESP_OK || err == ESP_ERR_INVALID_STATE);

    char cmd[544];
    size_t cmd_idx = 0;

    ESP_LOGI(TAG, "Serial command listener active (USB-JTAG: %d)", (int)jtag_installed);

    while (1) {

        uint8_t ch = 0;
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(50));
        if (n > 0) {
            if (ch == '\n' || ch == '\r') {
                if (cmd_idx > 0) {
                    cmd[cmd_idx] = '\0';
                    ESP_LOGI(TAG, "Serial command received: '%s'", cmd);
                    if (strcmp(cmd, "CMD:SELFTEST") == 0) {
#if CONFIG_ROCK_BRINGUP_TESTS
                        static bool ran;
                        char state[16];
                        rock_player_get_state(state, sizeof(state), NULL, 0, NULL, NULL);
                        if (!ran && (!strcmp(state, "idle") || !strcmp(state, "stopped"))) {
                            ran = rock_player_bringup_start() == 0;
                        }
#else
                        ESP_LOGI(TAG, "SELFTEST disabled (build with CONFIG_ROCK_BRINGUP_TESTS=y)");
#endif
                    } else if (!strncmp(cmd, "CMD:UI:", 7)) {
                        rock_gui_trigger(cmd + 7);
                    } else if (strcmp(cmd, "CMD:METRICS") == 0) {
                        uint32_t underruns, errors, reconnects;
                        rock_player_get_stats(&underruns, &errors, &reconnects);
                        uint32_t fps = 0;
                        rock_gui_get_metrics(&fps, NULL);
                        wifi_ps_type_t ps_now = WIFI_PS_NONE;
                        esp_err_t ps_err = esp_wifi_get_ps(&ps_now);
                        ESP_LOGI(TAG, "metrics heap=%u internal=%u underruns=%lu decode_errors=%lu reconnects=%lu tasks=%u timer_hz=%lu rssi=%d ch=%d ps=%d ps_err=%d",
                            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                            (unsigned long)underruns, (unsigned long)errors, (unsigned long)reconnects,
                            (unsigned)uxTaskGetNumberOfTasks(), (unsigned long)fps, rock_wifi_rssi(),
                            rock_wifi_channel(), (int)ps_now, (int)ps_err);
                        rock_player_log_stream_metrics();
                        rock_spectrum_log_metrics();
                        rock_gui_log_debug_state();
                    } else if (strcmp(cmd, "CMD:SCREENSHOT") == 0) {
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:NP") == 0) {
                        rock_gui_switch_view(0);
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:BROWSE") == 0) {
                        rock_gui_switch_view(1);
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:SEARCH") == 0) {
                        rock_gui_switch_view(2);
                        rock_gui_dump_screenshot();
                    }
                    cmd_idx = 0;
                }
            } else if (cmd_idx + 1 < sizeof(cmd)) {
                cmd[cmd_idx++] = (char)ch;
            }
        }
    }
}

int rock_serial_start(void)
{
    if (xTaskCreatePinnedToCore(serial_cmd_task, "serial_cmd", 32768, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial command task");
        return -1;
    }
    return 0;
}
