// Serial diagnostics console: reads CMD: lines from the USB-JTAG port and
// dispatches them to the GUI, player and platform. Extracted from rock_gui.c
// (phase 3) so the GUI module owns no I/O transport. The command set and the
// exact log/response formats are a contract of the tools python scripts —
// keep verbatim.

#include "rock_serial.h"
#include "control_platform.h"
#include "rock_player.h"
#include "rock_source.h"
#include "rock_wake_probe.h"
#include "rock_serial_re8.h"
#include "rock_gui.h"
#include "rock_screenshot.h"
#include "audio_spectrum.h"

#include "driver/usb_serial_jtag.h"
#include "eh_host_raw_tp_stats.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-serial";

// Raw transport throughput modes (bit values mirror ESP_TEST_RAW_TP__* in
// esp_hosted's eh_caps.h). The engine measures the SDIO transport alone,
// with WiFi idle, to separate link quality from transport capacity.
#define RAWTP_ESP_TO_HOST  (1 << 1)
#define RAWTP_HOST_TO_ESP  (1 << 2)
#define RAWTP_BIDIRECTIONAL (1 << 3)


// Raw HTTP throughput probe (network path diagnosis): download up to
// 16 MB / 20 s from the given URL and log connect time and sustained speed.
// Runs on the serial task; shares the WiFi link with anything playing.
static void nettest_run(const char *url)
{
    const size_t buf_len = 4096;
    uint8_t *buf = heap_caps_malloc(buf_len, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "nettest: no memory");
        return;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        ESP_LOGE(TAG, "nettest: init failed");
        free(buf);
        return;
    }
    int64_t open_t0 = esp_timer_get_time();
    int open_err = esp_http_client_open(h, 0);
    int64_t open_us = esp_timer_get_time() - open_t0;
    int64_t content_length = open_err >= 0 ? esp_http_client_fetch_headers(h) : -1;
    int status = esp_http_client_get_status_code(h);
    if (open_err < 0 || status != 200) {
        ESP_LOGE(TAG, "nettest: open err=%d status=%d connect=%lld ms",
            open_err, status, (long long)(open_us / 1000));
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        free(buf);
        return;
    }
    int64_t t0 = esp_timer_get_time();
    int64_t bytes = 0;
    int64_t last_log = t0;
    int r;
    while ((r = esp_http_client_read(h, (char *)buf, (int)buf_len)) >= 0) {
        int64_t now = esp_timer_get_time();
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(h)) {
                break;
            }
            if (now - t0 > 20000000) {
                break;
            }
            continue;
        }
        bytes += r;
        now = esp_timer_get_time();
        if (now - last_log >= 2000000) {
            int64_t ms = (now - t0) / 1000;
            ESP_LOGI(TAG, "nettest: %lld KB in %lld ms (%lld KB/s)",
                (long long)(bytes / 1024), (long long)ms,
                (long long)(ms > 0 ? bytes / 1024 * 1000 / ms : 0));
            last_log = now;
        }
        if (bytes >= 16 * 1024 * 1024 || now - t0 > 20000000) {
            break;
        }
    }
    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "nettest: done url=%s status=%ld len=%lld got=%lld KB in %lld ms avg=%lld KB/s connect=%lld ms",
        url, (long)status, (long long)content_length, (long long)(bytes / 1024),
        (long long)total_ms, (long long)(total_ms > 0 ? bytes / 1024 * 1000 / total_ms : 0),
        (long long)(open_us / 1000));
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    free(buf);
}

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
                    } else if (!strncmp(cmd, "CMD:RE8:", 8)) {
                        re8_serial_handle(cmd);
                    } else if (!strncmp(cmd, "CMD:NETTEST:", 12)) {
                        nettest_run(cmd + 12);
                    } else if (!strcmp(cmd, "CMD:RAWTP:RX")) {
                        ESP_LOGI(TAG, "rawtp: starting ESP->host (SDIO RX) test");
                        int rc = eh_host_raw_tp_start(RAWTP_ESP_TO_HOST);
                        ESP_LOGI(TAG, "rawtp: start rc=%d (logs as 'eh_raw_tp: N-M sec')", rc);
                    } else if (!strcmp(cmd, "CMD:RAWTP:TX")) {
                        ESP_LOGI(TAG, "rawtp: starting host->ESP (SDIO TX) test");
                        int rc = eh_host_raw_tp_start(RAWTP_HOST_TO_ESP);
                        ESP_LOGI(TAG, "rawtp: start rc=%d", rc);
                    } else if (!strcmp(cmd, "CMD:RAWTP:STOP")) {
                        ESP_LOGI(TAG, "rawtp: stop rc=%d", eh_host_raw_tp_stop());
                    } else if (!strcmp(cmd, "CMD:WIFIOPT")) {
                        uint8_t proto = 0;
                        esp_wifi_get_protocol(WIFI_IF_STA, &proto);
                        int8_t txp = 0;
                        esp_wifi_get_max_tx_power(&txp);
                        ESP_LOGI(TAG, "wifiopt: before proto=0x%x txp=%d (0.25dBm)",
                            (unsigned)proto, (int)txp);
                        esp_err_t e1 = esp_wifi_set_protocol(WIFI_IF_STA,
                            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
                        esp_err_t e2 = esp_wifi_set_max_tx_power(80); // 20 dBm
                        esp_err_t e3 = esp_wifi_set_ps(WIFI_PS_NONE);
                        esp_wifi_get_protocol(WIFI_IF_STA, &proto);
                        esp_wifi_get_max_tx_power(&txp);
                        ESP_LOGI(TAG, "wifiopt: after proto=0x%x txp=%d rc(proto=%d txp=%d ps=%d)",
                            (unsigned)proto, (int)txp, e1, e2, e3);
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
                        ESP_LOGI(TAG, "metrics_cast active=%d", (int)rock_player_is_cast_active());
                        uint32_t relay_bytes = 0, relay_overruns = 0, relay_errors = 0;
                        int relay_clients = 0;
                        rock_relay_get_metrics(&relay_bytes, &relay_overruns, &relay_errors,
                                               &relay_clients);
                        ESP_LOGI(TAG, "metrics_relay clients=%d bytes=%lu overruns=%lu send_errors=%lu",
                                 relay_clients, (unsigned long)relay_bytes,
                                 (unsigned long)relay_overruns, (unsigned long)relay_errors);
                        bool wake_running = false;
                        uint32_t wake_frames = 0, wake_invocations = 0, wake_detections = 0, wake_stage = 0, wake_peak = 0;
                        bool wake_armed = false;
                        rock_wake_get_metrics(&wake_running, &wake_frames,
                                              &wake_invocations, &wake_detections, &wake_stage,
                                              &wake_armed, &wake_peak);
                        ESP_LOGI(TAG, "metrics_wake running=%d frames=%lu invokes=%lu detections=%lu stage=%lu voice=%d armed=%d peak_milli=%lu muted=%d last_seq=%lu",
                                 (int)wake_running, (unsigned long)wake_frames,
                                 (unsigned long)wake_invocations,
                                 (unsigned long)wake_detections, (unsigned long)wake_stage,
                                 rock_voice_state(), (int)wake_armed, (unsigned long)wake_peak,
                                 (int)rock_wake_is_muted(),
                                 (unsigned long)rock_wake_last_detection_seq());
                        rock_spectrum_log_metrics();
                        rock_gui_log_debug_state();
                        ESP_LOGI(TAG, "metrics_ram int_free=%u int_min=%u int_largest=%u dma_largest=%u psram_free=%u psram_min=%u psram_largest=%u",
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
                    } else if (strcmp(cmd, "CMD:TASKS") == 0) {
                        char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (buf) {
                            vTaskList(buf);
                            ESP_LOGI(TAG, "task_list:\nName            State   Prio    Stack   Num\n%s", buf);
                            heap_caps_free(buf);
                        } else {
                            ESP_LOGE(TAG, "task_list alloc failed");
                        }
                    } else if (strcmp(cmd, "CMD:SCREENSHOT") == 0) {
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:NP") == 0) {
                        rock_gui_switch_view(0);
                        rock_gui_dump_screenshot();
                    } else if (strcmp(cmd, "CMD:VIEW:CATALOG") == 0 ||
                               strcmp(cmd, "CMD:VIEW:BROWSE") == 0 ||
                               strcmp(cmd, "CMD:VIEW:SEARCH") == 0) {
                        // BROWSE/SEARCH stay as aliases of CATALOG (UC-3 merged
                        // the two screens) so existing tool runs keep working.
                        rock_gui_switch_view(1);
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
    // 16 KiB: the screenshot path renders lv_snapshot on this stack (the
    // former 32 KiB was the single largest internal-RAM consumer after the
    // LVGL draw buffer and pushed MALLOC_CAP_INTERNAL to ~28 KiB free).
    if (xTaskCreatePinnedToCoreWithCaps(serial_cmd_task, "serial_cmd", 16384, NULL, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial command task");
        return -1;
    }
    return 0;
}
