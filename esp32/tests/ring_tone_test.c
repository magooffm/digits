#include "ring_tone.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int16_t pcm[(RING_TONE_FRAMES + 160) * 2];
    int16_t chunk[320];
    ring_tone_render(pcm, 0, RING_TONE_FRAMES + 160);
    int peak = 0;
    double energy = 0;
    for (size_t frame = 0; frame < RING_TONE_FRAMES + 160; ++frame) {
        assert(pcm[2 * frame] == pcm[2 * frame + 1]);
        int level = abs(pcm[2 * frame]);
        assert(level <= 8192);
        if (level > peak) peak = level;
        energy += (double)level * level;
        if (frame >= RING_TONE_FRAMES) assert(level == 0);
    }
    assert(pcm[0] == 0 && pcm[2 * (RING_TONE_FRAMES - 1)] == 0);
    assert(peak > 7000 && energy > 1e10);
    // Chunked streaming must preserve phase and fades across buffer boundaries.
    for (size_t first = 0; first < RING_TONE_FRAMES; first += 160) {
        ring_tone_render(chunk, first, 160);
        for (size_t i = 0; i < 320; ++i) assert(chunk[i] == pcm[2 * first + i]);
    }
    puts("PASS: one-second bounded stereo tone, silence, fades and streaming continuity");
    return 0;
}
