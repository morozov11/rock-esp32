#include "rock_player_internal.h"
#include "audio_bsp.h"

#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rock-player";

static void audio_tx_task(void *arg)
{
    (void)arg;
    static uint8_t pcm_chunk[1024];

    while (1) {
        rock_player_status_t st = rock_player_get_status();

        if (st == ROCK_PLAYER_STATUS_PLAYING) {
            size_t n = rock_pcm_ring_read(rock_player_get_pcm_ring(), pcm_chunk, sizeof(pcm_chunk));
            uint32_t rate;
            uint8_t channels;
            rock_player_get_audio_format(&rate, &channels);
            rock_pcm_publish(rock_player_get_pcm_tap(), pcm_chunk, n, rate, channels);
            if (n > 0) {
                esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)rock_audio_codec_handle();
                if (codec) {
                    esp_codec_dev_write(codec, pcm_chunk, (int)n);
                    rock_player_pa_set(true);
                }
                rock_player_note_playback_stable();
            } else {
                // Pause output until the relay has refilled enough PCM to play smoothly.
                uint32_t next_preroll = rock_player_note_underrun();
                rock_player_set_status(ROCK_PLAYER_STATUS_BUFFERING);
                rock_player_pa_set(false);
                rock_player_notify_ui(NULL, ROCK_PLAYER_UI_STALLED);
                ESP_LOGW(TAG, "PCM ring underrun (total: %u, next preroll %u bytes)",
                    (unsigned)rock_player_get_underruns(), (unsigned)next_preroll);
            }
        } else if (st == ROCK_PLAYER_STATUS_PAUSED) {
            rock_player_pa_set(false);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            // STOPPED / IDLE / ERROR
            rock_player_pa_set(false);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

int rock_player_tx_init(void)
{
    if (xTaskCreatePinnedToCore(audio_tx_task, "rock_au_tx", 4096, NULL, 6, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio TX worker");
        return -1;
    }
    return 0;
}
