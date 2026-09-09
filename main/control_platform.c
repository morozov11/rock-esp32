#include "control_platform.h"
#include "display_bsp.h"
#include "rock_ota.h"
#include "rock_wifi_storage.h"
#include "rock_onboarding.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define WIFI_READY BIT0
#define WIFI_FAILED BIT1
#define WS_CONNECTED BIT0
#define WS_DISCONNECTED BIT1
#define ROCK_MAX_FRAME 65536
#define ROCK_HTTP_TIMEOUT_MS 10000
#define ROCK_C6_RESET_GPIO GPIO_NUM_54

static const char *TAG = "rock-control";
static EventGroupHandle_t s_wifi_events;
static EventGroupHandle_t s_ws_events;
static int s_wifi_retries;
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_ws_queue;
static char s_ws_frame[ROCK_MAX_FRAME + 1];
static size_t s_ws_length;
static char s_ws_headers[640];

typedef struct { char *data; size_t cap; size_t len; bool overflow; } http_sink_t;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Connected explicitly after boot Wi-Fi scan
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_wifi_retries <= 10) esp_wifi_connect(); else xEventGroupSetBits(s_wifi_events, WIFI_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_READY);
    }
}

int rock_wifi_scan_and_show(void)
{
    ESP_LOGI(TAG, "Starting Wi-Fi scan...");
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 120,
                .max = 350,
            },
            .passive = 350,
        },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        rock_ui_wifi_scan_show(NULL, 0);
        return -1;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Wi-Fi scan finished: found %u access points", ap_count);

    uint16_t fetch_count = ap_count > 30 ? 30 : ap_count;
    wifi_ap_record_t *records = NULL;
    rock_wifi_scan_item_t *items = NULL;
    if (fetch_count > 0) {
        records = calloc(fetch_count, sizeof(wifi_ap_record_t));
        items = calloc(fetch_count, sizeof(rock_wifi_scan_item_t));
        if (records && items) {
            esp_wifi_scan_get_ap_records(&fetch_count, records);
            for (uint16_t i = 0; i < fetch_count; i++) {
                strlcpy(items[i].ssid, (const char *)records[i].ssid, sizeof(items[i].ssid));
                items[i].rssi = records[i].rssi;
                items[i].authmode = (uint8_t)records[i].authmode;
                items[i].channel = records[i].primary;
                ESP_LOGI(TAG, "  [%2u] SSID: %-32s | RSSI: %3d dBm | CH: %2u | Auth: %u",
                         i + 1, items[i].ssid, items[i].rssi, items[i].channel, items[i].authmode);
            }
        }
    }

    rock_ui_wifi_scan_show(items, ap_count);

    if (items) free(items);
    if (records) free(records);
    return 0;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_sink_t *sink = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && sink && event->data_len > 0) {
        size_t incoming = (size_t)event->data_len;
        if (sink->len + incoming >= sink->cap) { sink->overflow = true; return ESP_FAIL; }
        memcpy(sink->data + sink->len, event->data, incoming);
        sink->len += incoming;
        sink->data[sink->len] = '\0';
    }
    return ESP_OK;
}

static int http_request(const char *method, const char *path, const char *bearer,
                        const char *body, char *response, size_t response_cap, int *status)
{
    if (!path || !response || response_cap < 2 || !status) return -1;
    char url[512];
    int written = snprintf(url, sizeof(url), "%s%s", CONFIG_ROCKSERVER_BASE_URL, path);
    if (written <= 0 || (size_t)written >= sizeof(url)) return -1;
    http_sink_t sink = {.data = response, .cap = response_cap};
    response[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_event, .user_data = &sink,
        .timeout_ms = ROCK_HTTP_TIMEOUT_MS, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }
    char authorization[600];
    if (bearer) {
        written = snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
        if (written <= 0 || (size_t)written >= sizeof(authorization)) { esp_http_client_cleanup(client); return -1; }
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || sink.overflow) return -1;
    return (int)sink.len;
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg; (void)base;
    int result;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        xEventGroupSetBits(s_ws_events, WS_CONNECTED);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *event = event_data;
        if (event->op_code != 1 || event->payload_len > ROCK_MAX_FRAME || event->payload_offset < 0 ||
            event->data_len < 0 || event->payload_offset + event->data_len > ROCK_MAX_FRAME) {
            result = event->op_code == 1 ? -2 : -3;
            xQueueOverwrite(s_ws_queue, &result);
            return;
        }
        if (event->payload_offset == 0) s_ws_length = 0;
        memcpy(s_ws_frame + event->payload_offset, event->data_ptr, event->data_len);
        s_ws_length = (size_t)(event->payload_offset + event->data_len);
        if (event->fin && s_ws_length == (size_t)event->payload_len) {
            s_ws_frame[s_ws_length] = '\0';
            result = (int)s_ws_length;
            xQueueOverwrite(s_ws_queue, &result);
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        xEventGroupSetBits(s_ws_events, WS_DISCONNECTED);
        result = -1;
        xQueueOverwrite(s_ws_queue, &result);
    }
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
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events || !esp_netif_create_default_wifi_sta()) return -1;
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) return -1;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) return -1;

    // Perform initial Wi-Fi scan and display available networks
    rock_wifi_scan_and_show();

    // Check credentials hierarchy
    char active_ssid[33] = {0};
    char active_password[65] = {0};
    bool has_creds = false;
    bool creds_from_sdkconfig = false;

    if (CONFIG_ROCK_WIFI_SSID[0] != '\0') {
        ESP_LOGI(TAG, "Using Wi-Fi credentials from sdkconfig");
        strlcpy(active_ssid, CONFIG_ROCK_WIFI_SSID, sizeof(active_ssid));
        strlcpy(active_password, CONFIG_ROCK_WIFI_PASSWORD, sizeof(active_password));
        has_creds = true;
        creds_from_sdkconfig = true;
    } else if (rock_wifi_storage_load(active_ssid, sizeof(active_ssid), active_password, sizeof(active_password))) {
        ESP_LOGI(TAG, "Using Wi-Fi credentials from NVS rock_wifi");
        has_creds = true;
    }

    if (!has_creds) {
        ESP_LOGI(TAG, "No Wi-Fi credentials in sdkconfig or NVS. Launching onboarding...");
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
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Connect to configured Wi-Fi AP
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_READY | WIFI_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_READY)) {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
        if (!creds_from_sdkconfig) {
            ESP_LOGW(TAG, "Clearing invalid NVS credentials and restarting onboarding...");
            rock_wifi_storage_clear();
            rock_onboarding_run();
        }
        return -1;
    }

    ESP_LOGI(TAG, "Wi-Fi connection established");
    if (creds_from_sdkconfig) {
        rock_wifi_storage_save(active_ssid, active_password);
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp) != ESP_OK || esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync failed or timed out; continuing");
    }

    // OTA validation: cancel rollback now that network is functional
    rock_ota_mark_valid();

    // Check for OTA update if enabled
#if CONFIG_ROCK_OTA_CHECK_ON_BOOT
    if (CONFIG_ROCK_OTA_URL[0] != '\0') {
        rock_ota_check_start(CONFIG_ROCK_OTA_URL, false);
    }
#endif

    if (CONFIG_ROCKSERVER_BASE_URL[0] != '\0') {
        char response[128]; int status;
        if (http_request("GET", "/health/ready", NULL, NULL, response, sizeof(response), &status) < 0 || status / 100 != 2) return -1;
    } else {
        ESP_LOGI(TAG, "CONFIG_ROCKSERVER_BASE_URL not configured; skipping RockServer health check");
    }

    s_ws_queue = xQueueCreate(1, sizeof(int));
    s_ws_events = xEventGroupCreate();
    return s_ws_queue && s_ws_events ? 0 : -1;
}

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

int rock_http_post(const char *path, const char *bearer, const char *body, char *response, size_t cap, int *status)
{ return http_request("POST", path, bearer, body, response, cap, status); }

int rock_ws_start(const char *access_token)
{
    if (!access_token || s_ws) return -1;
    char uri[512];
    const char *base = CONFIG_ROCKSERVER_BASE_URL;
    const char *suffix = strstr(base, "://");
    if (!suffix) return -1;
    int prefix = strncmp(base, "https://", 8) == 0 ? snprintf(uri, sizeof(uri), "wss://%s/api/v1/devices/connect", base + 8)
                                                    : snprintf(uri, sizeof(uri), "ws://%s/api/v1/devices/connect", base + (strncmp(base, "http://", 7) == 0 ? 7 : 0));
    if (prefix <= 0 || (size_t)prefix >= sizeof(uri) || snprintf(s_ws_headers, sizeof(s_ws_headers), "Authorization: Bearer %s\r\n", access_token) >= (int)sizeof(s_ws_headers)) return -1;
    esp_websocket_client_config_t cfg = {
        .uri = uri, .disable_auto_reconnect = true, .task_stack = 8192, .buffer_size = 4096,
        .headers = s_ws_headers, .network_timeout_ms = 10000, .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return -1;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
    xEventGroupClearBits(s_ws_events, WS_CONNECTED | WS_DISCONNECTED);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return -1;
    }
    EventBits_t bits = xEventGroupWaitBits(s_ws_events, WS_CONNECTED | WS_DISCONNECTED,
                                            pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (bits & WS_CONNECTED) return 0;
    rock_ws_stop();
    return -1;
}

void rock_ws_stop(void)
{
    if (s_ws) { esp_websocket_client_stop(s_ws); esp_websocket_client_destroy(s_ws); s_ws = NULL; }
    memset(s_ws_headers, 0, sizeof(s_ws_headers));
    xQueueReset(s_ws_queue);
}

int rock_ws_send(const char *data, size_t len)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws) || len > ROCK_MAX_FRAME) return -1;
    return esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(10000));
}

int rock_ws_receive(char *data, size_t cap, uint32_t timeout_ms)
{
    int result;
    if (xQueueReceive(s_ws_queue, &result, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
    if (result <= 0) return result;
    if ((size_t)result >= cap) return -2;
    memcpy(data, s_ws_frame, result);
    data[result] = '\0';
    return result;
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
    esp_restart();
}
