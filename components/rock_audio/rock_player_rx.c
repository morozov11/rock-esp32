#include "rock_player_internal.h"
#include "audio_bsp.h"
#include "rock_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-player";

static TaskHandle_t s_rx_task = NULL;

static void stream_rx_task(void *arg)
{
    (void)arg;
    uint32_t current_gen = rock_player_get_generation();
    esp_audio_dec_handle_t dec = NULL;

    // Per-generation heap buffers: rock_player_play_stream joins the previous
    // task for only ~600 ms, so two generations can overlap and must never
    // share static buffers here. Freed on every exit path (fail/cleanup).
    uint8_t *in_buf = NULL;
    uint8_t *out_buf = NULL;

    char current_station[130];
    rock_player_get_station(current_station, sizeof(current_station));

    rock_player_set_status(ROCK_PLAYER_STATUS_BUFFERING);
    rock_player_notify_ui(current_station, ROCK_PLAYER_UI_BUFFERING);

    if (!rock_source_is_active()) {
        ESP_LOGE(TAG, "Stream source is not running");
        goto fail;
    }

    // PCM mode: the ring already holds 48 kHz stereo PCM (transcoded by the
    // source for Cast); the speaker consumes it directly without a decoder.
    // Compressed mode: decode locally like before, just from the ring.
    bool pcm_mode = rock_source_is_pcm();
    in_buf = heap_caps_malloc(HTTP_CHUNK_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out_buf = pcm_mode ? NULL : heap_caps_malloc(DECODE_OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in_buf || (!pcm_mode && !out_buf)) {
        ESP_LOGE(TAG, "Failed to allocate stream RX buffers");
        goto fail;
    }

    if (!pcm_mode) {
        const char *content_type = rock_source_content_type();
        ESP_LOGI(TAG, "Source content-type: %s", content_type ? content_type : "(null)");
        esp_audio_type_t codec_type = rock_decoder_detect_type(content_type);
        if (codec_type == ESP_AUDIO_TYPE_UNSUPPORT) {
            ESP_LOGE(TAG, "Unsupported Content-Type (not MP3/AAC): %s", content_type ? content_type : "(null)");
            goto fail;
        }

        if (rock_decoder_open(codec_type, &dec) != 0) {
            goto fail;
        }
    }

    size_t in_len = 0;
    uint32_t current_fs = 0;
    uint8_t current_channels = 0;
    rock_format_gate_t gate;
    rock_format_gate_init(&gate);

    if (pcm_mode) {
        current_fs = 48000;
        current_channels = 2;
        if (rock_audio_reconfig(current_fs, current_channels) != 0) {
            ESP_LOGE(TAG, "Audio reconfig to 48 kHz stereo failed");
            goto fail;
        }
        rock_player_set_audio_format(current_fs, current_channels);
    }

    unsigned work_chunks = 0;
    while (!rock_player_is_stop_signaled() && rock_player_get_generation() == current_gen) {
        // Flow control: wait if PCM ring buffer is getting full (> 85%)
        if (rock_pcm_ring_fill(rock_player_get_pcm_ring()) > (PCM_RING_CAPACITY * 85 / 100)) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        int space = (int)(HTTP_CHUNK_SIZE * 2 - in_len);
        if (!pcm_mode && space > HTTP_CHUNK_SIZE) space = HTTP_CHUNK_SIZE;
        int rlen = rock_source_read(in_buf + in_len, (size_t)space);
        if (rlen < 0) {
            ESP_LOGW(TAG, "Stream source ended (%d)", rlen);
            goto fail;
        }
        if (rlen == 0) {
            rock_player_stat_inc_zero_reads();
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        rock_player_stat_add_net_bytes((uint32_t)rlen);
        in_len += (size_t)rlen;

        if (pcm_mode) {
            size_t written = rock_pcm_ring_write(rock_player_get_pcm_ring(), in_buf, in_len);
            rock_player_stat_add_pcm_bytes((uint32_t)written);
            in_len = 0;
            rock_player_check_preroll(current_station);
            if (++work_chunks % 8 == 0) vTaskDelay(1);
            continue;
        }

        // Decode loop
        int consumed = rock_decoder_step(dec, current_station, in_buf, in_len, out_buf,
                                         &current_fs, &current_channels, &gate);
        if (consumed < 0) {
            goto fail;
        }
        size_t in_pos = (size_t)consumed;

        // Shift unconsumed bytes to beginning
        if (in_pos < in_len) {
            memmove(in_buf, in_buf + in_pos, in_len - in_pos);
            in_len -= in_pos;
        } else {
            in_len = 0;
        }
        if (++work_chunks % 8 == 0) vTaskDelay(1);
    }

    goto cleanup;

fail:
    // A concurrent stop (or a newer play_stream generation) may have torn the
    // source down and already installed its own status: never overwrite that
    // with ERROR, the stream did not fail on its own.
    if (!rock_player_is_stop_signaled() && rock_player_get_generation() == current_gen) {
        rock_player_set_status(ROCK_PLAYER_STATUS_ERROR);
        rock_player_notify_ui(current_station, ROCK_PLAYER_UI_ERROR);
    }
    rock_player_pa_set(false);

cleanup:
    rock_decoder_close(dec);
    dec = NULL;
    free(out_buf);
    free(in_buf);
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

int rock_player_rx_start(const char *station_id)
{
    // Terminate any existing streaming task and discard its output
    rock_player_set_stop_signal(true);
    rock_player_next_generation();

    // Wait briefly for previous task to finish
    int wait_cycles = 0;
    while (s_rx_task != NULL && ++wait_cycles < 30) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    rock_pcm_ring_clear(rock_player_get_pcm_ring());
    rock_player_pa_set(false);

    rock_player_set_station(station_id);

    rock_player_stat_reset_stream();
    rock_player_set_stop_signal(false);

    // Spawn stream RX worker on Core 1
    if (xTaskCreatePinnedToCore(stream_rx_task, "rock_st_rx", 8192, NULL, 5, &s_rx_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start stream RX task");
        rock_player_set_status(ROCK_PLAYER_STATUS_ERROR);
        return -1;
    }

    return 0;
}

int rock_player_rx_stop_and_wait(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    while (s_rx_task != NULL && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    return s_rx_task == NULL ? 0 : -1;
}

bool rock_player_rx_is_running(void)
{
    return s_rx_task != NULL;
}
