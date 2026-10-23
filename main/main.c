#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#include "board_bus.h"
#include "control_platform.h"
#include "display_bsp.h"
#include "rock_player.h"
#include "rock_ota.h"
#include "rock_gui.h"
#include "rock_ui_screens.h"

static const char *TAG = "rock-esp32";

extern void rust_main(void);

// OTA → UI adapter: renders download progress on the local screen.
static void ota_on_progress(const char *version, int percent)
{
    rock_ui_ota_progress_show(version, percent);
}

// Platform → UI adapters: splash boot status and the Wi-Fi scan list.
static bool platform_ui_on_splash_status(const char *text)
{
    return rock_ui_splash_status(text);
}

static void platform_ui_on_wifi_scan(const rock_wifi_scan_item_t *items, uint16_t count)
{
    rock_ui_wifi_scan_show(items, count);
}

// Player → UI adapter: the player only reports abstract states; which text
// and screen each state renders is decided here, next to the display wiring.
// When the native station GUI is active, playback status is driven through
// rock_gui_update_playback by the Rust transport session.
static void player_ui_on_playback_state(const char *station_id, rock_player_ui_state_t state)
{
    if (rock_gui_is_active()) {
        return;
    }
    switch (state) {
        case ROCK_PLAYER_UI_BUFFERING:
            rock_ui_show_now_playing(station_id, "BUFFERING", "Connecting to stream...");
            break;
        case ROCK_PLAYER_UI_STALLED:
            rock_ui_show_now_playing(station_id, "BUFFERING", "Waiting for stream...");
            break;
        case ROCK_PLAYER_UI_PLAYING:
            rock_ui_show_now_playing(station_id, "PLAYING", "RockCast Radio");
            break;
        case ROCK_PLAYER_UI_PAUSED:
            rock_ui_show_now_playing(station_id, "PAUSED", "");
            break;
        case ROCK_PLAYER_UI_ERROR:
            rock_ui_show_text("PLAYBACK ERROR");
            break;
    }
}

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
    esp_reset_reason_t rst = esp_reset_reason();
    const char *rst_name = "UNKNOWN";
    switch (rst) {
        case ESP_RST_POWERON:   rst_name = "POWERON"; break;
        case ESP_RST_EXT:       rst_name = "EXT"; break;
        case ESP_RST_SW:        rst_name = "SW"; break;
        case ESP_RST_PANIC:     rst_name = "PANIC"; break;
        case ESP_RST_INT_WDT:   rst_name = "INT_WDT"; break;
        case ESP_RST_TASK_WDT:  rst_name = "TASK_WDT"; break;
        case ESP_RST_WDT:       rst_name = "WDT"; break;
        case ESP_RST_DEEPSLEEP: rst_name = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT:  rst_name = "BROWNOUT"; break;
        case ESP_RST_SDIO:      rst_name = "SDIO"; break;
        default:                rst_name = "OTHER"; break;
    }
    ESP_LOGI(TAG, "Boot reset reason: %d (%s)", (int)rst, rst_name);

    // Initialize OTA state tracking early (checks if running image is PENDING_VERIFY)
    rock_ota_init();
    rock_ota_set_progress_cb(ota_on_progress);

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
    } else {
        // Restore the persisted volume before any consumer reads player
        // state: the Rust boot GUI seed and the session refresh ticks copy
        // hardware state, and with a default-loaded codec the first seconds
        // after reboot would show (and play at) the wrong level.
        uint8_t saved_volume = 0;
        if (rock_nvs_init() == 0 && rock_settings_volume_load(&saved_volume) == 1) {
            rock_player_set_volume(saved_volume);
        }
    }
    rock_player_ui_cbs_t player_ui_cbs = {
        .on_playback_state = player_ui_on_playback_state,
    };
    rock_player_set_ui_cbs(&player_ui_cbs);
    // rock_platform_init() runs from rust_main(), so platform UI callbacks
    // must be in place before it.
    rock_platform_ui_cbs_t platform_ui_cbs = {
        .on_splash_status = platform_ui_on_splash_status,
        .on_wifi_scan = platform_ui_on_wifi_scan,
        .on_onboarding_status = rock_ui_onboarding_status,
        .on_onboarding_show = rock_ui_onboarding_show,
    };
    rock_platform_set_ui_cbs(&platform_ui_cbs);
#if CONFIG_ROCK_BRINGUP_TESTS
    if (rock_player_bringup_start() != 0) {
        ESP_LOGE(TAG, "Player bring-up task start failed");
    }
#endif

    // Setup local timezone for SNTP (MSK UTC+3 by default per center instruction)
    setenv("TZ", "MSK-3", 1);
    tzset();

    rust_main();
}
