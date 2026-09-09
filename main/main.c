#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>

#include "audio_bsp.h"
#include "board_bus.h"
#include "display_bsp.h"

static const char *TAG = "rock-esp32";

extern void rust_main(void);

void rock_log_heap(size_t bytes)
{
    ESP_LOGI(TAG, "Rust + esp-idf-sys; free heap: %u bytes", (unsigned)bytes);
}

void rock_delay_ms(uint32_t milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

void app_main(void)
{
    // Create the shared I2C1 bus (GT911 + ES8311) once, before any consumer.
    if (rock_i2c1_bus_acquire() == NULL) {
        ESP_LOGE(TAG, "Shared I2C1 bus init failed; display/audio may fail");
    }
    if (rock_display_init() != 0 || rock_ui_clock_init() != 0) {
        ESP_LOGE(TAG, "Display bring-up failed; continuing without UI");
    }
    // RE-4 audio proof runs as an isolated task next to the normal firmware
    // (LVGL, GT911, ESP-Hosted); it is removed again when the RE-5 player
    // takes over the audio path.
    if (rock_audio_bringup_start() != 0) {
        ESP_LOGE(TAG, "Audio bring-up task start failed");
    }
    rust_main();
}
