#include "rock_ota.h"
#include "display_bsp.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "rock-ota";
static bool s_ota_in_progress = false;
static TaskHandle_t s_ota_task_handle = NULL;

typedef struct {
    char url[512];
    bool force;
} ota_task_param_t;

esp_err_t rock_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Cannot determine running partition");
        return ESP_FAIL;
    }

    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Running partition '%s' (0x%lx) is PENDING_VERIFY! Validation required",
                     running->label, (unsigned long)running->address);
        } else {
            ESP_LOGI(TAG, "Running partition '%s' (0x%lx), state: %d",
                     running->label, (unsigned long)running->address, (int)ota_state);
        }
    } else {
        ESP_LOGI(TAG, "Running partition '%s' (0x%lx), no OTA state (factory or initial)",
                 running->label, (unsigned long)running->address);
    }
    return ESP_OK;
}

esp_err_t rock_ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_FAIL;
    }
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Cancelling rollback; marking running app as VALID");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}

esp_err_t rock_ota_mark_invalid_and_rollback(void)
{
    ESP_LOGE(TAG, "Marking running app as INVALID and initiating rollback reboot...");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool rock_ota_is_in_progress(void)
{
    return s_ota_in_progress;
}

#ifdef CONFIG_ROCK_OTA_TEST_CERT_PEM
static char s_unescaped_cert[4096];
static const char *get_test_ca_pem(void)
{
    const char *raw = CONFIG_ROCK_OTA_TEST_CERT_PEM;
    if (!raw || raw[0] == '\0') return NULL;

    // If it contains BEGIN CERTIFICATE, handle escaped \n if present
    if (strstr(raw, "-----BEGIN CERTIFICATE-----") != NULL) {
        if (strstr(raw, "\\n") != NULL) {
            size_t j = 0;
            for (size_t i = 0; raw[i] && j + 1 < sizeof(s_unescaped_cert); i++) {
                if (raw[i] == '\\' && raw[i+1] == 'n') {
                    s_unescaped_cert[j++] = '\n';
                    i++;
                } else {
                    s_unescaped_cert[j++] = raw[i];
                }
            }
            s_unescaped_cert[j] = '\0';
            return s_unescaped_cert;
        }
        return raw;
    }

    // Otherwise treat as raw base64 payload and construct valid RFC 7468 PEM
    size_t j = 0;
    const char header[] = "-----BEGIN CERTIFICATE-----\n";
    memcpy(s_unescaped_cert + j, header, sizeof(header) - 1);
    j += sizeof(header) - 1;

    size_t raw_len = strlen(raw);
    for (size_t i = 0; i < raw_len && j + 2 < sizeof(s_unescaped_cert); i++) {
        s_unescaped_cert[j++] = raw[i];
        if ((i + 1) % 64 == 0) {
            s_unescaped_cert[j++] = '\n';
        }
    }
    if (s_unescaped_cert[j - 1] != '\n') {
        s_unescaped_cert[j++] = '\n';
    }

    const char footer[] = "-----END CERTIFICATE-----\n";
    if (j + sizeof(footer) < sizeof(s_unescaped_cert)) {
        memcpy(s_unescaped_cert + j, footer, sizeof(footer) - 1);
        j += sizeof(footer) - 1;
    }
    s_unescaped_cert[j] = '\0';
    return s_unescaped_cert;
}
#endif

static void ota_worker_task(void *pvParameter)
{
    ota_task_param_t *params = (ota_task_param_t *)pvParameter;
    s_ota_in_progress = true;

    ESP_LOGI(TAG, "Starting OTA update from URL: %s", params->url);

    esp_http_client_config_t http_cfg = {
        .url = params->url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

#ifdef CONFIG_ROCK_OTA_TEST_CERT_PEM
    const char *test_ca = get_test_ca_pem();
    if (test_ca && test_ca[0] != '\0') {
        ESP_LOGI(TAG, "Using explicit test CA certificate from CONFIG_ROCK_OTA_TEST_CERT_PEM (length: %u)", (unsigned)strlen(test_ca));
        http_cfg.cert_pem = test_ca;
        http_cfg.cert_len = strlen(test_ca) + 1;
        http_cfg.crt_bundle_attach = NULL;
    } else {
        ESP_LOGI(TAG, "CONFIG_ROCK_OTA_TEST_CERT_PEM empty; using system certificate bundle");
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
#else
    ESP_LOGI(TAG, "Using default system certificate bundle");
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif



    esp_https_ota_config_t ota_config = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK || !ota_handle) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        free(params);
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(ota_handle, &app_desc);
    if (err == ESP_OK) {
        const esp_app_desc_t *running_desc = esp_app_get_description();
        ESP_LOGI(TAG, "New firmware version: '%s', current version: '%s'", app_desc.version, running_desc->version);
        if (!params->force && strncmp(app_desc.version, running_desc->version, sizeof(app_desc.version)) == 0) {
            ESP_LOGI(TAG, "Firmware version matches running image; aborting update");
            esp_https_ota_abort(ota_handle);
            s_ota_in_progress = false;
            free(params);
            s_ota_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
    } else {
        ESP_LOGW(TAG, "Could not read image descriptor: %s", esp_err_to_name(err));
    }

    int last_reported_percent = -1;
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int len_read = esp_https_ota_get_image_len_read(ota_handle);
        int total_size = esp_https_ota_get_image_size(ota_handle);
        if (total_size > 0) {
            int percent = (len_read * 100) / total_size;
            if (percent != last_reported_percent && (percent % 5 == 0 || percent == 100)) {
                last_reported_percent = percent;
                ESP_LOGI(TAG, "OTA progress: %d%% (%d / %d bytes)", percent, len_read, total_size);
                rock_ui_ota_progress_show(app_desc.version, percent);
            }
        }

        // Yield to let Core 1 audio streaming and other tasks breathe without starvation
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota_handle)) {
        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA update successfully written and verified! Rebooting in 2 seconds...");
            rock_ui_ota_progress_show(app_desc.version, 100);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "esp_https_ota_perform failed or incomplete: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
    }

    s_ota_in_progress = false;
    free(params);
    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t rock_ota_check_start(const char *url, bool force)
{
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA check already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    const char *target_url = url;
    if (!target_url || target_url[0] == '\0') {
#ifdef CONFIG_ROCK_OTA_URL
        target_url = CONFIG_ROCK_OTA_URL;
#else
        target_url = "";
#endif
    }

    if (target_url[0] == '\0') {
        ESP_LOGI(TAG, "No OTA URL configured; skipping update check");
        return ESP_OK;
    }

    ota_task_param_t *params = malloc(sizeof(ota_task_param_t));
    if (!params) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(params->url, target_url, sizeof(params->url));
    params->force = force;

    // Pin OTA task to Core 0 with priority 3 (Audio decoder/playback is on Core 1 at priorities 5 & 6)
    BaseType_t res = xTaskCreatePinnedToCore(ota_worker_task, "rock_ota", 8192, params, 3, &s_ota_task_handle, 0);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(params);
        return ESP_FAIL;
    }

    return ESP_OK;
}
