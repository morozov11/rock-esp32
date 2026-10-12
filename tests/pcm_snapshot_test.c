/* Run with a C11 compiler: cc -std=c11 tests/pcm_snapshot_test.c -o pcm_test */
#include "../components/rock_audio/include/pcm_snapshot.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    rock_pcm_snapshot_t tap = {0};
    int16_t in[512], out[512];
    uint32_t rate;
    uint8_t channels;
    assert(rock_pcm_copy(&tap, out, 512, &rate, &channels) == 0);
    for (int i = 0; i < 512; i++) in[i] = (int16_t)i;
    rock_pcm_publish(&tap, in, sizeof(in), 44100, 2);
    assert(rock_pcm_copy(&tap, out, 512, &rate, &channels) == 512);
    assert(rate == 44100 && channels == 2 && !memcmp(in, out, sizeof(in)));
    assert(rock_pcm_copy(&tap, out, 512, &rate, &channels) == 0);
    /* A reader-owned buffer cannot be overwritten, even across many publications. */
    atomic_store(&tap.slots[0].state, 3);
    tap.slots[0].samples[0] = 1234;
    for (int n = 0; n < 1000; n++) {
        in[0] = (int16_t)n;
        rock_pcm_publish(&tap, in, sizeof(in), 48000, 1);
    }
    assert(tap.slots[0].samples[0] == 1234);
    assert(rock_pcm_copy(&tap, out, 1, &rate, &channels) == 1);
    assert(out[0] == 999 && rate == 48000 && channels == 1);
    for (int i = 0; i < 3; i++) atomic_store(&tap.slots[i].state, 3);
    rock_pcm_publish(&tap, in, sizeof(in), 16000, 1); /* drop, never wait */
    assert(rock_pcm_copy(&tap, out, 512, &rate, &channels) == 0);
    puts("PCM snapshot ownership/bounds: PASS");
}
