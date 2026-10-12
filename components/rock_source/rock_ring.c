// Ring buffer implementations, phase-4 split out of relay_bsp.c (byte ring)
// and player_bsp.c (PCM bounded queue). See rock_ring.h for the two
// deliberately separate contracts.

#include "rock_ring.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "rock_ring";

// Ring sized like the desktop relay fanout (2 MiB ~= 10 s of PCM).
#define ROCK_RING_BYTES (2 * 1024 * 1024)

// ---------------------------------------------------------------- ring ---

bool rock_ring_init(rock_ring_t *r)
{
    if (r->buf && r->lock) {
        // Persistent ring: reuse the allocation across source restarts.
        xSemaphoreTake(r->lock, portMAX_DELAY);
        r->start = 0;
        r->end = 0;
        xSemaphoreGive(r->lock);
        return true;
    }
    memset(r, 0, sizeof(*r));
    r->buf = heap_caps_malloc(ROCK_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->lock = xSemaphoreCreateMutex();
    if (!r->buf || !r->lock) {
        if (r->lock) {
            vSemaphoreDelete(r->lock);
        }
        free(r->buf);
        r->buf = NULL;
        return false;
    }
    r->cap = ROCK_RING_BYTES;
    return true;
}

void rock_ring_reset(rock_ring_t *r)
{
    if (!r->lock) {
        return;
    }
    xSemaphoreTake(r->lock, portMAX_DELAY);
    r->start = 0;
    r->end = 0;
    xSemaphoreGive(r->lock);
}

void rock_ring_push(rock_ring_t *r, const uint8_t *data, size_t len)
{
    if (len == 0 || !r->buf) {
        return;
    }
    xSemaphoreTake(r->lock, portMAX_DELAY);
    if (r->end - r->start + len > r->cap) {
        uint64_t drop = r->end - r->start + len - r->cap;
        drop = ((drop + 3) / 4) * 4;
        if (drop > r->end - r->start) {
            drop = r->end - r->start;
        }
        r->start += drop;
    }
    uint64_t pos = r->end;
    size_t first = (size_t)(r->cap - (pos & (r->cap - 1)));
    if (first > len) {
        first = len;
    }
    memcpy(r->buf + (size_t)(pos & (r->cap - 1)), data, first);
    if (len > first) {
        memcpy(r->buf, data + first, len - first);
    }
    r->end = pos + len;
    xSemaphoreGive(r->lock);
}

size_t rock_ring_buffered(rock_ring_t *r)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t len = (size_t)(r->end - r->start);
    xSemaphoreGive(r->lock);
    return len;
}

rock_ring_read_result_t rock_ring_read(rock_ring_t *r, uint64_t pos, uint8_t *out,
                                       size_t cap, size_t *out_len)
{
    rock_ring_read_result_t result = ROCK_RING_READ_OK;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    if (pos < r->start) {
        result = ROCK_RING_READ_OVERRUN;
        *out_len = 0;
    } else if (pos >= r->end) {
        result = ROCK_RING_READ_EMPTY;
        *out_len = 0;
    } else {
        size_t available = (size_t)(r->end - pos);
        size_t n = cap < available ? cap : available;
        size_t off = (size_t)(pos & (r->cap - 1));
        size_t first = r->cap - off;
        if (first > n) {
            first = n;
        }
        memcpy(out, r->buf + off, first);
        if (n > first) {
            memcpy(out + first, r->buf, n - first);
        }
        *out_len = n;
    }
    xSemaphoreGive(r->lock);
    return result;
}

uint64_t rock_ring_join_position(rock_ring_t *r, size_t cushion_bytes)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t len = (size_t)(r->end - r->start);
    size_t cushion = cushion_bytes < len ? cushion_bytes : len;
    uint64_t pos = r->end - cushion;
    xSemaphoreGive(r->lock);
    return pos & ~3ULL;
}

uint64_t rock_ring_clamp_position(rock_ring_t *r, uint64_t pos, size_t cushion_bytes)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t len = (size_t)(r->end - r->start);
    size_t cushion = cushion_bytes < (len / 2) ? cushion_bytes : (len / 2);
    uint64_t clamped = r->end - cushion;
    if (pos < r->start) {
        pos = clamped;
    }
    if (pos > r->end) {
        pos = r->end;
    }
    xSemaphoreGive(r->lock);
    return pos & ~3ULL;
}

// ----------------------------------------------------------- pcm queue ---

int rock_pcm_ring_init(rock_pcm_ring_t *r, size_t capacity)
{
    r->buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->buffer) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for PCM ring buffer", (unsigned)capacity);
        return -1;
    }
    r->capacity = capacity;
    r->head = 0;
    r->tail = 0;
    r->fill = 0;
    r->lock = xSemaphoreCreateMutex();
    return r->lock ? 0 : -1;
}

size_t rock_pcm_ring_write(rock_pcm_ring_t *r, const uint8_t *data, size_t len)
{
    if (!r || !r->buffer || !data || len == 0) return 0;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t free_space = r->capacity - r->fill;
    size_t to_write = len < free_space ? len : free_space;
    if (to_write > 0) {
        size_t part1 = r->capacity - r->head;
        if (to_write <= part1) {
            memcpy(r->buffer + r->head, data, to_write);
            r->head = (r->head + to_write) % r->capacity;
        } else {
            memcpy(r->buffer + r->head, data, part1);
            memcpy(r->buffer, data + part1, to_write - part1);
            r->head = to_write - part1;
        }
        r->fill += to_write;
    }
    xSemaphoreGive(r->lock);
    return to_write;
}

size_t rock_pcm_ring_read(rock_pcm_ring_t *r, uint8_t *out, size_t max_len)
{
    if (!r || !r->buffer || !out || max_len == 0) return 0;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t to_read = max_len < r->fill ? max_len : r->fill;
    if (to_read > 0) {
        size_t part1 = r->capacity - r->tail;
        if (to_read <= part1) {
            memcpy(out, r->buffer + r->tail, to_read);
            r->tail = (r->tail + to_read) % r->capacity;
        } else {
            memcpy(out, r->buffer + r->tail, part1);
            memcpy(out + part1, r->buffer, to_read - part1);
            r->tail = to_read - part1;
        }
        r->fill -= to_read;
    }
    xSemaphoreGive(r->lock);
    return to_read;
}

size_t rock_pcm_ring_fill(rock_pcm_ring_t *r)
{
    if (!r || !r->lock) return 0;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t fill = r->fill;
    xSemaphoreGive(r->lock);
    return fill;
}

void rock_pcm_ring_clear(rock_pcm_ring_t *r)
{
    if (!r || !r->lock) return;
    xSemaphoreTake(r->lock, portMAX_DELAY);
    r->head = 0;
    r->tail = 0;
    r->fill = 0;
    xSemaphoreGive(r->lock);
}
