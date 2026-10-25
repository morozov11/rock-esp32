// Cast-side HTTP server split out of relay_bsp.c (phase 4): the TCP listener,
// request parsing and the client streaming loops (WAV PCM and chunked MP3
// passthrough). Public ABI: rock_source.h; shared plumbing: relay_internal.h.

#include "rock_source.h"
#include "control_platform.h"
#include "relay_internal.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *TAG = "rock_relay";

#define RELAY_CLIENT_READ_BYTES 8192
#define RELAY_MAX_CLIENTS 3

static const char RELAY_WAV_RESPONSE_HEADERS[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/wav\r\n"
    "Content-Length: 4294967339\r\n"
    "Cache-Control: no-cache, no-store\r\n"
    "Pragma: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Accept-Ranges: none\r\n"
    "X-Audio-Sample-Rate: 48000\r\n"
    "X-Audio-Channels: 2\r\n"
    "X-Audio-Byte-Rate: 192000\r\n"
    "\r\n";

// Chunked passthrough response, mirroring the desktop stream_response_headers.
static const char RELAY_FORMAT_PASSTHROUGH_HEADERS[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: %s\r\n"
    "Cache-Control: no-cache, no-store\r\n"
    "Pragma: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Accept-Ranges: none\r\n"
    "\r\n";

static int s_server_sock = -1;
static atomic_int s_client_count = 0;
static atomic_uint s_streamed_bytes = 0;
static atomic_uint s_overruns = 0;
static atomic_uint s_send_errors = 0;

static bool relay_read_request(int client_sock, char *req_buf, size_t req_cap, bool *is_head, bool *is_get)
{
    int req_len = 0;
    *is_head = false;
    *is_get = false;
    while (req_len < (int)req_cap - 1 && !relay_stopped()) {
        int n = recv(client_sock, req_buf + req_len, req_cap - 1 - req_len, 0);
        if (n <= 0) {
            return false;
        }
        req_len += n;
        req_buf[req_len] = '\0';
        if (strstr(req_buf, "\r\n\r\n")) {
            *is_head = (strncmp(req_buf, "HEAD ", 5) == 0);
            *is_get = (strncmp(req_buf, "GET ", 4) == 0);
            return true;
        }
    }
    return false;
}

// Chunked-transfer writer for passthrough clients (rockcast write_chunk).
static bool relay_write_chunk(
    int sock,
    const uint8_t *data,
    size_t len,
    size_t *total_bytes,
    size_t *last_log_bytes
)
{
    if (len == 0) {
        return true;
    }
    char prefix[16];
    int prefix_len = snprintf(prefix, sizeof(prefix), "%X\r\n", (unsigned)len);
    static const char suffix[] = "\r\n";
    return relay_send_all(sock, prefix, (size_t)prefix_len)
        && relay_send_audio(sock, data, len, total_bytes, last_log_bytes)
        && relay_send_all(sock, suffix, sizeof(suffix) - 1);
}

// One streaming loop for both serve modes (rockcast handle_client): join
// behind the live edge, clamp forward on overrun, stream until the source
// stops or the client socket fails. Passthrough adds chunked framing and
// MP3 frame alignment; WAV streams raw PCM bytes.
static void relay_serve_stream(int client_sock, uint8_t *chunk, bool passthrough)
{
    uint64_t pos = rock_ring_join_position(relay_ring(), relay_cushion_bytes());
    if (passthrough) {
        // Align the join position to the next MP3 frame so the receiver's
        // decoder never starts mid-frame.
        size_t n = 0;
        if (rock_ring_read(relay_ring(), pos, chunk, RELAY_CLIENT_READ_BYTES, &n) == ROCK_RING_READ_OK
            && n >= 4) {
            size_t offset = relay_find_mpeg_frame(chunk, n);
            pos += offset;
        }
    }

    size_t total_bytes = 0;
    size_t last_log_bytes = 0;
    while (!relay_stopped()) {
        size_t n = 0;
        rock_ring_read_result_t r = rock_ring_read(relay_ring(), pos, chunk, RELAY_CLIENT_READ_BYTES, &n);
        if (r == ROCK_RING_READ_OVERRUN) {
            atomic_fetch_add(&s_overruns, 1);
            pos = rock_ring_clamp_position(relay_ring(), pos, relay_cushion_bytes());
            continue;
        }
        if (r == ROCK_RING_READ_EMPTY || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool ok = passthrough
            ? relay_write_chunk(client_sock, chunk, n, &total_bytes, &last_log_bytes)
            : relay_send_audio(client_sock, chunk, n, &total_bytes, &last_log_bytes);
        if (!ok) {
            atomic_fetch_add(&s_send_errors, 1);
            break;
        }
        atomic_fetch_add(&s_streamed_bytes, n);
        pos += n;
    }
    ESP_LOGI(TAG, "Relay stream session closed (total: %u bytes)", (unsigned)total_bytes);
}

// Serve one connected Cast client from the shared ring (rockcast
// handle_client: join behind the live edge, clamp on overrun).
static void relay_serve_client(int client_sock)
{
    // The receiver may open its fetch socket seconds before it actually
    // sends GET (desktop relay allows 12 s); a short timeout here kills the
    // real player connection and leaves LOAD silently pending.
    struct timeval tv_rcv = { .tv_sec = 12, .tv_usec = 0 };
    struct timeval tv_snd = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));

    char *req_buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *chunk = heap_caps_malloc(RELAY_CLIENT_READ_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!req_buf || !chunk) {
        ESP_LOGE(TAG, "Failed to allocate relay client buffers");
        free(req_buf);
        free(chunk);
        close(client_sock);
        return;
    }

    bool is_head = false;
    bool is_get = false;
    if (!relay_read_request(client_sock, req_buf, 1024, &is_head, &is_get)) {
        ESP_LOGW(TAG, "Incomplete HTTP request from client");
        goto out;
    }
    if (!is_head && !is_get) {
        static const char not_found[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(client_sock, not_found, strlen(not_found), 0);
        goto out;
    }
    if (is_head) {
        if (relay_is_passthrough()) {
            char headers[256];
            int headers_len = snprintf(headers, sizeof(headers),
                RELAY_FORMAT_PASSTHROUGH_HEADERS, relay_content_type());
            relay_send_all(client_sock, headers, (size_t)headers_len);
        } else {
            relay_send_all(client_sock, RELAY_WAV_RESPONSE_HEADERS,
                strlen(RELAY_WAV_RESPONSE_HEADERS));
        }
        ESP_LOGI(TAG, "Responded to HEAD /stream with content-type %s", relay_content_type());
        goto out;
    }

    if (relay_is_passthrough()) {
        char headers[256];
        int headers_len = snprintf(headers, sizeof(headers),
            RELAY_FORMAT_PASSTHROUGH_HEADERS, relay_content_type());
        if (!relay_send_all(client_sock, headers, (size_t)headers_len)) {
            goto out;
        }
        ESP_LOGI(TAG, "Starting relay streaming (content-type: %s, passthrough)",
            relay_content_type());
        relay_serve_stream(client_sock, chunk, true);
        goto out;
    }

    if (!relay_send_all(client_sock, RELAY_WAV_RESPONSE_HEADERS, strlen(RELAY_WAV_RESPONSE_HEADERS))) {
        goto out;
    }
    {
        uint8_t wav_header[44];
        relay_make_wav_header(wav_header);
        if (!relay_send_all(client_sock, wav_header, sizeof(wav_header))) {
            goto out;
        }
        ESP_LOGI(TAG, "Starting relay streaming (content-type: audio/wav, 48000 Hz, 2 ch; preroll=%u)",
            (unsigned)rock_ring_buffered(relay_ring()));
        relay_serve_stream(client_sock, chunk, false);
    }

out:
    free(req_buf);
    free(chunk);
    close(client_sock);
}

static void relay_client_task(void *pv)
{
    relay_serve_client((int)(intptr_t)pv);
    atomic_fetch_sub(&s_client_count, 1);
    vTaskDelete(NULL);
}

void relay_listener_poll(void)
{
    if (s_server_sock < 0) {
        return;
    }
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(s_server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock < 0) {
        return;
    }
    if (atomic_load(&s_client_count) >= RELAY_MAX_CLIENTS) {
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
        send(client_sock, busy, strlen(busy), 0);
        close(client_sock);
        return;
    }
    char client_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    ESP_LOGI(TAG, "Relay client connected from %s:%d",
        client_ip, ntohs(client_addr.sin_port));

    atomic_fetch_add(&s_client_count, 1);
    BaseType_t ok = xTaskCreatePinnedToCore(relay_client_task, "rock_rc", 4096,
        (void *)(intptr_t)client_sock, 5, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn relay client task");
        close(client_sock);
        atomic_fetch_sub(&s_client_count, 1);
    }
}

void relay_listener_close(void)
{
    if (s_server_sock >= 0) {
        close(s_server_sock);
        s_server_sock = -1;
    }
}

void relay_listener_wake(void)
{
    int fd = s_server_sock;
    if (fd >= 0 && relay_is_running()) {
        shutdown(fd, 0);
    }
}

int relay_client_count(void)
{
    return atomic_load(&s_client_count);
}

void rock_relay_get_metrics(uint32_t *streamed_bytes, uint32_t *overruns,
                            uint32_t *send_errors, int *clients)
{
    if (streamed_bytes) *streamed_bytes = atomic_load(&s_streamed_bytes);
    if (overruns) *overruns = atomic_load(&s_overruns);
    if (send_errors) *send_errors = atomic_load(&s_send_errors);
    if (clients) *clients = atomic_load(&s_client_count);
}

void relay_clients_reset(void)
{
    atomic_store(&s_client_count, 0);
}

esp_err_t rock_relay_listen(char *out_relay_url, size_t out_len)
{
    if (!out_relay_url || out_len < 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!relay_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server_sock >= 0) {
        char local_ip[32] = {0};
        if (rock_get_local_ip(local_ip, sizeof(local_ip)) == 0) {
            snprintf(out_relay_url, out_len, "http://%s:%d/stream", local_ip, ROCK_RELAY_PORT);
            return ESP_OK;
        }
        return ESP_FAIL;
    }

    char local_ip[32] = {0};
    if (rock_get_local_ip(local_ip, sizeof(local_ip)) != 0) {
        ESP_LOGE(TAG, "Cannot start relay listener: local IP unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    s_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create relay socket: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Non-blocking listener: the feeder polls accept() between decode
    // iterations and must never stall the media pace.
    int flags = fcntl(s_server_sock, F_GETFL, 0);
    fcntl(s_server_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in saddr = { 0 };
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(ROCK_RELAY_PORT);

    if (bind(s_server_sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind relay socket on port %d: errno %d", ROCK_RELAY_PORT, errno);
        goto fail_socket;
    }

    if (listen(s_server_sock, 4) < 0) {
        ESP_LOGE(TAG, "Failed to listen on relay socket: errno %d", errno);
        goto fail_socket;
    }

    atomic_store(&s_client_count, 0);

    snprintf(out_relay_url, out_len, "http://%s:%d/stream", local_ip, ROCK_RELAY_PORT);
    ESP_LOGI(TAG, "Relay listener attached: %s", out_relay_url);
    return ESP_OK;

fail_socket:
    if (s_server_sock >= 0) {
        close(s_server_sock);
        s_server_sock = -1;
    }
    return ESP_FAIL;
}
