// Screenshot transport: LVGL snapshot of the active screen, RLE-compressed,
// base64-encoded and streamed over stdout between ===SCREENSHOT_START=== /
// ===SCREENSHOT_END=== markers. Extracted from rock_gui.c (phase 3); the
// marker/line format is a contract of tools/capture_screen.py — keep verbatim.

#include "rock_screenshot.h"

#include "draw/snapshot/lv_snapshot.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>

static const char *TAG = "rock-screenshot";

typedef struct {
    uint8_t octets[3];
    size_t octet_count;
    char line_buf[128];
    size_t line_pos;
} b64_stream_t;

static void b64_emit_byte(b64_stream_t *s, uint8_t b)
{
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    s->octets[s->octet_count++] = b;
    if (s->octet_count == 3) {
        uint32_t triple = (s->octets[0] << 16) | (s->octets[1] << 8) | s->octets[2];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 18) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 12) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 6) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[triple & 0x3F];
        s->octet_count = 0;
        if (s->line_pos >= 120) {
            s->line_buf[s->line_pos++] = '\n';
            s->line_buf[s->line_pos] = '\0';
            fputs(s->line_buf, stdout);
            s->line_pos = 0;
            vTaskDelay(1);
        }
    }
}

static void b64_flush(b64_stream_t *s)
{
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (s->octet_count > 0) {
        uint32_t a = s->octets[0];
        uint32_t b = (s->octet_count >= 2) ? s->octets[1] : 0;
        uint32_t triple = (a << 16) | (b << 8);
        s->line_buf[s->line_pos++] = b64_table[(triple >> 18) & 0x3F];
        s->line_buf[s->line_pos++] = b64_table[(triple >> 12) & 0x3F];
        s->line_buf[s->line_pos++] = (s->octet_count >= 2) ? b64_table[(triple >> 6) & 0x3F] : '=';
        s->line_buf[s->line_pos++] = '=';
        s->octet_count = 0;
    }
    if (s->line_pos > 0) {
        s->line_buf[s->line_pos++] = '\n';
        s->line_buf[s->line_pos] = '\0';
        fputs(s->line_buf, stdout);
        s->line_pos = 0;
        vTaskDelay(1);
    }
    fflush(stdout);
}

int rock_gui_dump_screenshot(void)
{
    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL for screenshot");
        return -1;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "No active screen");
        return -1;
    }

    lv_obj_update_layout(screen);
    lv_draw_buf_t *draw_buf = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();

    if (!draw_buf) {
        ESP_LOGE(TAG, "lv_snapshot_take failed");
        return -1;
    }

    uint32_t w = draw_buf->header.w;
    uint32_t h = draw_buf->header.h;
    uint32_t stride = draw_buf->header.stride;
    uint32_t data_size = draw_buf->data_size;
    uint32_t total_pixels = w * h;

    esp_log_level_set("*", ESP_LOG_NONE);

    printf("\n===SCREENSHOT_START:w=%u,h=%u,stride=%u,cf=RGB565_RLE,len=%u===\n",
           (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)data_size);
    fflush(stdout);

    b64_stream_t stream = {0};
    uint32_t idx = 0;

    while (idx < total_pixels) {
        uint16_t cur = *(uint16_t *)(draw_buf->data + (idx / w) * stride + (idx % w) * 2);
        uint32_t run = 1;
        while (idx + run < total_pixels && *(uint16_t *)(draw_buf->data + ((idx + run) / w) * stride + ((idx + run) % w) * 2) == cur && run < 255) {
            run++;
        }
        b64_emit_byte(&stream, (uint8_t)run);
        b64_emit_byte(&stream, (uint8_t)(cur & 0xFF));
        b64_emit_byte(&stream, (uint8_t)((cur >> 8) & 0xFF));
        idx += run;
    }
    b64_flush(&stream);

    printf("===SCREENSHOT_END===\n");
    fflush(stdout);

    esp_log_level_set("*", ESP_LOG_INFO);

    lv_draw_buf_destroy(draw_buf);
    return 0;
}
