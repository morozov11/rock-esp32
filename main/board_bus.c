// Single owner of the shared I2C1 bus (GT911 touch + ES8311 codec), see
// board_bus.h. Created here once and handed out to both consumers; a second
// bus over the display-owned pins must never appear (plan step 1.4).
#include "board_bus.h"

#include "esp_log.h"

static const char *TAG = "rock-bus";

#define ROCK_I2C1_PORT I2C_NUM_1
#define ROCK_I2C1_SDA_GPIO GPIO_NUM_7
#define ROCK_I2C1_SCL_GPIO GPIO_NUM_8

static i2c_master_bus_handle_t s_i2c1_bus;

i2c_master_bus_handle_t rock_i2c1_bus_acquire(void)
{
    if (s_i2c1_bus) return s_i2c1_bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ROCK_I2C1_PORT,
        .sda_io_num = ROCK_I2C1_SDA_GPIO,
        .scl_io_num = ROCK_I2C1_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c1_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C1 bus creation failed");
        s_i2c1_bus = NULL;
        return NULL;
    }
    ESP_LOGI(TAG, "Shared I2C1 bus up (port %d, SDA %d, SCL %d)",
             ROCK_I2C1_PORT, ROCK_I2C1_SDA_GPIO, ROCK_I2C1_SCL_GPIO);
    return s_i2c1_bus;
}
