// Minimal display bring-up for the JC4880P443C_I_W board (ESP32-P4 +
// ST7701 4.3" 480x800 panel over MIPI-DSI) plus the first slice of the
// owned LVGL binding layer (clock UI facade for Rust).
//
// Every pin, timing and init value below is taken from the working vendor
// LVGL v9 demo for this board (D:\work\JC4880P443C_I_W, idf_examples
// ESP-IDF_5.5.4, esp32_p4_function_ev_board BSP + esp_lcd_st7701 1.1.3).
// The panel is natively 480x800 portrait; LVGL software-rotates it to the
// landscape 800x480 the product uses.

#include "display_bsp.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include <stddef.h>

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
static lv_obj_t *s_clock_label;
static void *s_qr_buffer;

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

    // LVGL port owns the render task; a small single internal-RAM buffer is
    // enough for the clock (the vendor demo uses the same shape). Task
    // settings mirror the vendor demo: 16 KiB stack, priority 4, 5 ms tick.
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 16384;
    port_cfg.task_affinity = -1;
    port_cfg.task_max_sleep_ms = 500;
    port_cfg.timer_period_ms = 5;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed");
        return -1;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = 480 * 50,
        .double_buffer = false,
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
            .buff_dma = false,
            .buff_spiram = false,
            .sw_rotate = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_disp_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    if (lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg) == NULL) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return -1;
    }

    if (display_brightness_init() != 0) {
        ESP_LOGE(TAG, "Backlight init failed");
        return -1;
    }
    return 0;
}

int rock_ui_clock_init(void)
{
    if (!lvgl_port_lock(0)) {
        return -1;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Small product caption; the big uptime label sits centered below.
    lv_obj_t *caption = lv_label_create(screen);
    lv_label_set_text(caption, "ROCK");
    lv_obj_set_style_text_color(caption, lv_color_hex(0x7C8598), 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_16, 0);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 24);

    s_clock_label = lv_label_create(screen);
    lv_label_set_text(s_clock_label, "00:00:00");
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_center(s_clock_label);

    lvgl_port_unlock();
    return 0;
}

bool rock_ui_clock_set_text(const char *text)
{
    if (!lvgl_port_lock(100)) {
        return false;
    }
    lv_label_set_text(s_clock_label, text);
    lvgl_port_unlock();
    return true;
}

bool rock_ui_pairing_show(const char *short_code, const char *phrase,
                          const uint8_t *modules, uint16_t width)
{
    if (!short_code || !phrase || !modules || width == 0 || width > 177 || !lvgl_port_lock(1000)) return false;
    const int canvas_size = 320, quiet = 4;
    const int scale = canvas_size / (width + quiet * 2);
    const int used = scale * (width + quiet * 2);
    if (scale < 1) { lvgl_port_unlock(); return false; }
    if (!s_qr_buffer) {
        s_qr_buffer = heap_caps_calloc((size_t)canvas_size * canvas_size, sizeof(lv_color16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_qr_buffer) { lvgl_port_unlock(); return false; }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_fmt(title, "PAIR  %s", short_code);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, -45);
    lv_obj_t *verification = lv_label_create(screen);
    lv_label_set_text(verification, phrase);
    lv_obj_set_style_text_color(verification, lv_color_hex(0x7C8598), 0);
    lv_obj_set_style_text_font(verification, &lv_font_montserrat_16, 0);
    lv_obj_align(verification, LV_ALIGN_LEFT_MID, 45, 25);
    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, s_qr_buffer, canvas_size, canvas_size, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    int origin = (canvas_size - used) / 2 + quiet * scale;
    for (uint16_t y = 0; y < width; ++y) for (uint16_t x = 0; x < width; ++x) {
        if (!modules[(size_t)y * width + x]) continue;
        for (int py = 0; py < scale; ++py) for (int px = 0; px < scale; ++px)
            lv_canvas_set_px(canvas, origin + x * scale + px, origin + y * scale + py, lv_color_black(), LV_OPA_COVER);
    }
    lv_obj_align(canvas, LV_ALIGN_RIGHT_MID, -35, 0);
    lvgl_port_unlock();
    return true;
}

static const char *get_auth_mode_str(uint8_t authmode)
{
    switch (authmode) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA-PSK";
        case 3: return "WPA2-PSK";
        case 4: return "WPA/WPA2";
        case 5: return "WPA2-ENT";
        case 6: return "WPA3-PSK";
        case 7: return "WPA2/WPA3";
        case 8: return "WAPI";
        case 9: return "OWE";
        default: return "SECURED";
    }
}

bool rock_ui_wifi_scan_show(const rock_wifi_scan_item_t *items, uint16_t count)
{
    if (!lvgl_port_lock(1000)) return false;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AVAILABLE WI-FI NETWORKS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 20);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text_fmt(subtitle, "Found %u access point(s)", count);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x7C8598), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 30, 45);

    if (count == 0 || !items) {
        lv_obj_t *empty = lv_label_create(screen);
        lv_label_set_text(empty, "No Wi-Fi networks found");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x7C8598), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_center(empty);
        lvgl_port_unlock();
        return true;
    }

    lv_obj_t *cont = lv_obj_create(screen);
    lv_obj_set_size(cont, 740, 390);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x14171F), 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x262C38), 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_radius(cont, 8, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    uint16_t show_count = count > 30 ? 30 : count;
    for (uint16_t i = 0; i < show_count; i++) {
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1B202B), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // RSSI badge
        lv_obj_t *rssi_lbl = lv_label_create(row);
        lv_label_set_text_fmt(rssi_lbl, "%3d dBm", items[i].rssi);
        uint32_t rssi_color = items[i].rssi >= -60 ? 0x4ADE80 : (items[i].rssi >= -75 ? 0xFACC15 : 0xF87171);
        lv_obj_set_style_text_color(rssi_lbl, lv_color_hex(rssi_color), 0);
        lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(rssi_lbl, 80);

        // SSID
        lv_obj_t *ssid_lbl = lv_label_create(row);
        lv_label_set_text(ssid_lbl, items[i].ssid[0] ? items[i].ssid : "<Hidden Network>");
        lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(0xF5F7FA), 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_flex_grow(ssid_lbl, 1);

        // Channel
        lv_obj_t *ch_lbl = lv_label_create(row);
        lv_label_set_text_fmt(ch_lbl, "CH %u", items[i].channel);
        lv_obj_set_style_text_color(ch_lbl, lv_color_hex(0x7C8598), 0);
        lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(ch_lbl, 60);

        // Auth
        lv_obj_t *auth_lbl = lv_label_create(row);
        lv_label_set_text(auth_lbl, get_auth_mode_str(items[i].authmode));
        lv_obj_set_style_text_color(auth_lbl, lv_color_hex(items[i].authmode == 0 ? 0x94A3B8 : 0x60A5FA), 0);
        lv_obj_set_style_text_font(auth_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(auth_lbl, 110);
    }

    lvgl_port_unlock();
    return true;
}
