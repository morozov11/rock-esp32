#include "rock_wifi_storage.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "rock-wifi-store";
static const char *NVS_NAMESPACE = "rock_wifi";

bool rock_wifi_storage_load(char *ssid, size_t ssid_cap, char *password, size_t password_cap)
{
    if (!ssid || ssid_cap == 0) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_len = ssid_cap;
    err = nvs_get_str(handle, "ssid", ssid, &required_len);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(handle);
        return false;
    }

    if (password && password_cap > 0) {
        required_len = password_cap;
        err = nvs_get_str(handle, "password", password, &required_len);
        if (err != ESP_OK) {
            password[0] = '\0';
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Wi-Fi credentials loaded from NVS (SSID length: %u)", (unsigned)strlen(ssid));
    return true;
}

bool rock_wifi_storage_save(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32) {
        ESP_LOGE(TAG, "Invalid SSID supplied");
        return false;
    }
    if (password && strlen(password) > 64) {
        ESP_LOGE(TAG, "Password exceeds maximum length");
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace %s: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit Wi-Fi credentials to NVS: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Wi-Fi credentials safely committed to NVS");
    return true;
}

bool rock_wifi_storage_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Wi-Fi credentials cleared from NVS (namespace: %s)", NVS_NAMESPACE);
    return err == ESP_OK;
}
