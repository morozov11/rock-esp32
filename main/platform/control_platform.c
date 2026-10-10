// Platform composition root (phase-4 split): NVS+identity, the Wi-Fi/HTTP/WSS
// init sequence delegating to rock_wifi_sta.c / rock_net_ws.c, time/random
// helpers and the C6 co-processor reset. Public ABI: control_platform.h;
// cross-file plumbing: control_platform_internal.h.

#include "control_platform.h"
#include "control_platform_internal.h"
#include "rock_ota.h"
#include "rock_wifi_storage.h"
#include "rock_onboarding.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROCK_C6_RESET_GPIO GPIO_NUM_54

// UI callbacks registered by the composition root; this module never calls
// rock_ui_* directly (dependency inversion, phase 3).
static rock_platform_ui_cbs_t s_ui_cbs;

static const char *TAG = "rock-control";

void rock_platform_set_ui_cbs(const rock_platform_ui_cbs_t *cbs)
{
    s_ui_cbs = cbs ? *cbs : (rock_platform_ui_cbs_t){0};
}

void rock_platform_notify_splash_status(const char *text)
{
    if (s_ui_cbs.on_splash_status) s_ui_cbs.on_splash_status(text);
}

void rock_platform_notify_wifi_scan(const rock_wifi_scan_item_t *items, uint16_t count)
{
    if (s_ui_cbs.on_wifi_scan) s_ui_cbs.on_wifi_scan(items, count);
}

int rock_platform_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS requires maintenance; refusing to erase stored identity");
        return -1;
    }
    if (err != ESP_OK) return -1;
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
    if (!rock_wifi_events_create() || !esp_netif_create_default_wifi_sta()) return -1;
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) return -1;
    rock_wifi_register_event_handlers();
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) return -1;
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Allow scanning/operating on channels 12-13 (EU/RU 2.4 GHz band) so the
    // default 1-11 country cannot hide the target AP after a router channel change.
    esp_err_t country_err = esp_wifi_set_country_code("RU", true);
    if (country_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi country code set failed: %s", esp_err_to_name(country_err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi country set to RU (channels 1-13 enabled)");
    }

    rock_platform_notify_splash_status("Checking network...");

    // Check credentials hierarchy
    char active_ssid[33] = {0};
    char active_password[65] = {0};
    bool has_creds = false;
    bool creds_from_sdkconfig = false;

    if (rock_wifi_storage_load(active_ssid, sizeof(active_ssid), active_password, sizeof(active_password))) {
        ESP_LOGI(TAG, "Using Wi-Fi credentials from NVS rock_wifi");
        has_creds = true;
    } else if (CONFIG_ROCK_WIFI_SSID[0] != '\0') {
        ESP_LOGI(TAG, "Using Wi-Fi credentials from sdkconfig");
        strlcpy(active_ssid, CONFIG_ROCK_WIFI_SSID, sizeof(active_ssid));
        strlcpy(active_password, CONFIG_ROCK_WIFI_PASSWORD, sizeof(active_password));
        has_creds = true;
        creds_from_sdkconfig = true;
    }

    if (!has_creds) {
        ESP_LOGI(TAG, "No Wi-Fi credentials in sdkconfig or NVS. Launching onboarding...");
        rock_platform_notify_splash_status("Starting Wi-Fi setup...");
        rock_onboarding_run();
        return -1;
    }

    wifi_config_t wifi = {0};
    strlcpy((char *)wifi.sta.ssid, active_ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, active_password, sizeof(wifi.sta.password));
    wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    wifi.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi.sta.failure_retry_cnt = 5;
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi) != ESP_OK) return -1;
    vTaskDelay(pdMS_TO_TICKS(500));

    // Keep valid credentials through transient AP outages and retry with backoff.
    rock_platform_notify_splash_status("Connecting to Wi-Fi...");
    if (!rock_wifi_connect_with_backoff(rock_uptime_ms() + 600000)) {
        ESP_LOGW(TAG, "STA unavailable for 10 minutes; opening onboarding with credentials preserved");
        rock_onboarding_run();
        return -1;
    }

    ESP_LOGI(TAG, "Wi-Fi connection established");
    if (creds_from_sdkconfig) {
        rock_wifi_storage_save(active_ssid, active_password);
    }
    if (rock_wifi_start_reconnect_task() != 0) return -1;
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp) != ESP_OK || esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync failed or timed out; continuing in background");
    }

    // OTA validation: cancel rollback now that network is functional
    rock_ota_mark_valid();

    // Check for OTA update if enabled
#if CONFIG_ROCK_OTA_CHECK_ON_BOOT
    if (CONFIG_ROCK_OTA_URL[0] != '\0') {
        rock_ota_check_start(CONFIG_ROCK_OTA_URL, false);
    }
#endif

    if (!rock_ws_init()) return -1;

    if (CONFIG_ROCKSERVER_BASE_URL[0] != '\0') {
        char response[128] = {0};
        int status = 0;
        ESP_LOGI(TAG, "Checking RockServer health at %s/health/ready...", CONFIG_ROCKSERVER_BASE_URL);
        int ret = rock_http_request("GET", "/health/ready", NULL, NULL, response, sizeof(response), &status);
        ESP_LOGI(TAG, "RockServer health result: ret=%d, status=%d, body='%s'", ret, status, response);
        if (ret < 0 || status / 100 != 2) {
            ESP_LOGW(TAG, "RockServer health check failed; connection loop will retry");
        }
    } else {
        ESP_LOGI(TAG, "CONFIG_ROCKSERVER_BASE_URL not configured; skipping RockServer health check");
    }

    return 0;
}

// NVS-backed device identity: core platform composition data; a separate file
// for ~30 lines would not pay for itself.
int rock_identity_load(char *id, size_t id_cap, char *secret, size_t secret_cap)
{
    nvs_handle_t nvs;
    if (nvs_open("rock_auth", NVS_READONLY, &nvs) != ESP_OK) return 0;
    size_t id_len = id_cap, secret_len = secret_cap;
    esp_err_t a = nvs_get_str(nvs, "device_id", id, &id_len);
    esp_err_t b = nvs_get_str(nvs, "secret", secret, &secret_len);
    nvs_close(nvs);
    return a == ESP_OK && b == ESP_OK ? 1 : 0;
}

int rock_identity_save(const char *id, const char *secret)
{
    if (!id || !secret || strlen(id) > 63 || strlen(secret) > 159) return -1;
    nvs_handle_t nvs;
    if (nvs_open("rock_auth", NVS_READWRITE, &nvs) != ESP_OK) return -1;
    esp_err_t err = nvs_set_str(nvs, "device_id", id);
    if (err == ESP_OK) err = nvs_set_str(nvs, "secret", secret);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK ? 0 : -1;
}

int rock_identity_clear(void)
{
    nvs_handle_t nvs;
    if (nvs_open("rock_auth", NVS_READWRITE, &nvs) != ESP_OK) return -1;
    esp_err_t err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Cleared rock_auth NVS partition (err=%d)", (int)err);
    return err == ESP_OK ? 0 : -1;
}

int rock_now_rfc3339(char *out, size_t cap)
{
    time_t current; time(&current);
    struct tm utc;
    if (!gmtime_r(&current, &utc) || strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) return -1;
    return 0;
}

void rock_random_fill(uint8_t *out, size_t len) { esp_fill_random(out, len); }
const char *rock_server_base_url(void) { return CONFIG_ROCKSERVER_BASE_URL; }

void rock_hosted_recover(void)
{
    ESP_LOGW(TAG, "Resetting unresponsive ESP32-C6 on GPIO54");
    gpio_set_direction(ROCK_C6_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ROCK_C6_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(ROCK_C6_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

uint64_t rock_uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}
