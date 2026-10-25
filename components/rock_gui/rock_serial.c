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
#include "rock_gui.h"
#include "rock_screenshot.h"
#include "audio_spectrum.h"
#include "audio_bsp.h"

#include "driver/usb_serial_jtag.h"
#include "mbedtls/base64.h"
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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-serial";

// Raw transport throughput modes (bit values mirror ESP_TEST_RAW_TP__* in
// esp_hosted's eh_caps.h). The engine measures the SDIO transport alone,
// with WiFi idle, to separate link quality from transport capacity.
#define RAWTP_ESP_TO_HOST  (1 << 1)
#define RAWTP_HOST_TO_ESP  (1 << 2)
#define RAWTP_BIDIRECTIONAL (1 << 3)

// RE-8 dataset capture is intentionally a serial-only diagnostic path.  It
// records on the board, then sends bounded base64 chunks to the host after the
// microphone has been released.  It never opens the voice WebSocket.
#define RE8_CAPTURE_RATE_HZ       16000u
#define RE8_CAPTURE_DELAY_MS       2000u
#define RE8_CAPTURE_DURATION_MS    8000u
#define RE8_CAPTURE_MIN_DURATION_MS 5000u
#define RE8_CAPTURE_MAX_DURATION_MS 120000u
#define RE8_CAPTURE_READ_BYTES     1600u
#define RE8_CAPTURE_DATA_BYTES      240u
#define RE8_CAPTURE_LABEL_CAP        33u

typedef struct {
    uint32_t id;
    uint32_t duration_ms;
    char label[RE8_CAPTURE_LABEL_CAP];
} re8_capture_job_t;

static atomic_bool s_re8_capture_active;
static atomic_uint s_re8_capture_next_id;

static bool re8_capture_label_valid(const char *label)
{
    if (!label || !label[0] || strlen(label) >= RE8_CAPTURE_LABEL_CAP) return false;
    for (const unsigned char *p = (const unsigned char *)label; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static uint32_t re8_capture_fnv1a(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void re8_capture_task(void *arg)
{
    rock_wake_set_paused(true);
    re8_capture_job_t *job = arg;
    const size_t total_bytes = (size_t)RE8_CAPTURE_RATE_HZ * job->duration_ms / 1000u * sizeof(int16_t);
    uint8_t *recording = NULL;
    uint32_t rate = 0;
    uint8_t channels = 0;

    if (rock_audio_init() != 0) {
        ESP_LOGE(TAG, "RE8CAP error id=%lu reason=audio_init", (unsigned long)job->id);
        goto done;
    }
    rock_audio_capture_format(&rate, &channels);
    if (rate < RE8_CAPTURE_RATE_HZ || rate > 96000 || (channels != 1 && channels != 2)) {
        ESP_LOGE(TAG, "RE8CAP error id=%lu reason=format rate=%lu channels=%u",
                 (unsigned long)job->id, (unsigned long)rate, (unsigned)channels);
        goto done;
    }
    recording = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording) {
        ESP_LOGE(TAG, "RE8CAP error id=%lu reason=no_psram bytes=%u", (unsigned long)job->id,
                 (unsigned)total_bytes);
        goto done;
    }

    ESP_LOGI(TAG, "RE8CAP ready id=%lu label=%s delay_ms=%u duration_ms=%u",
             (unsigned long)job->id, job->label, RE8_CAPTURE_DELAY_MS, (unsigned)job->duration_ms);
    vTaskDelay(pdMS_TO_TICKS(RE8_CAPTURE_DELAY_MS));
    ESP_LOGI(TAG, "RE8CAP begin id=%lu", (unsigned long)job->id);
    size_t written_samples = 0;
    uint32_t phase = 0;
    int16_t raw[RE8_CAPTURE_READ_BYTES / sizeof(int16_t)];
    while (written_samples < total_bytes / sizeof(int16_t)) {
        uint32_t current_rate = 0;
        uint8_t current_channels = 0;
        rock_audio_capture_format(&current_rate, &current_channels);
        if (current_rate < RE8_CAPTURE_RATE_HZ || current_rate > 96000 ||
            (current_channels != 1 && current_channels != 2) ||
            rock_audio_capture_read(raw, sizeof(raw)) != sizeof(raw)) {
            ESP_LOGE(TAG, "RE8CAP error id=%lu reason=microphone", (unsigned long)job->id);
            goto done;
        }
        if (current_rate != rate || current_channels != channels) {
            rate = current_rate;
            channels = current_channels;
            phase = 0;
        }
        const size_t frames = sizeof(raw) / sizeof(raw[0]) / channels;
        bool left_only = false;
        bool right_only = false;
        if (channels == 2) {
            uint32_t left = 0;
            uint32_t right = 0;
            for (size_t frame = 0; frame < frames; ++frame) {
                left += (unsigned)(raw[2 * frame] < 0 ? -(int)raw[2 * frame] : raw[2 * frame]);
                right += (unsigned)(raw[2 * frame + 1] < 0 ? -(int)raw[2 * frame + 1] : raw[2 * frame + 1]);
            }
            left_only = left > right * 4;
            right_only = right > left * 4;
        }
        int16_t *pcm16 = (int16_t *)recording;
        for (size_t frame = 0; frame < frames && written_samples < total_bytes / sizeof(int16_t); ++frame) {
            int16_t sample = channels == 1 ? raw[frame] :
                left_only ? raw[2 * frame] : right_only ? raw[2 * frame + 1] :
                (int16_t)(((int32_t)raw[2 * frame] + raw[2 * frame + 1]) / 2);
            phase += RE8_CAPTURE_RATE_HZ;
            if (phase >= rate) {
                phase -= rate;
                pcm16[written_samples++] = sample;
            }
        }
    }

    for (size_t offset = 0, seq = 0; offset < total_bytes; offset += RE8_CAPTURE_DATA_BYTES, ++seq) {
        const size_t input_len = total_bytes - offset < RE8_CAPTURE_DATA_BYTES ?
            total_bytes - offset : RE8_CAPTURE_DATA_BYTES;
        char encoded[4 * ((RE8_CAPTURE_DATA_BYTES + 2) / 3) + 1];
        size_t encoded_len = 0;
        if (mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encoded_len,
                                  recording + offset, input_len) != 0) {
            ESP_LOGE(TAG, "RE8CAP error id=%lu reason=base64", (unsigned long)job->id);
            goto done;
        }
        encoded[encoded_len] = '\0';
        ESP_LOGI(TAG, "RE8CAP data id=%lu seq=%u %s", (unsigned long)job->id,
                 (unsigned)seq, encoded);
    }
    ESP_LOGI(TAG, "RE8CAP end id=%lu bytes=%u fnv1a=%08lx", (unsigned long)job->id,
             (unsigned)total_bytes, (unsigned long)re8_capture_fnv1a(recording, total_bytes));

done:
    rock_wake_set_paused(false);
    if (recording) heap_caps_free(recording);
    free(job);
    atomic_store(&s_re8_capture_active, false);
    vTaskDeleteWithCaps(NULL);
}

static void re8_capture_start(const char *label, uint32_t duration_ms)
{
    if (!re8_capture_label_valid(label)) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=label (use 1..32 letters, digits, _ or -)");
        return;
    }
    if (duration_ms < RE8_CAPTURE_MIN_DURATION_MS || duration_ms > RE8_CAPTURE_MAX_DURATION_MS) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=duration (use %u..%u ms)",
                 RE8_CAPTURE_MIN_DURATION_MS, RE8_CAPTURE_MAX_DURATION_MS);
        return;
    }
    int voice_state = rock_voice_state();
    if (voice_state >= 1 && voice_state <= 3) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=voice_active state=%d", voice_state);
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_re8_capture_active, &expected, true)) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=busy");
        return;
    }
    re8_capture_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_DEFAULT);
    if (!job) {
        atomic_store(&s_re8_capture_active, false);
        ESP_LOGE(TAG, "RE8CAP rejected reason=no_memory");
        return;
    }
    job->id = atomic_fetch_add(&s_re8_capture_next_id, 1) + 1;
    job->duration_ms = duration_ms;
    strcpy(job->label, label);
    if (xTaskCreateWithCaps(re8_capture_task, "re8_capture", 8192, job, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(job);
        atomic_store(&s_re8_capture_active, false);
        ESP_LOGE(TAG, "RE8CAP rejected reason=task");
    }
}

// Format: CMD:RE8:BULK:<label>:<seconds>.  The command buffer is mutable, so
// split it in place before scheduling the normal capture task.
static void re8_bulk_capture_start(char *arg)
{
    char *separator = strrchr(arg, ':');
    if (!separator || separator == arg) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=bulk_format (use label:seconds)");
        return;
    }
    *separator = '\0';
    char *end = NULL;
    unsigned long seconds = strtoul(separator + 1, &end, 10);
    if (!separator[1] || !end || *end || seconds > RE8_CAPTURE_MAX_DURATION_MS / 1000u ||
        seconds < RE8_CAPTURE_MIN_DURATION_MS / 1000u) {
        ESP_LOGE(TAG, "RE8CAP rejected reason=bulk_duration (use 5..120 seconds)");
        return;
    }
    re8_capture_start(arg, (uint32_t)seconds * 1000u);
}

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
                    } else if (!strncmp(cmd, "CMD:RE8:CAPTURE:", 16)) {
                        re8_capture_start(cmd + 16, RE8_CAPTURE_DURATION_MS);
                    } else if (!strncmp(cmd, "CMD:RE8:BULK:", 13)) {
                        re8_bulk_capture_start(cmd + 13);
                    } else if (!strcmp(cmd, "CMD:RE8:STATUS")) {
                        ESP_LOGI(TAG, "RE8CAP status active=%d next_id=%lu",
                                 (int)atomic_load(&s_re8_capture_active),
                                 (unsigned long)atomic_load(&s_re8_capture_next_id));
                    } else if (!strcmp(cmd, "CMD:RE8:PROBE")) {
                        rock_wake_probe();
                    } else if (!strcmp(cmd, "CMD:RE8:FEATURES")) {
                        rock_wake_feature_probe();
                    } else if (!strcmp(cmd, "CMD:RE8:WAKE:ARM")) {
                        rock_wake_set_armed(true);
                        ESP_LOGI(TAG, "RE8WAKE armed by serial command");
                    } else if (!strcmp(cmd, "CMD:RE8:WAKE:DISARM")) {
                        rock_wake_set_armed(false);
                        ESP_LOGI(TAG, "RE8WAKE disarmed by serial command");
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
                        ESP_LOGI(TAG, "metrics_wake running=%d frames=%lu invokes=%lu detections=%lu stage=%lu voice=%d armed=%d peak_milli=%lu",
                                 (int)wake_running, (unsigned long)wake_frames,
                                 (unsigned long)wake_invocations,
                                 (unsigned long)wake_detections, (unsigned long)wake_stage,
                                 rock_voice_state(), (int)wake_armed, (unsigned long)wake_peak);
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
