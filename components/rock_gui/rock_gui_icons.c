// Fetch one player icon before starting its station; keep only the decoded image in PSRAM.
#include "rock_gui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "rock_player.h"
#include "webp/decode.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define ICON_SIDE 88
#define ICON_BYTES (ICON_SIDE * ICON_SIDE * 2)
#define ICON_SOURCE_MAX_BYTES (512 * 1024)

static const char *TAG = "rock-icons";
static TaskHandle_t s_task;
static char s_pending_play_id[129]; // Accessed under the LVGL port lock.
static atomic_uint s_selection;

static struct {
    char id[129];
    uint8_t *pixels;
    lv_image_dsc_t image;
    bool ready;
} s_icon;

typedef struct {
    uint8_t bytes[ICON_SOURCE_MAX_BYTES];
    uint8_t decoded[ICON_BYTES];
    size_t len;
    bool overflow;
    int64_t started_us;
    unsigned selection;
} icon_response_t;

static bool audio_active(void)
{
    int status = rock_player_status_enum();
    return status == ROCK_PLAYER_STATUS_BUFFERING || status == ROCK_PLAYER_STATUS_PLAYING ||
           status == ROCK_PLAYER_STATUS_PAUSED;
}

void gui_icons_apply(lv_obj_t *image, lv_obj_t *monogram, const char *station_id)
{
    if (station_id && s_icon.ready && strcmp(s_icon.id, station_id) == 0) {
        lv_image_set_src(image, &s_icon.image);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(monogram, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (lv_image_get_src(image)) lv_image_set_src(image, NULL);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(monogram, LV_OBJ_FLAG_HIDDEN);
    }
}

void gui_icons_refresh(void)
{
    gui_now_playing_icon_refresh();
}

static esp_err_t on_http(esp_http_client_event_t *event)
{
    icon_response_t *response = event->user_data;
    if (!response || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if (audio_active() || response->selection != atomic_load(&s_selection) ||
        esp_timer_get_time() - response->started_us > 4000000) return ESP_FAIL;
    size_t n = (size_t)event->data_len;
    if (n > ICON_SOURCE_MAX_BYTES - response->len) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->bytes + response->len, event->data, n);
    response->len += n;
    return ESP_OK;
}

static bool icon_path(char *out, size_t cap, const char *id)
{
    int n = snprintf(out, cap, "%s/api/v1/stations/", CONFIG_ROCKSERVER_BASE_URL);
    if (n < 0 || (size_t)n >= cap) return false;
    size_t used = (size_t)n;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '~') {
            if (used + 1 >= cap) return false;
            out[used++] = (char)*p;
        } else {
            if (used + 3 >= cap) return false;
            n = snprintf(out + used, cap - used, "%%%02X", *p);
            used += 3;
        }
    }
    if (used + sizeof("/icon") > cap) return false;
    memcpy(out + used, "/icon", sizeof("/icon"));
    return true;
}

static bool decode_icon(icon_response_t *response)
{
    WebPDecoderConfig decoder;
    if (!WebPInitDecoderConfig(&decoder) ||
        WebPGetFeatures(response->bytes, response->len, &decoder.input) != VP8_STATUS_OK ||
        decoder.input.width != 256 || decoder.input.height != 256 || decoder.input.has_animation) {
        return false;
    }
    decoder.options.use_scaling = 1;
    decoder.options.scaled_width = ICON_SIDE;
    decoder.options.scaled_height = ICON_SIDE;
    decoder.output.colorspace = MODE_RGB_565;
    decoder.output.is_external_memory = 1;
    decoder.output.u.RGBA.rgba = response->decoded;
    decoder.output.u.RGBA.stride = ICON_SIDE * 2;
    decoder.output.u.RGBA.size = ICON_BYTES;
    return WebPDecode(response->bytes, response->len, &decoder) == VP8_STATUS_OK;
}

static int fetch_icon(const char *id, icon_response_t *response)
{
    char url[768];
    if (!icon_path(url, sizeof(url), id) || audio_active()) return -1;
    response->started_us = esp_timer_get_time();
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = on_http, .user_data = response,
        .timeout_ms = 3500, .is_async = true, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return -1;
    if (esp_http_client_set_header(http, "Accept", "image/webp") != ESP_OK) {
        esp_http_client_cleanup(http);
        return -1;
    }
    esp_err_t err;
    do {
        err = esp_http_client_perform(http);
        if (err == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (err == ESP_ERR_HTTP_EAGAIN && !audio_active() &&
             response->selection == atomic_load(&s_selection) &&
             esp_timer_get_time() - response->started_us < 4000000);
    int status = err == ESP_OK ? esp_http_client_get_status_code(http) : -1;
    esp_http_client_cleanup(http);
    if (audio_active() || response->selection != atomic_load(&s_selection)) return -1;
    if (status == 200 && response->len && !response->overflow && decode_icon(response)) return 200;
    return status == 404 ? 404 : -1;
}

static void icon_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char id[129] = {0};
        while (!lvgl_port_lock(100)) vTaskDelay(pdMS_TO_TICKS(10));
        strlcpy(id, s_pending_play_id, sizeof(id));
        unsigned selection = atomic_load(&s_selection);
        lvgl_port_unlock();
        if (!id[0]) continue;

        int64_t stop_deadline = esp_timer_get_time() + 3000000;
        while (selection == atomic_load(&s_selection) && audio_active() &&
               esp_timer_get_time() < stop_deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        int64_t started = esp_timer_get_time();
        int status = -1;
        icon_response_t *response = NULL;
        if (selection == atomic_load(&s_selection) && !audio_active()) {
            response = heap_caps_calloc(1, sizeof(*response), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (response) {
                response->selection = selection;
                status = fetch_icon(id, response);
            }
        }

        while (!lvgl_port_lock(1000)) vTaskDelay(pdMS_TO_TICKS(10));
        if (selection == atomic_load(&s_selection) && strcmp(s_pending_play_id, id) == 0) {
            s_pending_play_id[0] = '\0';
            if (status == 200 && !s_icon.pixels) {
                s_icon.pixels = heap_caps_malloc(ICON_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!s_icon.pixels) status = -1;
            }
            bool had_image = s_icon.ready;
            s_icon.ready = false;
            gui_icons_refresh(); // detach the old LVGL image before reusing its buffer
            if (had_image) lv_image_cache_drop(&s_icon.image);
            s_icon.id[0] = '\0';
            if (status == 200) {
                for (size_t i = 0; i < ICON_BYTES; i += 2) {
                    s_icon.pixels[i] = response->decoded[i + 1];
                    s_icon.pixels[i + 1] = response->decoded[i];
                }
                s_icon.image = (lv_image_dsc_t){
                    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                               .w = ICON_SIDE, .h = ICON_SIDE, .stride = ICON_SIDE * 2},
                    .data_size = ICON_BYTES, .data = s_icon.pixels,
                };
                s_icon.ready = true;
            }
            if (status == 200 || status == 404) strlcpy(s_icon.id, id, sizeof(s_icon.id));
            gui_icons_refresh();
            gui_enqueue_event(ROCK_UI_EVT_PLAY_STATION, id, 0);
        }
        lvgl_port_unlock();
        free(response);
        ESP_LOGI(TAG, "player icon id=%s status=%d ms=%lld", id, status,
                 (long long)((esp_timer_get_time() - started) / 1000));
    }
}

void gui_icons_prepare_play(const char *station_id)
{
    if (!station_id || !station_id[0]) return;
    atomic_fetch_add(&s_selection, 1);
    s_pending_play_id[0] = '\0';
    if (strcmp(s_icon.id, station_id) == 0 || !s_task) {
        gui_enqueue_event(ROCK_UI_EVT_PLAY_STATION, station_id, 0);
        return;
    }
    strlcpy(s_pending_play_id, station_id, sizeof(s_pending_play_id));
    if (audio_active()) gui_enqueue_event(ROCK_UI_EVT_PLAYBACK_STOP, NULL, 0);
    xTaskNotifyGive(s_task);
}

void gui_icons_start(void)
{
    if (s_task) return;
    if (xTaskCreatePinnedToCoreWithCaps(icon_task, "rock-icons", 8192, NULL, 1, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start icon task in PSRAM");
        s_task = NULL;
    }
}
