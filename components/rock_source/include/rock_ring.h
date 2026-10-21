#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Ring buffers, phase-4 split out of relay_bsp.c / player_bsp.c.
//
// Two deliberately SEPARATE types sharing a home: they implement different
// contracts and must not be unified.
//   - rock_ring_t:    broadcast byte ring (overwrite-oldest on overflow,
//                     monotonic uint64 positions, many readers at their own
//                     pace) — relay fanout;
//   - rock_pcm_ring_t: bounded FIFO queue (writers clamp to free space and
//                     back off; single consumer drains in order) — speaker.

// Byte ring shared by a single feeder and many readers.  Positions are
// monotonic uint64 offsets; the physical slot is pos & (cap - 1).
typedef struct {
    uint8_t *buf;
    size_t cap;
    uint64_t start;
    uint64_t end;
    SemaphoreHandle_t lock;
} rock_ring_t;

typedef enum {
    ROCK_RING_READ_OK = 0,
    ROCK_RING_READ_EMPTY,
    ROCK_RING_READ_OVERRUN,
} rock_ring_read_result_t;

// Allocate (or reuse across source restarts) the persistent 2 MiB SPIRAM
// ring and reset its contents. Returns false on allocation failure.
bool rock_ring_init(rock_ring_t *r);

// Reset ring contents under the lock; the buffers stay allocated.
void rock_ring_reset(rock_ring_t *r);

// Feeder push; drops the oldest whole 4-byte frames on overflow (rockcast
// fanout::push semantics).
void rock_ring_push(rock_ring_t *r, const uint8_t *data, size_t len);

size_t rock_ring_buffered(rock_ring_t *r);

// Copy up to cap bytes at pos (two-segment wrap copy) under the lock.
rock_ring_read_result_t rock_ring_read(rock_ring_t *r, uint64_t pos, uint8_t *out,
                                       size_t cap, size_t *out_len);

// Position a fresh reader joins at: behind the live edge by cushion_bytes.
uint64_t rock_ring_join_position(rock_ring_t *r, size_t cushion_bytes);

// Clamp an overrun reader back towards the live edge (cushion capped at
// len/2; rockcast read_at semantics).
uint64_t rock_ring_clamp_position(rock_ring_t *r, uint64_t pos, size_t cushion_bytes);

// Bounded FIFO of PCM bytes for the speaker path (former player_bsp pcm_ring).
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t fill;
    SemaphoreHandle_t lock;
} rock_pcm_ring_t;

// Allocate the ring in PSRAM. Returns 0 on success, -1 on allocation failure.
int rock_pcm_ring_init(rock_pcm_ring_t *r, size_t capacity);

// Write up to len bytes (clamped to free space); returns bytes written.
size_t rock_pcm_ring_write(rock_pcm_ring_t *r, const uint8_t *data, size_t len);

// Drain up to max_len bytes in FIFO order; returns bytes read.
size_t rock_pcm_ring_read(rock_pcm_ring_t *r, uint8_t *out, size_t max_len);

// Discard oldest bytes before a non-blocking spectrum write; caller keeps frame alignment.
void rock_pcm_ring_drop_oldest(rock_pcm_ring_t *r, size_t len);

size_t rock_pcm_ring_fill(rock_pcm_ring_t *r);

void rock_pcm_ring_clear(rock_pcm_ring_t *r);
