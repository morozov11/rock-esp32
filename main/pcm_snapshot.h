#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Single ring consumer publishes; analysis owns a slot while copying it.
 * No ring lock, allocation, retry loop, or wait on the audio path. */
#define ROCK_PCM_SNAPSHOT_SAMPLES 512
typedef struct {
    atomic_int state; /* 0 available, 1 writer, 2 ready, 3 reader */
    uint32_t sample_rate;
    uint8_t channels;
    size_t count;
    int16_t samples[ROCK_PCM_SNAPSHOT_SAMPLES];
} rock_pcm_slot_t;
typedef struct {
    rock_pcm_slot_t slots[3];
    atomic_int latest;
} rock_pcm_snapshot_t;

static inline void rock_pcm_publish(rock_pcm_snapshot_t *tap, const void *pcm,
                                   size_t bytes, uint32_t rate, uint8_t channels)
{
    int start = atomic_load_explicit(&tap->latest, memory_order_relaxed);
    for (int n = 1; n <= 3; n++) {
        int i = (start + n) % 3;
        rock_pcm_slot_t *s = &tap->slots[i];
        int expected = atomic_load_explicit(&s->state, memory_order_relaxed);
        if ((expected != 0 && expected != 2) ||
            !atomic_compare_exchange_strong_explicit(&s->state, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) continue;
        s->count = bytes / sizeof(int16_t);
        if (s->count > ROCK_PCM_SNAPSHOT_SAMPLES) s->count = ROCK_PCM_SNAPSHOT_SAMPLES;
        memcpy(s->samples, pcm, s->count * sizeof(int16_t));
        s->sample_rate = rate;
        s->channels = channels;
        atomic_store_explicit(&s->state, 2, memory_order_release);
        atomic_store_explicit(&tap->latest, i, memory_order_release);
        return;
    }
}

static inline size_t rock_pcm_copy(rock_pcm_snapshot_t *tap, int16_t *out,
                                  size_t cap, uint32_t *rate, uint8_t *channels)
{
    int i = atomic_load_explicit(&tap->latest, memory_order_acquire);
    rock_pcm_slot_t *s = &tap->slots[i];
    int expected = 2;
    if (!atomic_compare_exchange_strong_explicit(&s->state, &expected, 3,
            memory_order_acquire, memory_order_relaxed)) return 0;
    size_t count = s->count < cap ? s->count : cap;
    memcpy(out, s->samples, count * sizeof(int16_t));
    *rate = s->sample_rate;
    *channels = s->channels;
    atomic_store_explicit(&s->state, 0, memory_order_release);
    return count;
}
