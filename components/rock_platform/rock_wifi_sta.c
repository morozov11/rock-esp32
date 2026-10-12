// Wi-Fi STA domain split out of control_platform.c (phase 4): event handling,
// RF survey, connect/reconnect policy, scanning and link status queries.
// Public ABI: control_platform.h; sequencing: rock_platform_init.

#include "control_platform.h"
#include "control_platform_internal.h"
#include "rock_source.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-control";

#define WIFI_READY BIT0
#define WIFI_FAILED BIT1

static EventGroupHandle_t s_wifi_events;
static atomic_int s_last_wifi_rssi = ATOMIC_VAR_INIT(-128);
static atomic_int s_last_wifi_channel = ATOMIC_VAR_INIT(0);
static uint64_t s_last_wifi_query_ms = 0;
static uint32_t s_wifi_query_backoff_ms = 10000;

static char s_sta_ip[32] = {0};

bool rock_wifi_events_create(void)
{
    s_wifi_events = xEventGroupCreate();
    return s_wifi_events != NULL;
}

static const char *wifi_reason_desc(unsigned reason)
{
    switch (reason) {
    case 2: return "auth-expire";
    case 4: return "assoc-expire";
    case 8: return "assoc-leave";
    case 15: return "4way-handshake-timeout";
    case 200: return "beacon-timeout";
    case 201: return "no-ap-found";
    case 202: return "auth-fail";
    case 203: return "assoc-fail";
    case 204: return "handshake-timeout";
    case 205: return "connection-fail";
    default: return "other";
    }
}

int rock_get_local_ip(char *out, size_t cap)
{
    if (!out || cap == 0) return -1;
    if (s_sta_ip[0] == '\0') return -1;
    strncpy(out, s_sta_ip, cap - 1);
    out[cap - 1] = '\0';
    return 0;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    static bool s_sta_associated = false;
    static uint64_t s_sta_since_ms = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Connected explicitly after boot Wi-Fi scan
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_ip[0] = '\0';
        atomic_store(&s_last_wifi_rssi, -128);
        const wifi_event_sta_disconnected_t *event = data;
        unsigned reason = event ? event->reason : 0;
        if (s_sta_associated) {
            ESP_LOGW(TAG, "STA link lost reason=%u (%s) after %llu s associated",
                     reason, wifi_reason_desc(reason),
                     (unsigned long long)((rock_uptime_ms() - s_sta_since_ms) / 1000));
            s_sta_associated = false;
        } else {
            ESP_LOGW(TAG, "STA reconnect attempt failed reason=%u (%s)",
                     reason, wifi_reason_desc(reason));
        }
        xEventGroupClearBits(s_wifi_events, WIFI_READY);
        xEventGroupSetBits(s_wifi_events, WIFI_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(s_wifi_events, WIFI_FAILED);
        xEventGroupSetBits(s_wifi_events, WIFI_READY);
        s_sta_associated = true;
        s_sta_since_ms = rock_uptime_ms();
        esp_wifi_set_ps(WIFI_PS_NONE);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        if (event) {
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            char ip_buf[48];
            snprintf(ip_buf, sizeof(ip_buf), "Connected! IP: %s", s_sta_ip);
            rock_platform_notify_splash_status(ip_buf);
        }
    }
}

void rock_wifi_register_event_handlers(void)
{
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
}

// Rate-limited all-channel survey used while STA cannot associate.
// Reports neighbour density per channel and whether the configured network
// is visible, without logging any SSID.
static void wifi_rf_survey(void)
{
    static uint64_t s_last_ms = 0;
    uint64_t now_ms = rock_uptime_ms();
    if (s_last_ms != 0 && now_ms - s_last_ms < 30000) return;
    s_last_ms = now_ms;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 120, .max = 350 },
            .passive = 350,
        },
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) return;
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    uint16_t fetch = ap_count > 64 ? 64 : ap_count;
    if (fetch == 0) {
        ESP_LOGW(TAG, "RF survey: radio silent, no networks visible");
        return;
    }
    wifi_ap_record_t *records = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!records) return;
    esp_wifi_scan_get_ap_records(&fetch, records);

    wifi_config_t cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    uint16_t per_channel[14] = {0};
    int target_channel = 0;
    int target_rssi = 0;
    for (uint16_t i = 0; i < fetch; i++) {
        if (records[i].primary >= 1 && records[i].primary <= 13) {
            per_channel[records[i].primary]++;
        }
        if (target_channel == 0 && cfg.sta.ssid[0] != '\0' &&
            records[i].ssid[0] != '\0' &&
            strncmp((const char *)records[i].ssid, (const char *)cfg.sta.ssid,
                    sizeof(cfg.sta.ssid)) == 0) {
            target_channel = records[i].primary;
            target_rssi = records[i].rssi;
        }
    }
    free(records);

    if (target_channel > 0) {
        ESP_LOGW(TAG, "RF survey: aps=%u ch 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u 9:%u 10:%u 11:%u 12:%u 13:%u | target ch=%d rssi=%d",
                 (unsigned)fetch,
                 per_channel[1], per_channel[2], per_channel[3], per_channel[4],
                 per_channel[5], per_channel[6], per_channel[7], per_channel[8],
                 per_channel[9], per_channel[10], per_channel[11], per_channel[12],
                 per_channel[13], target_channel, target_rssi);
    } else {
        ESP_LOGW(TAG, "RF survey: aps=%u ch 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u 9:%u 10:%u 11:%u 12:%u 13:%u | target not visible",
                 (unsigned)fetch,
                 per_channel[1], per_channel[2], per_channel[3], per_channel[4],
                 per_channel[5], per_channel[6], per_channel[7], per_channel[8],
                 per_channel[9], per_channel[10], per_channel[11], per_channel[12],
                 per_channel[13]);
    }
}

bool rock_wifi_connect_with_backoff(uint64_t deadline_ms)
{
    uint32_t backoff_ms = 0;
    int fail_count = 0;
    do {
        xEventGroupClearBits(s_wifi_events, WIFI_FAILED);
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_READY | WIFI_FAILED,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
            if (bits & WIFI_READY) {
                esp_wifi_set_ps(WIFI_PS_NONE);
                return true;
            }
        } else {
            ESP_LOGW(TAG, "STA connect request failed: %s", esp_err_to_name(err));
        }
        fail_count++;
        if (fail_count >= 5) {
            ESP_LOGW(TAG, "STA failed %d times; resetting ESP32-C6 co-processor...", fail_count);
            rock_hosted_recover();
            fail_count = 0;
        }
        if (rock_uptime_ms() >= deadline_ms) break;
        wifi_rf_survey();
        if (backoff_ms > 0) {
            ESP_LOGW(TAG, "STA retry in %lu ms", (unsigned long)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }
        backoff_ms = backoff_ms == 0 ? 250 : (backoff_ms * 2 > 2000 ? 2000 : backoff_ms * 2);
    } while (rock_uptime_ms() < deadline_ms);
    return false;
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_wifi_events, WIFI_FAILED, pdTRUE, pdFALSE, portMAX_DELAY);
        rock_wifi_connect_with_backoff(UINT64_MAX);
    }
}

int rock_wifi_start_reconnect_task(void)
{
    return xTaskCreate(wifi_reconnect_task, "wifi_retry", 3072, NULL, 4, NULL) == pdPASS ? 0 : -1;
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
        rock_platform_notify_wifi_scan(NULL, 0);
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
            }
        }
    }

    rock_platform_notify_wifi_scan(items, ap_count);

    if (items) free(items);
    if (records) free(records);
    return 0;
}

bool rock_wifi_is_connected(void)
{
    return s_wifi_events != NULL && ((xEventGroupGetBits(s_wifi_events) & WIFI_READY) != 0);
}

int rock_wifi_rssi(void)
{
    if (!rock_wifi_is_connected()) return -128;
    // When audio relay is active, NEVER query AP info over SDIO RPC.
    // The concurrent upstream/downstream audio saturates the ESP32-C6 coprocessor,
    // causing 5000 ms RPC timeouts that freeze the main loop and drop the WSS session.
    if (rock_source_is_active()) {
        return atomic_load(&s_last_wifi_rssi);
    }
    uint64_t now = rock_uptime_ms();
    if (now - s_last_wifi_query_ms >= s_wifi_query_backoff_ms) {
        s_last_wifi_query_ms = now;
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            atomic_store(&s_last_wifi_rssi, ap_info.rssi);
            atomic_store(&s_last_wifi_channel, ap_info.primary);
            s_wifi_query_backoff_ms = 30000;
            return ap_info.rssi;
        } else {
            s_wifi_query_backoff_ms = 60000;
        }
    }
    return atomic_load(&s_last_wifi_rssi);
}

int rock_wifi_channel(void)
{
    return atomic_load(&s_last_wifi_channel);
}
