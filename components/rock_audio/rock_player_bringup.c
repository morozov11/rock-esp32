#include "rock_player_internal.h"
#include "audio_bsp.h"
#include "rock_ffi_exports.h"

#if CONFIG_ROCK_BRINGUP_TESTS
#include "test_mp3.h"

#include "esp_audio_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <assert.h>

static const char *TAG = "rock-player";

static void player_bringup_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "RE-5 PLAYER HARDWARE VERIFICATION TASK STARTED");
    ESP_LOGI(TAG, "==================================================");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 1. SSRF Gate Verification
    ESP_LOGI(TAG, "[TEST 1/5] SSRF Gate validation...");
    assert(!rock_validate_stream_uri("http://127.0.0.1:8000/stream.mp3"));
    assert(!rock_validate_stream_uri("http://localhost:8000/stream.mp3"));
    assert(!rock_validate_stream_uri("http://10.0.0.1/stream.mp3"));
    assert(!rock_validate_stream_uri("http://192.168.1.100/stream.mp3"));
    assert(!rock_validate_stream_uri("http://169.254.169.254/latest/meta-data"));
    assert(!rock_validate_stream_uri("ftp://example.com/stream.mp3"));
    assert(rock_validate_stream_uri("https://stream.rockcast.live/classic.mp3"));
    assert(rock_validate_stream_uri("http://stream.example.org:8000/live.aac"));
    ESP_LOGI(TAG, "[TEST 1/5] SSRF Gate PASS (all 8 vectors verified)");

    // 2. Decoder & Audio Path verification
    ESP_LOGI(TAG, "[TEST 2/5] MP3 Decoder & Pipeline initialization...");
    rock_player_set_status(ROCK_PLAYER_STATUS_BUFFERING);
    rock_player_notify_ui("bringup-station", ROCK_PLAYER_UI_BUFFERING);

    esp_audio_dec_handle_t dec = NULL;
    esp_audio_dec_cfg_t dec_cfg = {
        .type = ESP_AUDIO_TYPE_MP3,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder");
        vTaskDelete(NULL);
        return;
    }

    static uint8_t out_buf[DECODE_OUT_CAP];
    size_t in_pos = 0;
    uint32_t current_fs = 16000;
    uint8_t current_channels = 1;
    bool playback_started = false;

    // Decode until pre-roll is reached and playback starts. The embedded
    // clip decodes to only ~230 KB of PCM, under the pre-roll threshold,
    // so loop the input like the soak below instead of a single pass.
    while (!playback_started) {
        if (in_pos >= s_test_mp3_len) in_pos = 0;
        esp_audio_dec_in_raw_t raw = {
            .buffer = (uint8_t *)(s_test_mp3_data + in_pos),
            .len = (uint32_t)(s_test_mp3_len - in_pos),
            .consumed = 0,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = out_buf,
            .len = sizeof(out_buf),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t dret = esp_audio_dec_process(dec, &raw, &out_frame);
        if (dret == ESP_AUDIO_ERR_OK || dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            in_pos += raw.consumed;
            if (out_frame.decoded_size > 0) {
                esp_audio_dec_info_t info;
                if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    if (info.sample_rate != current_fs || info.channel != current_channels) {
                        ESP_LOGI(TAG, "[TEST 2/5] Decoded stream format: %lu Hz, %u ch (reconfiguring codec)",
                                 (unsigned long)info.sample_rate, info.channel);
                        rock_audio_reconfig(info.sample_rate, info.channel);
                        rock_player_set_audio_format(info.sample_rate, info.channel);
                        current_fs = info.sample_rate;
                        current_channels = info.channel;
                    }
                }
                rock_pcm_ring_write(rock_player_get_pcm_ring(), out_buf, out_frame.decoded_size);
                if (!playback_started && rock_pcm_ring_fill(rock_player_get_pcm_ring()) >= PREROLL_MIN_BYTES) {
                    playback_started = true;
                    rock_player_set_status(ROCK_PLAYER_STATUS_PLAYING);
                    rock_player_notify_ui("bringup-station", ROCK_PLAYER_UI_PLAYING);
                    rock_player_pa_set(true);
                    ESP_LOGI(TAG, "[TEST 2/5] Pre-roll buffer reached (%u bytes), PA GPIO11 active, playback PASS",
                             (unsigned)PREROLL_MIN_BYTES);
                    break;
                }
            }
        } else {
            in_pos++;
        }
    }

    // 3. Hardware Controls Verification
    ESP_LOGI(TAG, "[TEST 3/5] Volume & Mute control verification...");
    rock_player_set_volume(40);
    vTaskDelay(pdMS_TO_TICKS(100));
    {
        uint8_t vol = 0;
        rock_player_get_state(NULL, 0, NULL, 0, &vol, NULL);
        assert(vol == 40);
    }
    rock_player_set_volume(70);
    vTaskDelay(pdMS_TO_TICKS(100));
    {
        uint8_t vol = 0;
        rock_player_get_state(NULL, 0, NULL, 0, &vol, NULL);
        assert(vol == 70);
    }

    rock_player_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    assert(rock_player_is_muted() == true);
    rock_player_set_mute(false);
    vTaskDelay(pdMS_TO_TICKS(200));
    assert(rock_player_is_muted() == false);

    rock_player_pause();
    assert(rock_player_get_status() == ROCK_PLAYER_STATUS_PAUSED);
    vTaskDelay(pdMS_TO_TICKS(300));
    rock_player_play();
    assert(rock_player_get_status() == ROCK_PLAYER_STATUS_PLAYING);
    ESP_LOGI(TAG, "[TEST 3/5] Volume/Mute/Pause/Play control PASS");

    // 4. Extended 30-Second Playback & Soak Loop
    ESP_LOGI(TAG, "[TEST 4/5] 30-second continuous playback & soak on Core 1...");
    int soak_seconds = 30;
    TickType_t start_tick = xTaskGetTickCount();
    size_t loop_pos = in_pos;
    size_t initial_internal = esp_get_free_internal_heap_size();
    size_t initial_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    int last_logged_sec = 0;
    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(soak_seconds * 1000)) {
        // Feed decoder
        if (rock_pcm_ring_fill(rock_player_get_pcm_ring()) < (PCM_RING_CAPACITY * 70 / 100)) {
            if (loop_pos >= s_test_mp3_len) loop_pos = 0;
            esp_audio_dec_in_raw_t raw = {
                .buffer = (uint8_t *)(s_test_mp3_data + loop_pos),
                .len = (uint32_t)(s_test_mp3_len - loop_pos),
                .consumed = 0,
            };
            esp_audio_dec_out_frame_t out_frame = {
                .buffer = out_buf,
                .len = sizeof(out_buf),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_err_t dret = esp_audio_dec_process(dec, &raw, &out_frame);
            if (dret == ESP_AUDIO_ERR_OK || dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                loop_pos += raw.consumed;
                if (out_frame.decoded_size > 0) {
                    rock_pcm_ring_write(rock_player_get_pcm_ring(), out_buf, out_frame.decoded_size);
                }
            } else {
                loop_pos++;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        int elapsed_sec = (int)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS / 1000);
        if (elapsed_sec > last_logged_sec && elapsed_sec % 5 == 0) {
            last_logged_sec = elapsed_sec;
            size_t cur_internal = esp_get_free_internal_heap_size();
            size_t cur_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            uint32_t underruns, dec_errors, net_reconnects;
            rock_player_get_stats(&underruns, &dec_errors, &net_reconnects);
            ESP_LOGI(TAG, "[SOAK t=%2d s] internal_ram=%u B, psram=%u B, underruns=%lu, dec_errors=%lu",
                     elapsed_sec, (unsigned)cur_internal, (unsigned)cur_psram,
                     (unsigned long)underruns, (unsigned long)dec_errors);
        }
    }

    size_t final_internal = esp_get_free_internal_heap_size();
    size_t final_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t total_underruns, total_dec_errors, total_reconnects;
    rock_player_get_stats(&total_underruns, &total_dec_errors, &total_reconnects);
    ESP_LOGI(TAG, "[TEST 4/5] 30s soak finished: internal delta=%d B, psram delta=%d B, underruns=%lu",
             (int)(final_internal - initial_internal), (int)(final_psram - initial_psram),
             (unsigned long)total_underruns);

    // 5. Stop & Error Resilience
    ESP_LOGI(TAG, "[TEST 5/5] Stop & decoder resilience...");
    rock_player_stop();
    assert(rock_player_get_status() == ROCK_PLAYER_STATUS_STOPPED);
    assert(rock_pcm_ring_fill(rock_player_get_pcm_ring()) == 0);
    ESP_LOGI(TAG, "[TEST 5/5] Stop verified");

    // Close bring-up decoder
    esp_audio_dec_close(dec);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "RE-5 PLAYER HARDWARE VERIFICATION: ALL PASSED!");
    ESP_LOGI(TAG, "==================================================");

    vTaskDelete(NULL);
}

int rock_player_bringup_start(void)
{
    if (xTaskCreatePinnedToCore(player_bringup_task, "rock_pl_test", 8192, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn player bringup task");
        return -1;
    }
    return 0;
}
#endif // CONFIG_ROCK_BRINGUP_TESTS
