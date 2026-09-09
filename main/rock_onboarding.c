#include "rock_onboarding.h"
#include "rock_wifi_storage.h"
#include "display_bsp.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock-onboard";

extern int rock_qr_encode(const char *text, uint8_t *out_buf, size_t out_cap, uint16_t *out_width);

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task_handle = NULL;
static bool s_dns_running = false;
static char s_onboarding_pin[8] = {0};

static const char ONBOARDING_HTML[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>RockCast Wi-Fi Setup</title>\n"
"<style>\n"
"body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0F1115; color: #F5F7FA; margin: 0; padding: 24px 16px; display: flex; justify-content: center; }\n"
".card { background: #14171F; border: 1px solid #262C38; border-radius: 12px; padding: 24px; max-width: 400px; width: 100%; box-shadow: 0 4px 20px rgba(0,0,0,0.5); }\n"
"h1 { font-size: 22px; font-weight: 700; margin-top: 0; margin-bottom: 8px; color: #F5F7FA; }\n"
"p { font-size: 14px; color: #94A3B8; margin-top: 0; margin-bottom: 20px; line-height: 1.4; }\n"
"label { display: block; font-size: 13px; font-weight: 600; color: #CBD5E1; margin-bottom: 6px; }\n"
"input[type=\"text\"], input[type=\"password\"] { width: 100%; box-sizing: border-box; padding: 12px; font-size: 15px; border-radius: 8px; border: 1px solid #334155; background: #1E293B; color: #F8FAFC; margin-bottom: 16px; outline: none; }\n"
"input:focus { border-color: #38BDF8; }\n"
"button { width: 100%; padding: 14px; font-size: 16px; font-weight: 600; color: #0F1115; background: #38BDF8; border: none; border-radius: 8px; cursor: pointer; transition: background 0.2s; }\n"
"button:hover { background: #0EA5E9; }\n"
".badge { display: inline-block; padding: 4px 8px; font-size: 11px; font-weight: 700; border-radius: 4px; background: rgba(56, 189, 248, 0.15); color: #38BDF8; margin-bottom: 12px; }\n"
".pin-notice { font-size: 12px; color: #38BDF8; margin-top: -12px; margin-bottom: 16px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"card\">\n"
"<div class=\"badge\">ROCKCAST RADIO</div>\n"
"<h1>Wi-Fi Setup</h1>\n"
"<p>Connect RockCast to your local network by providing the SSID, password, and setup PIN shown on the device screen.</p>\n"
"<form method=\"POST\" action=\"/connect\">\n"
"<label for=\"pin\">Setup PIN (shown on device)</label>\n"
"<input type=\"text\" id=\"pin\" name=\"pin\" required placeholder=\"6-digit PIN\" maxlength=\"6\" pattern=\"[0-9]{6}\" autocomplete=\"off\">\n"
"<div class=\"pin-notice\">Required for device security. Check the 6 digits on the screen.</div>\n"
"<label for=\"ssid\">Network Name (SSID)</label>\n"
"<input type=\"text\" id=\"ssid\" name=\"ssid\" required placeholder=\"Network SSID\" maxlength=\"32\">\n"
"<label for=\"password\">Wi-Fi Password</label>\n"
"<input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Password (empty for open network)\" maxlength=\"64\">\n"
"<button type=\"submit\">Save and Connect</button>\n"
"</form>\n"
"</div>\n"
"<script>\n"
"const p = new URLSearchParams(window.location.search).get('pin');\n"
"if (p) document.getElementById('pin').value = p;\n"
"</script>\n"
"</body>\n"
"</html>\n";

static const char CONNECTED_HTML[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>Saved</title>\n"
"<style>body{font-family:sans-serif;background:#0F1115;color:#F5F7FA;padding:32px;text-align:center;}\n"
".card{background:#14171F;border:1px solid #262C38;border-radius:12px;padding:24px;display:inline-block;max-width:360px;}\n"
"h2{color:#4ADE80;margin-top:0;}</style></head>\n"
"<body><div class=\"card\"><h2>Credentials Saved</h2><p>RockCast is restarting to connect to your network. You may now close this window.</p></div></body></html>\n";

static const char FORBIDDEN_PIN_HTML[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>Invalid PIN</title>\n"
"<style>body{font-family:sans-serif;background:#0F1115;color:#F5F7FA;padding:32px;text-align:center;}\n"
".card{background:#14171F;border:1px solid #7F1D1D;border-radius:12px;padding:24px;display:inline-block;max-width:360px;}\n"
"h2{color:#F87171;margin-top:0;}a{color:#38BDF8;text-decoration:none;}</style></head>\n"
"<body><div class=\"card\"><h2>Invalid Setup PIN</h2><p>The 6-digit setup PIN was incorrect. Please check the device screen and try again.</p><p><a href=\"/\">Try again</a></p></div></body></html>\n";

static void url_decode(char *dst, const char *src, size_t dst_cap)
{
    size_t d = 0;
    while (*src && d + 1 < dst_cap) {
        if (*src == '%' && isxdigit((int)*(src + 1)) && isxdigit((int)*(src + 2))) {
            char hex[3] = { *(src + 1), *(src + 2), '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

static void reboot_task(void *pvParameter)
{
    (void)pvParameter;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "Rebooting into STA mode...");
    esp_restart();
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ONBOARDING_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    char buf[512];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char raw_pin[32] = {0};
    char raw_ssid[64] = {0};
    char raw_pwd[128] = {0};
    char *token = strtok(buf, "&");
    while (token) {
        if (strncmp(token, "pin=", 4) == 0) {
            strlcpy(raw_pin, token + 4, sizeof(raw_pin));
        } else if (strncmp(token, "ssid=", 5) == 0) {
            strlcpy(raw_ssid, token + 5, sizeof(raw_ssid));
        } else if (strncmp(token, "password=", 9) == 0) {
            strlcpy(raw_pwd, token + 9, sizeof(raw_pwd));
        }
        token = strtok(NULL, "&");
    }

    char pin[16] = {0};
    char ssid[33] = {0};
    char pwd[65] = {0};
    url_decode(pin, raw_pin, sizeof(pin));
    url_decode(ssid, raw_ssid, sizeof(ssid));
    url_decode(pwd, raw_pwd, sizeof(pwd));

    // Verify 6-digit setup PIN for anti-hijack protection
    if (strcmp(pin, s_onboarding_pin) != 0) {
        ESP_LOGW(TAG, "Onboarding rejected: invalid PIN provided");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, FORBIDDEN_PIN_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Setup PIN verified. Received Wi-Fi credentials (SSID length: %u)", (unsigned)strlen(ssid));

    if (!rock_wifi_storage_save(ssid, pwd)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    rock_ui_onboarding_status("Credentials saved! Restarting into network...");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONNECTED_HTML, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "rock_reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

static void dns_server_task(void *pvParameter)
{
    (void)pvParameter;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(53);

    if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS server started on port 53");
    s_dns_running = true;

    uint8_t rx_buf[512];
    uint8_t tx_buf[512];

    while (s_dns_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) {
            continue;
        }

        // Parse query and craft DNS response pointing to 192.168.4.1
        memcpy(tx_buf, rx_buf, len);
        tx_buf[2] = 0x81; // Flags: Response, Opcode 0, Authoritative
        tx_buf[3] = 0x80; // Flags: Recursion Available, No Error
        tx_buf[6] = 0x00; tx_buf[7] = 0x01; // Answer count: 1

        size_t tx_len = len;
        if (tx_len + 16 <= sizeof(tx_buf)) {
            tx_buf[tx_len++] = 0xc0; // Name pointer to question
            tx_buf[tx_len++] = 0x0c;
            tx_buf[tx_len++] = 0x00; tx_buf[tx_len++] = 0x01; // Type A
            tx_buf[tx_len++] = 0x00; tx_buf[tx_len++] = 0x01; // Class IN
            tx_buf[tx_len++] = 0x00; tx_buf[tx_len++] = 0x00; tx_buf[tx_len++] = 0x00; tx_buf[tx_len++] = 0x3c; // TTL 60
            tx_buf[tx_len++] = 0x00; tx_buf[tx_len++] = 0x04; // Data length 4
            tx_buf[tx_len++] = 192;  tx_buf[tx_len++] = 168;  tx_buf[tx_len++] = 4;    tx_buf[tx_len++] = 1;    // IP: 192.168.4.1

            sendto(sock, tx_buf, tx_len, 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

esp_err_t rock_onboarding_run(void)
{
    ESP_LOGI(TAG, "Entering Wi-Fi SoftAP onboarding mode");

    // Generate random 6-digit PIN for anti-hijack protection
    uint32_t rand_val = esp_random();
    snprintf(s_onboarding_pin, sizeof(s_onboarding_pin), "%06lu", (unsigned long)(rand_val % 1000000));
    ESP_LOGI(TAG, "Onboarding setup PIN generated: %s", s_onboarding_pin);

    // Configure and start SoftAP
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_config_t ap_config = {
        .ap = {
            .channel = CONFIG_ROCK_AP_CHANNEL,
            .max_connection = CONFIG_ROCK_AP_MAX_CONN,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, CONFIG_ROCK_AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(CONFIG_ROCK_AP_SSID);

    if (CONFIG_ROCK_AP_PASSWORD[0] != '\0') {
        strlcpy((char *)ap_config.ap.password, CONFIG_ROCK_AP_PASSWORD, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Try APSTA first (allows STA scanning if supported); fall back to AP mode if needed
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APSTA mode failed (%s); falling back to pure AP mode", esp_err_to_name(err));
        err = esp_wifi_set_mode(WIFI_MODE_AP);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi mode: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP active. SSID: '%s', IP: 192.168.4.1", CONFIG_ROCK_AP_SSID);

    // Start Captive DNS server
    xTaskCreate(dns_server_task, "rock_dns", 4096, NULL, 5, &s_dns_task_handle);

    // Start HTTP server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size = 8192;
    http_cfg.lru_purge_enable = true;

    if (httpd_start(&s_httpd, &http_cfg) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_get);

        httpd_uri_t uri_post = { .uri = "/connect", .method = HTTP_POST, .handler = connect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_post);

        // Captive portal detection redirects
        httpd_uri_t uri_c1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_c1);
        httpd_uri_t uri_c2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_c2);
        httpd_uri_t uri_c3 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_c3);
        httpd_uri_t uri_c4 = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_c4);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    // Generate QR code for http://192.168.4.1/?pin=XXXXXX
    char qr_url[64];
    snprintf(qr_url, sizeof(qr_url), "http://192.168.4.1/?pin=%s", s_onboarding_pin);
    static uint8_t qr_modules[1024];
    uint16_t qr_width = 0;
    if (rock_qr_encode(qr_url, qr_modules, sizeof(qr_modules), &qr_width) == 0) {
        rock_ui_onboarding_show(CONFIG_ROCK_AP_SSID, s_onboarding_pin, "http://192.168.4.1", qr_modules, qr_width);
    } else {
        ESP_LOGW(TAG, "QR code generation failed for onboarding URL");
        rock_ui_onboarding_show(CONFIG_ROCK_AP_SSID, s_onboarding_pin, "http://192.168.4.1", NULL, 0);
    }

    // Block here while user completes onboarding (reboot task handles exit)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    return ESP_OK;
}
