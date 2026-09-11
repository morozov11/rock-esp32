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
"* { box-sizing: border-box; margin: 0; padding: 0; }\n"
"body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #1A1410; color: #E8DCC8; min-height: 100vh; padding: 20px 16px; display: flex; justify-content: center; }\n"
".card { background: #241C16; border: 1px solid #3A2E24; border-radius: 14px; padding: 24px; max-width: 420px; width: 100%; box-shadow: 0 8px 32px rgba(0,0,0,0.6); margin-top: 8px; }\n"
".brand { display: flex; align-items: center; gap: 8px; margin-bottom: 14px; }\n"
".badge { background: rgba(196,92,38,0.18); color: #C45C26; border: 1px solid rgba(196,92,38,0.35); font-size: 11px; font-weight: 700; letter-spacing: 1px; padding: 4px 8px; border-radius: 6px; }\n"
"h1 { font-size: 22px; font-weight: 700; color: #E8DCC8; margin-bottom: 6px; }\n"
"p { font-size: 13px; color: #9A8B78; line-height: 1.45; margin-bottom: 18px; }\n"
"label { display: block; font-size: 13px; font-weight: 600; color: #CBD5E1; margin-bottom: 6px; margin-top: 14px; }\n"
".field-hdr { display: flex; justify-content: space-between; align-items: center; margin-top: 14px; margin-bottom: 6px; }\n"
".field-hdr label { margin: 0; }\n"
".scan-btn { background: none; border: none; color: #C45C26; font-size: 12px; font-weight: 600; cursor: pointer; padding: 2px 4px; }\n"
".scan-btn:hover { text-decoration: underline; }\n"
"select, input[type=\"text\"], input[type=\"password\"] { width: 100%; padding: 12px 14px; font-size: 15px; border-radius: 8px; border: 1px solid #3A2E24; background: #2E241C; color: #E8DCC8; outline: none; transition: border-color 0.2s; }\n"
"select:focus, input:focus { border-color: #C45C26; }\n"
"select { cursor: pointer; }\n"
".pwd-wrap { position: relative; }\n"
".pwd-wrap input { padding-right: 44px; }\n"
".eye-btn { position: absolute; right: 8px; top: 50%; transform: translateY(-50%); background: none; border: none; color: #9A8B78; font-size: 16px; cursor: pointer; padding: 6px; }\n"
".pin-card { background: #2E241C; border: 1px dashed #C45C26; border-radius: 8px; padding: 12px; margin-bottom: 14px; }\n"
".pin-card label { margin-top: 0; color: #C45C26; }\n"
".pin-input { font-size: 20px !important; letter-spacing: 4px; text-align: center; font-weight: 700; color: #E8DCC8 !important; }\n"
".btn-submit { width: 100%; margin-top: 22px; padding: 14px; font-size: 16px; font-weight: 700; color: #FFFFFF; background: #C45C26; border: none; border-radius: 8px; cursor: pointer; transition: background 0.2s; }\n"
".btn-submit:hover { background: #D96B30; }\n"
".hint { font-size: 11px; color: #9A8B78; margin-top: 4px; }\n"
"#custom-ssid-wrap { display: none; margin-top: 8px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"card\">\n"
"<div class=\"brand\"><span class=\"badge\">ROCKCAST RADIO</span></div>\n"
"<h1>Wi-Fi Setup</h1>\n"
"<p>Select your home Wi-Fi network and enter its password.</p>\n"
"<form method=\"POST\" action=\"/connect\" onsubmit=\"return validateForm()\">\n"
"<div class=\"pin-card\">\n"
"<label for=\"pin\">Setup PIN (from radio screen)</label>\n"
"<input type=\"text\" id=\"pin\" name=\"pin\" class=\"pin-input\" required placeholder=\"● ● ● ● ● ●\" maxlength=\"6\" pattern=\"[0-9]{6}\" inputmode=\"numeric\" autocomplete=\"off\">\n"
"<div class=\"hint\">Required for security. Enter the 6 digits from the display.</div>\n"
"</div>\n"
"<div class=\"field-hdr\">\n"
"<label for=\"net-select\">Wi-Fi Network</label>\n"
"<button type=\"button\" class=\"scan-btn\" onclick=\"loadNetworks()\">🔄 Refresh list</button>\n"
"</div>\n"
"<select id=\"net-select\" onchange=\"onNetworkChange(this.value)\">\n"
"<option value=\"\" disabled selected>🔄 Scanning available networks...</option>\n"
"</select>\n"
"<div id=\"custom-ssid-wrap\">\n"
"<input type=\"text\" id=\"custom-ssid\" placeholder=\"Enter network name (SSID)\" maxlength=\"32\" oninput=\"onCustomInput(this.value)\">\n"
"</div>\n"
"<input type=\"hidden\" id=\"ssid\" name=\"ssid\">\n"
"<label for=\"password\">Wi-Fi Password</label>\n"
"<div class=\"pwd-wrap\">\n"
"<input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Password (empty if open)\" maxlength=\"64\">\n"
"<button type=\"button\" class=\"eye-btn\" onclick=\"togglePwd()\">👁️</button>\n"
"</div>\n"
"<button type=\"submit\" id=\"sub-btn\" class=\"btn-submit\">Save and Connect</button>\n"
"</form>\n"
"</div>\n"
"<script>\n"
"function loadNetworks() {\n"
"  const sel = document.getElementById('net-select');\n"
"  sel.innerHTML = '<option value=\"\" disabled selected>🔄 Scanning available networks...</option>';\n"
"  fetch('/api/scan')\n"
"    .then(r => r.json())\n"
"    .then(nets => {\n"
"      sel.innerHTML = '<option value=\"\" disabled selected>— Select your Wi-Fi network —</option>';\n"
"      if (nets && nets.length > 0) {\n"
"        nets.forEach(n => {\n"
"          const opt = document.createElement('option');\n"
"          opt.value = n.ssid;\n"
"          const bars = n.rssi >= -60 ? '●●●●' : (n.rssi >= -75 ? '●●●○' : '●●○○');\n"
"          const lock = n.auth === 0 ? '🔓' : '🔒';\n"
"          opt.textContent = n.ssid + ' (' + bars + ' ' + lock + ')';\n"
"          sel.appendChild(opt);\n"
"        });\n"
"      }\n"
"      const optManual = document.createElement('option');\n"
"      optManual.value = '__custom__';\n"
"      optManual.textContent = '✏️ Enter other network manually...';\n"
"      sel.appendChild(optManual);\n"
"    })\n"
"    .catch(() => {\n"
"      sel.innerHTML = '<option value=\"__custom__\">Scan unavailable — enter manually</option>';\n"
"      onNetworkChange('__custom__');\n"
"    });\n"
"}\n"
"function onNetworkChange(val) {\n"
"  const wrap = document.getElementById('custom-ssid-wrap');\n"
"  const ssid = document.getElementById('ssid');\n"
"  const custom = document.getElementById('custom-ssid');\n"
"  if (val === '__custom__') {\n"
"    wrap.style.display = 'block';\n"
"    ssid.value = custom.value;\n"
"    custom.focus();\n"
"  } else {\n"
"    wrap.style.display = 'none';\n"
"    ssid.value = val;\n"
"  }\n"
"}\n"
"function onCustomInput(val) {\n"
"  document.getElementById('ssid').value = val;\n"
"}\n"
"function togglePwd() {\n"
"  const p = document.getElementById('password');\n"
"  p.type = p.type === 'password' ? 'text' : 'password';\n"
"}\n"
"function validateForm() {\n"
"  const pin = document.getElementById('pin').value;\n"
"  if (!pin || pin.length !== 6) {\n"
"    alert('Please enter the 6-digit Setup PIN from the radio screen.');\n"
"    return false;\n"
"  }\n"
"  const ssid = document.getElementById('ssid').value;\n"
"  if (!ssid || ssid.trim() === '') {\n"
"    alert('Please select a Wi-Fi network.');\n"
"    return false;\n"
"  }\n"
"  return true;\n"
"}\n"
"window.onload = function() {\n"
"  const p = new URLSearchParams(window.location.search).get('pin');\n"
"  if (p) document.getElementById('pin').value = p;\n"
"  loadNetworks();\n"
"};\n"
"</script>\n"
"</body>\n"
"</html>\n";

static const char CONNECTED_HTML[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>Saved</title>\n"
"<style>body{font-family:sans-serif;background:#1A1410;color:#E8DCC8;padding:32px;text-align:center;}\n"
".card{background:#241C16;border:1px solid #3A2E24;border-radius:12px;padding:24px;display:inline-block;max-width:360px;}\n"
"h2{color:#C45C26;margin-top:0;}</style></head>\n"
"<body><div class=\"card\"><h2>Credentials Saved</h2><p>RockCast is connecting to your Wi-Fi network. You can close this window now.</p></div></body></html>\n";

static const char FORBIDDEN_PIN_HTML[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>Invalid PIN</title>\n"
"<style>body{font-family:sans-serif;background:#1A1410;color:#E8DCC8;padding:32px;text-align:center;}\n"
".card{background:#241C16;border:1px solid #E05353;border-radius:12px;padding:24px;display:inline-block;max-width:360px;}\n"
"h2{color:#E05353;margin-top:0;}a{color:#C45C26;text-decoration:none;font-weight:600;}</style></head>\n"
"<body><div class=\"card\"><h2>Invalid Setup PIN</h2><p>The 6-digit PIN does not match the device screen.</p><p><a href=\"/\">Try again</a></p></div></body></html>\n";

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

static int compare_ap_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return (int)ap_b->rssi - (int)ap_a->rssi;
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 250,
            },
            .passive = 250,
        },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Wi-Fi scan complete, found %u APs", ap_count);

    uint16_t fetch_count = ap_count > 30 ? 30 : ap_count;
    wifi_ap_record_t *records = NULL;
    if (fetch_count > 0) {
        records = calloc(fetch_count, sizeof(wifi_ap_record_t));
    }

    size_t json_cap = 2048;
    char *json = malloc(json_cap);
    if (!json) {
        if (records) free(records);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t offset = 0;
    json[offset++] = '[';

    if (records && esp_wifi_scan_get_ap_records(&fetch_count, records) == ESP_OK) {
        if (fetch_count > 1) {
            qsort(records, fetch_count, sizeof(wifi_ap_record_t), compare_ap_rssi);
        }

        char seen_ssids[30][33];
        int seen_count = 0;

        for (uint16_t i = 0; i < fetch_count; i++) {
            if (records[i].ssid[0] == '\0') {
                continue;
            }

            const char *curr_ssid = (const char *)records[i].ssid;
            bool duplicate = false;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen_ssids[s], curr_ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (seen_count < 30) {
                strlcpy(seen_ssids[seen_count++], curr_ssid, sizeof(seen_ssids[0]));
            }

            char esc_ssid[65] = {0};
            size_t e = 0;
            for (size_t k = 0; curr_ssid[k] != '\0' && e + 2 < sizeof(esc_ssid); k++) {
                if (curr_ssid[k] == '"' || curr_ssid[k] == '\\') {
                    esc_ssid[e++] = '\\';
                }
                esc_ssid[e++] = curr_ssid[k];
            }
            esc_ssid[e] = '\0';

            char entry[128];
            int entry_len = snprintf(entry, sizeof(entry),
                                     "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                                     (offset > 1 ? "," : ""),
                                     esc_ssid,
                                     (int)records[i].rssi,
                                     (int)records[i].authmode);
            if (entry_len > 0 && offset + (size_t)entry_len < json_cap - 2) {
                memcpy(json + offset, entry, entry_len);
                offset += entry_len;
            }
        }
    }

    json[offset++] = ']';
    json[offset] = '\0';

    if (records) free(records);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, offset);
    free(json);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    rock_ui_onboarding_status("Phone connected! Complete setup in browser...");
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

        httpd_uri_t uri_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_httpd, &uri_scan);

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

    // Generate Wi-Fi Quick Connect QR code: WIFI:S:<SSID>;T:nopass;; (or T:WPA;P:<pwd>;;)
    char qr_payload[128];
    if (CONFIG_ROCK_AP_PASSWORD[0] != '\0') {
        snprintf(qr_payload, sizeof(qr_payload), "WIFI:S:%s;T:WPA;P:%s;;", CONFIG_ROCK_AP_SSID, CONFIG_ROCK_AP_PASSWORD);
    } else {
        snprintf(qr_payload, sizeof(qr_payload), "WIFI:S:%s;T:nopass;;", CONFIG_ROCK_AP_SSID);
    }
    ESP_LOGI(TAG, "Wi-Fi Quick Connect QR payload: %s", qr_payload);

    static uint8_t qr_modules[1024];
    uint16_t qr_width = 0;
    if (rock_qr_encode(qr_payload, qr_modules, sizeof(qr_modules), &qr_width) == 0) {
        rock_ui_onboarding_show(CONFIG_ROCK_AP_SSID, s_onboarding_pin, "http://192.168.4.1", qr_modules, qr_width);
    } else {
        ESP_LOGW(TAG, "QR code generation failed for Wi-Fi Quick Connect");
        rock_ui_onboarding_show(CONFIG_ROCK_AP_SSID, s_onboarding_pin, "http://192.168.4.1", NULL, 0);
    }

    // Block here while user completes onboarding (reboot task handles exit)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    return ESP_OK;
}
