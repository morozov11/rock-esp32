#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>

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
    if (rock_display_init() != 0 || rock_ui_clock_init() != 0) {
        ESP_LOGE(TAG, "Display bring-up failed; continuing without UI");
    }
    rust_main();
}
