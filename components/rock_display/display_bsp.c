// Display hardware bring-up for the JC4880P443C_I_W board (ESP32-P4 +
// ST7701 4.3" 480x800 panel over MIPI-DSI): DSI PHY power, bus, panel,
// LVGL port task, GT911 touch and backlight PWM.
//
// Every pin, timing and init value below is taken from the working vendor
// LVGL v9 demo for this board (D:\work\JC4880P443C_I_W, idf_examples
// ESP-IDF_5.5.4, esp32_p4_function_ev_board BSP + esp_lcd_st7701 1.1.3).
// The panel is natively 480x800 portrait; LVGL software-rotates it to the
// landscape 800x480 the product uses. All screens live in rock_ui_screens.c
// and rock_gui.c; this file knows no UI.

#include "display_bsp.h"

#include "board_bus.h"

#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "lvgl.h"

static const char *TAG = "rock-display";

// ST7701 reset line and the MIPI D-PHY rail (LDO_VO3 @ 2.5 V), per the
// vendor BSP for this board family.
#define ROCK_LCD_RST_GPIO GPIO_NUM_5
#define ROCK_DSI_PHY_LDO_CHAN 3
#define ROCK_DSI_PHY_LDO_MV 2500

// Backlight is a plain LEDC PWM line (GPIO23, 5 kHz, 10 bit).
#define ROCK_BACKLIGHT_GPIO GPIO_NUM_23
#define ROCK_BACKLIGHT_DUTY (716) // ~70% of 1023

static esp_ldo_channel_handle_t s_phy_ldo;
static lv_display_t *s_display;
static i2c_master_bus_handle_t s_touch_bus;
static esp_lcd_panel_io_handle_t s_touch_io;
static esp_lcd_touch_handle_t s_touch;

static void touch_pressed_cb(lv_event_t *event)
{
    lv_point_t point;
    lv_indev_get_point(lv_event_get_target(event), &point);
    ESP_LOGI("rock-touch", "pressed x=%ld y=%ld", (long)point.x, (long)point.y);
}

static int display_touch_init(void)
{
    // The GT911 shares I2C1 with the ES8311 codec; the bus itself is owned
    // and created by board_bus.c.
    s_touch_bus = rock_i2c1_bus_acquire();
    if (s_touch_bus == NULL) return -1;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_touch_bus, &io_cfg, &s_touch_io) != ESP_OK) return -1;
    // The touch controller native coordinates match panel; LVGL 9 handles rotation mapping
    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = 480, .y_max = 800, .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC, .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    if (esp_lcd_touch_new_i2c_gt911(s_touch_io, &touch_cfg, &s_touch) != ESP_OK) return -1;
    const lvgl_port_touch_cfg_t lvgl_cfg = {.disp = s_display, .handle = s_touch};
    lv_indev_t *indev = lvgl_port_add_touch(&lvgl_cfg);
    if (!indev) return -1;
    lv_indev_add_event_cb(indev, touch_pressed_cb, LV_EVENT_PRESSED, NULL);
    return 0;
}

static int display_brightness_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        return -1;
    }
    const ledc_channel_config_t channel = {
        .gpio_num = ROCK_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = 0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&channel) != ESP_OK) {
        return -1;
    }
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, 0, ROCK_BACKLIGHT_DUTY) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, 0) != ESP_OK) {
        return -1;
    }
    return 0;
}

int rock_display_init(void)
{
    // The DSI PHY starts in "no power"; its LDO rail must be up before the
    // bus is created.
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = ROCK_DSI_PHY_LDO_CHAN,
        .voltage_mv = ROCK_DSI_PHY_LDO_MV,
    };
    if (esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo) != ESP_OK) {
        ESP_LOGE(TAG, "DSI PHY LDO acquire failed");
        return -1;
    }

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 750,
    };
    if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus init failed");
        return -1;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "DBI panel IO failed");
        return -1;
    }

    // Vendor preset: 480x800 @ 28 MHz DPI, DMA2D copy path, RGB565.
    esp_lcd_dpi_panel_config_t dpi_cfg =
        ST7701_480_360_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
    dpi_cfg.num_fbs = 1;

    st7701_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = ROCK_LCD_RST_GPIO,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st7701(io, &dev_cfg, &panel) != ESP_OK ||
        esp_lcd_panel_reset(panel) != ESP_OK ||
        esp_lcd_panel_init(panel) != ESP_OK) {
        ESP_LOGE(TAG, "ST7701 panel init failed");
        return -1;
    }
    // ESP-IDF 6.x replaced the DPI `use_dma2d` flag with this explicit call.
    if (esp_lcd_dpi_panel_enable_dma2d(panel) != ESP_OK) {
        ESP_LOGE(TAG, "DMA2D enable failed");
        return -1;
    }

    // LVGL port owns the render task. Stack 16 KiB / 5 ms tick mirror the
    // vendor demo. The task is pinned to Core 0 at priority 5: floating at 4
    // let the Core-1 HE-AAC decode bursts (rock_st_rx prio 5, rock_au_tx 6)
    // starve the GUI down to ~10 FPS.
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 5;
    port_cfg.task_stack = 16384;
    port_cfg.task_affinity = 0;
    port_cfg.task_max_sleep_ms = 500;
    port_cfg.timer_period_ms = 5;
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed");
        return -1;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = 480 * 50,
        .double_buffer = true,
        .hres = 480,
        .vres = 800,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            // P4 PSRAM is DMA-capable: keeping the draw buffers (and the PPA
            // rotation buffer) in PSRAM frees ~144 KiB of internal RAM that
            // the I2S DMA descriptors need during codec reconfigs, and double
            // buffering lets the DMA2D flush overlap the next strip render.
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_disp_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    s_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return -1;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL port for rotation");
        return -1;
    }
    lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();

    if (display_touch_init() != 0) {
        ESP_LOGE(TAG, "GT911 touch init failed");
        return -1;
    }

    if (display_brightness_init() != 0) {
        ESP_LOGE(TAG, "Backlight init failed");
        return -1;
    }
    return 0;
}
