#include "cast_bsp.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "mdns.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "cast_bsp";

struct rock_cast_tls {
    esp_tls_t *tls;
};

// ---------------------------------------------------------------------------
// HTTP eureka_info Reader (Port 8008)
// ---------------------------------------------------------------------------

static bool fetch_eureka_info(const char *ip_str, char *out_name, size_t name_len,
                              char *out_model, size_t model_len, char *out_id, size_t id_len)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 }; // 500ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in saddr = { 0 };
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(8008);
    inet_pton(AF_INET, ip_str, &saddr.sin_addr);

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(sock);
        return false;
    }

    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "GET /setup/eureka_info?params=name,device_info,build_info HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "User-Agent: RockCast/0.1\r\n\r\n", ip_str);
    send(sock, req, req_len, 0);

    char buf[1536];
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);

    if (n <= 0) return false;
    buf[n] = '\0';

    // Parse "name":"..."
    char *p = strstr(buf, "\"name\":");
    if (p) {
        p += 7;
        while (*p == ' ' || *p == '\"') p++;
        char *end = strchr(p, '\"');
        if (end) {
            size_t len = end - p;
            if (len > name_len - 1) len = name_len - 1;
            strncpy(out_name, p, len);
            out_name[len] = '\0';
        }
    }

    // Parse "ssdp_udn":"..." or "mac_address":"..."
    p = strstr(buf, "\"ssdp_udn\":");
    if (!p) p = strstr(buf, "\"mac_address\":");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '\"') p++;
            char *end = strchr(p, '\"');
            if (end) {
                size_t len = end - p;
                if (len > id_len - 1) len = id_len - 1;
                strncpy(out_id, p, len);
                out_id[len] = '\0';
            }
        }
    }

    // Parse model_name or manufacturer
    p = strstr(buf, "\"model_name\":");
    if (!p) p = strstr(buf, "\"manufacturer\":");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '\"') p++;
            char *end = strchr(p, '\"');
            if (end) {
                size_t len = end - p;
                if (len > model_len - 1) len = model_len - 1;
                strncpy(out_model, p, len);
                out_model[len] = '\0';
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Unicast Subnet Scan Fallback (Port 8009)
// ---------------------------------------------------------------------------

static size_t cast_subnet_scan(rock_cast_device_t *out_devices, size_t max_devices, size_t current_count)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return current_count;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return current_count;
    }

    uint32_t my_ip = ntohl(ip_info.ip.addr);
    uint32_t subnet = my_ip & 0xFFFFFF00;
    uint8_t my_last = my_ip & 0xFF;

    // Order targets: first +/- 25 around our IP, then remaining 1..254
    uint8_t hosts[254];
    size_t num_hosts = 0;

    int min_local = (int)my_last - 25;
    if (min_local < 1) min_local = 1;
    int max_local = (int)my_last + 25;
    if (max_local > 254) max_local = 254;

    for (int h = min_local; h <= max_local; h++) {
        if (h != (int)my_last) {
            hosts[num_hosts++] = (uint8_t)h;
        }
    }
    size_t num_hosts_local = num_hosts;
    for (int h = 1; h <= 254; h++) {
        if (h < min_local || h > max_local) {
            hosts[num_hosts++] = (uint8_t)h;
        }
    }

    size_t count = current_count;
    #define BATCH_SIZE 6
    for (size_t i = 0; i < num_hosts && count < max_devices; i += BATCH_SIZE) {
        // If we already found devices in the local neighbourhood, don't waste time scanning the rest
        if (count > current_count && i >= num_hosts_local) {
            break;
        }

        int socks[BATCH_SIZE];
        uint32_t ips[BATCH_SIZE];
        size_t bsize = 0;
        int max_fd = -1;

        for (size_t b = 0; b < BATCH_SIZE && (i + b) < num_hosts; b++) {
            uint32_t target_ip = subnet | hosts[i + b];
            char target_str[16];
            snprintf(target_str, sizeof(target_str), "%u.%u.%u.%u",
                     (unsigned)((target_ip >> 24) & 0xFF),
                     (unsigned)((target_ip >> 16) & 0xFF),
                     (unsigned)((target_ip >> 8) & 0xFF),
                     (unsigned)(target_ip & 0xFF));

            bool already = false;
            for (size_t d = 0; d < count; d++) {
                if (strcmp(out_devices[d].host, target_str) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;

            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) continue;

            int flags = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_in saddr = { 0 };
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(8009);
            saddr.sin_addr.s_addr = htonl(target_ip);

            connect(s, (struct sockaddr *)&saddr, sizeof(saddr));
            socks[bsize] = s;
            ips[bsize] = target_ip;
            if (s > max_fd) max_fd = s;
            bsize++;
        }

        if (bsize > 0 && max_fd >= 0) {
            fd_set wfds;
            FD_ZERO(&wfds);
            for (size_t b = 0; b < bsize; b++) {
                FD_SET(socks[b], &wfds);
            }
            // 120ms for local neighbourhood, 60ms for remainder
            int timeout_us = (i < num_hosts_local) ? 120000 : 60000;
            struct timeval tv = { .tv_sec = 0, .tv_usec = timeout_us };
            int sel = select(max_fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                for (size_t b = 0; b < bsize && count < max_devices; b++) {
                    if (FD_ISSET(socks[b], &wfds)) {
                        int so_error = 0;
                        socklen_t len = sizeof(so_error);
                        if (getsockopt(socks[b], SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                            rock_cast_device_t *dev = &out_devices[count];
                            memset(dev, 0, sizeof(*dev));
                            dev->port = 8009;
                            snprintf(dev->host, sizeof(dev->host), "%u.%u.%u.%u",
                                     (unsigned)((ips[b] >> 24) & 0xFF),
                                     (unsigned)((ips[b] >> 16) & 0xFF),
                                     (unsigned)((ips[b] >> 8) & 0xFF),
                                     (unsigned)(ips[b] & 0xFF));

                            fetch_eureka_info(dev->host, dev->name, sizeof(dev->name),
                                              dev->model, sizeof(dev->model),
                                              dev->id, sizeof(dev->id));

                            if (dev->name[0] == '\0') {
                                snprintf(dev->name, sizeof(dev->name), "Chromecast %.48s", dev->host);
                            }
                            if (dev->id[0] == '\0') {
                                snprintf(dev->id, sizeof(dev->id), "%.48s:%u", dev->host, (unsigned)dev->port);
                            }

                            ESP_LOGI(TAG, "Subnet scan found Cast: '%s' [%s] at %s:%u (id: %s)",
                                     dev->name, dev->model, dev->host, dev->port, dev->id);
                            count++;
                        }
                    }
                }
            }
            for (size_t b = 0; b < bsize; b++) {
                close(socks[b]);
            }
        }
    }
    #undef BATCH_SIZE

    return count;
}

// ---------------------------------------------------------------------------
// Discovery API: mDNS + Unicast Fallback
// ---------------------------------------------------------------------------

int rock_cast_discover(uint32_t timeout_ms, rock_cast_device_t *out_devices, size_t max_devices)
{
    if (!out_devices || max_devices == 0) {
        return 0;
    }

    size_t count = 0;

    // 1. Try mDNS discovery first
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init failed: %d", err);
    }

    mdns_result_t *results = NULL;
    uint32_t mdns_time = timeout_ms < 2000 ? 2000 : (timeout_ms > 3500 ? 3500 : timeout_ms);
    err = mdns_query_ptr("_googlecast", "_tcp", mdns_time, max_devices, &results);
    if (err == ESP_OK && results) {
        mdns_result_t *cur = results;
        while (cur && count < max_devices) {
            rock_cast_device_t *dev = &out_devices[count];
            memset(dev, 0, sizeof(*dev));
            dev->port = cur->port ? cur->port : 8009;

            bool has_ip = false;
            mdns_ip_addr_t *a = cur->addr;
            while (a) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    esp_ip4addr_ntoa(&a->addr.u_addr.ip4, dev->host, sizeof(dev->host));
                    has_ip = true;
                    break;
                }
                a = a->next;
            }

            if (has_ip) {
                for (size_t t = 0; t < cur->txt_count; t++) {
                    const char *key = cur->txt[t].key;
                    const char *val = cur->txt[t].value;
                    if (!key || !val) continue;

                    if (strcasecmp(key, "fn") == 0) {
                        strncpy(dev->name, val, sizeof(dev->name) - 1);
                    } else if (strcasecmp(key, "md") == 0) {
                        strncpy(dev->model, val, sizeof(dev->model) - 1);
                    } else if (strcasecmp(key, "id") == 0) {
                        strncpy(dev->id, val, sizeof(dev->id) - 1);
                    }
                }

                if (dev->name[0] == '\0' && cur->instance_name) {
                    strncpy(dev->name, cur->instance_name, sizeof(dev->name) - 1);
                }
                if (dev->id[0] == '\0') {
                    if (cur->instance_name) {
                        strncpy(dev->id, cur->instance_name, sizeof(dev->id) - 1);
                    } else {
                        snprintf(dev->id, sizeof(dev->id), "%.48s:%u", dev->host, dev->port);
                    }
                }
                count++;
            }
            cur = cur->next;
        }
        mdns_query_results_free(results);
    }

    // 2. Unicast LAN subnet fallback if mDNS found 0 devices
    if (count == 0) {
        ESP_LOGI(TAG, "mDNS returned 0 devices; probing LAN subnet fallback (port 8009/8008)...");
        count = cast_subnet_scan(out_devices, max_devices, count);
    }

    ESP_LOGI(TAG, "Cast discovery total: %u devices", (unsigned)count);
    return (int)count;
}

// ---------------------------------------------------------------------------
// TLS Session Client
// ---------------------------------------------------------------------------

rock_cast_tls_t *rock_cast_tls_open(const char *host, uint16_t port, uint32_t timeout_ms)
{
    if (!host || port == 0) return NULL;

    rock_cast_tls_t *h = (rock_cast_tls_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->tls = esp_tls_init();
    if (!h->tls) {
        free(h);
        return NULL;
    }

    // Chromecast devices use self-signed certificates on port 8009.
    // CONFIG_ESP_TLS_INSECURE and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY allows
    // esp_tls without crt_bundle_attach to bypass certificate checks,
    // while global HTTPS and server WSS continue to verify via esp_crt_bundle_attach.
    uint32_t eff_timeout = timeout_ms < 10000 ? 10000 : timeout_ms;
    esp_tls_cfg_t cfg = {
        .skip_common_name = true,
        .timeout_ms = (int)eff_timeout,
        .non_block = false,
    };

    ESP_LOGI(TAG, "Connecting TLS to %s:%u (timeout %lu ms, internal free=%u, largest=%u)...",
             host, port, (unsigned long)eff_timeout,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    int ret = esp_tls_conn_new_sync(host, (int)strlen(host), (int)port, &cfg, h->tls);
    if (ret != 1) {
        esp_tls_error_handle_t err_handle = NULL;
        int tls_code = 0, flags = 0;
        esp_err_t last_error = ESP_OK;
        if (esp_tls_get_error_handle(h->tls, &err_handle) == ESP_OK && err_handle) {
            last_error = esp_tls_get_and_clear_last_error(err_handle, &tls_code, &flags);
        }
        ESP_LOGW(TAG, "esp_tls_conn_new_sync to %s:%u failed: ret=%d last_error=%s (0x%x) tls_code=0x%x flags=0x%x internal free=%u largest=%u",
                 host, port, ret, esp_err_to_name(last_error), last_error, tls_code, flags,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_tls_conn_destroy(h->tls);
        free(h);
        return NULL;
    }

    ESP_LOGI(TAG, "TLS connected to Chromecast at %s:%u", host, port);
    return h;
}

int rock_cast_tls_send(rock_cast_tls_t *h, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!h || !h->tls || !data || len == 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t ret = esp_tls_conn_write(h->tls, data + written, len - written);
        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGW(TAG, "esp_tls_conn_write failed: %d", (int)ret);
            return -1;
        }
    }
    return (int)written;
}

int rock_cast_tls_recv(rock_cast_tls_t *h, uint8_t *data, size_t max_len, uint32_t timeout_ms)
{
    if (!h || !h->tls || !data || max_len == 0) return -1;

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(h->tls, &sockfd) == ESP_OK && sockfd >= 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ssize_t ret = esp_tls_conn_read(h->tls, data, max_len);
    if (ret > 0) {
        return (int)ret;
    }
    if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_TIMEOUT) {
        return 0;
    }
    if (ret == 0) {
        ESP_LOGI(TAG, "Chromecast TLS connection closed by peer (EOF)");
        return -1; // connection closed
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
        return 0; // timeout
    }
    ESP_LOGW(TAG, "esp_tls_conn_read error: ret=%d errno=%d", (int)ret, errno);
    return -1;
}

void rock_cast_tls_close(rock_cast_tls_t *h)
{
    if (!h) return;
    if (h->tls) {
        esp_tls_conn_destroy(h->tls);
        h->tls = NULL;
    }
    free(h);
}

void rock_cast_log(const char *msg)
{
    if (msg) {
        ESP_LOGI(TAG, "%s", msg);
    }
}
