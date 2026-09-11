#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>

#include "board_bus.h"
#include "display_bsp.h"
#include "player_bsp.h"
#include "rock_ota.h"

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
    // Initialize OTA state tracking early (checks if running image is PENDING_VERIFY)
    rock_ota_init();

    // Create the shared I2C1 bus (GT911 + ES8311) once, before any consumer.
    if (rock_i2c1_bus_acquire() == NULL) {
        ESP_LOGE(TAG, "Shared I2C1 bus init failed; display/audio may fail");
    }
    if (rock_display_init() != 0 || rock_ui_splash_show() != 0) {
        ESP_LOGE(TAG, "Display bring-up failed; continuing without UI");
    }
    // RE-5 streaming player takes over the audio path (I2S, ES8311, decoders, TX worker).
    if (rock_player_init() != 0) {
        ESP_LOGE(TAG, "Player subsystem init failed");
    }
    rust_main();
}
