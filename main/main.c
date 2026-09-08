#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rock-esp32";

void app_main(void)
{
    while (true) {
        ESP_LOGI(TAG, "Hello from Rock ESP32-P4");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
