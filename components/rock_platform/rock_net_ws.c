// HTTP/WSS client plumbing split out of control_platform.c (phase 4): the
// RockServer REST wrappers and the websocket client with its frame queue.
// Public ABI: control_platform.h; sequencing: rock_platform_init.

#include "control_platform.h"
#include "control_platform_internal.h"
#include "rock_protocol_abi.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "rock-control";

#define WS_CONNECTED BIT0
#define WS_DISCONNECTED BIT1
#define ROCK_HTTP_TIMEOUT_MS 10000

static EventGroupHandle_t s_ws_events;
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_ws_queue;
static char s_ws_frame[ROCK_ABI_MAX_FRAME_BYTES + 1];
static size_t s_ws_length;
static char s_ws_headers[640];

typedef struct { char *data; size_t cap; size_t len; bool overflow; } http_sink_t;

bool rock_ws_init(void)
{
    s_ws_queue = xQueueCreate(1, sizeof(int));
    s_ws_events = xEventGroupCreate();
    return s_ws_queue != NULL && s_ws_events != NULL;
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

int rock_http_request(const char *method, const char *path, const char *bearer,
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
    int ret = ((err == ESP_OK || *status > 0) && !sink.overflow) ? (int)sink.len : -1;
    ESP_LOGI(TAG, "HTTP %s %s -> status=%d err=%s len=%u",
             method, path, *status, esp_err_to_name(err), (unsigned)sink.len);
    esp_http_client_cleanup(client);
    return ret;
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg; (void)base;
    int result;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WSS connected uptime_ms=%llu", (unsigned long long)(esp_timer_get_time() / 1000));
        xEventGroupSetBits(s_ws_events, WS_CONNECTED);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *event = event_data;
        if (event->op_code == 9 || event->op_code == 10) {
            ESP_LOGI(TAG, "WSS control opcode=%u handled by client", event->op_code);
            return;
        }
        if (event->op_code == 8) {
            ESP_LOGW(TAG, "WSS close frame received");
            xEventGroupSetBits(s_ws_events, WS_DISCONNECTED);
            result = -1;
            xQueueOverwrite(s_ws_queue, &result);
            return;
        }
        if (event->op_code != 1 || event->payload_len > ROCK_ABI_MAX_FRAME_BYTES || event->payload_offset < 0 ||
            event->data_len < 0 || event->payload_offset + event->data_len > ROCK_ABI_MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "WSS unsupported frame opcode=%u len=%d", event->op_code, event->payload_len);
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
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_CLOSED) {
        const esp_websocket_event_data_t *event = event_data;
        if (event) {
            ESP_LOGW(TAG, "WSS %s type=%d close=%d http=%d uptime_ms=%llu",
                     id == WEBSOCKET_EVENT_ERROR ? "error" : id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected",
                     event->error_handle.error_type, event->close_status_code,
                     event->error_handle.esp_ws_handshake_status_code,
                     (unsigned long long)(esp_timer_get_time() / 1000));
            if (event->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGW(TAG, "WSS transport esp=%s tls=%d verify=%d errno=%d",
                         esp_err_to_name(event->error_handle.esp_tls_last_esp_err),
                         event->error_handle.esp_tls_stack_err,
                         event->error_handle.esp_tls_cert_verify_flags,
                         event->error_handle.esp_transport_sock_errno);
            }
        }
        xEventGroupSetBits(s_ws_events, WS_DISCONNECTED);
        result = -1;
        xQueueOverwrite(s_ws_queue, &result);
    }
}

int rock_http_post(const char *path, const char *bearer, const char *body, char *response, size_t cap, int *status)
{ return rock_http_request("POST", path, bearer, body, response, cap, status); }

int rock_http_get(const char *path, const char *bearer, char *response, size_t cap, int *status)
{ return rock_http_request("GET", path, bearer, NULL, response, cap, status); }

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
        .headers = s_ws_headers, .network_timeout_ms = 10000,
        // Heartbeat cadence from rock_protocol_abi.h. The pingpong timeout is
        // 15 s tighter than the Rust session watchdog, so the C client's own
        // reconnect fires first; the Rust watchdog
        // (ROCK_ABI_HEARTBEAT_ACK_TIMEOUT_MS) stays the last line of defence.
        .ping_interval_sec = ROCK_ABI_HEARTBEAT_MS / 1000,
        .pingpong_timeout_sec = (ROCK_ABI_HEARTBEAT_ACK_TIMEOUT_MS - 15000) / 1000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return -1;
    ESP_LOGI(TAG, "WSS start uptime_ms=%llu", (unsigned long long)(esp_timer_get_time() / 1000));
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
    ESP_LOGW(TAG, "WSS start failed bits=0x%lx uptime_ms=%llu", (unsigned long)bits,
             (unsigned long long)(esp_timer_get_time() / 1000));
    rock_ws_stop();
    return -1;
}

void rock_ws_stop(void)
{
    ESP_LOGI(TAG, "WSS local stop uptime_ms=%llu", (unsigned long long)(esp_timer_get_time() / 1000));
    if (s_ws) { esp_websocket_client_stop(s_ws); esp_websocket_client_destroy(s_ws); s_ws = NULL; }
    memset(s_ws_headers, 0, sizeof(s_ws_headers));
    xQueueReset(s_ws_queue);
}

int rock_ws_send(const char *data, size_t len)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws) || len > ROCK_ABI_MAX_FRAME_BYTES) return -1;
    int sent = esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(10000));
    // Confidentiality: never log raw frames — device.command carries
    // stream_uri with ?token=… (transport.rs contract). Frame-kind taxonomy
    // belongs to Rust; it logs the kind after parsing the envelope.
    ESP_LOGI(TAG, "WSS send result=%d", sent);
    return sent;
}

int rock_ws_receive(char *data, size_t cap, uint32_t timeout_ms)
{
    int result;
    if (xQueueReceive(s_ws_queue, &result, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
    if (result <= 0) return result;
    if ((size_t)result >= cap) return -2;
    memcpy(data, s_ws_frame, result);
    data[result] = '\0';
    // Confidentiality: never log raw frames — device.command carries
    // stream_uri with ?token=… (transport.rs contract). The frame kind is
    // logged by the Rust session loop (rock_ws_log_rx_frame) once parsed.
    ESP_LOGI(TAG, "WSS receive len=%d", result);
    return result;
}

void rock_ws_log_rx_frame(const char *kind)
{
    // Called from the Rust session loop with the parsed envelope kind only —
    // never frame content (phase-1 confidentiality contract).
    ESP_LOGI(TAG, "WSS receive type=%s", kind ? kind : "unknown");
}

void rock_ws_log_heartbeat_timeout(uint64_t age_ms)
{
    ESP_LOGW(TAG, "WSS heartbeat ACK timeout age_ms=%llu", (unsigned long long)age_ms);
}
