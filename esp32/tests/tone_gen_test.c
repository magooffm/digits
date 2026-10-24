// Host test for call-progress tone rendering: frequencies (Goertzel), cadence,
// level, block-size independence. Run by esp32/tests/run_host_tests.sh.
#include "tone_gen.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE TONE_GEN_SAMPLE_RATE
#define PI 3.14159265358979323846

static int16_t *render(tone_gen_tone_t tone, uint64_t start, size_t frames, size_t block)
{
    int16_t *pcm = malloc(frames * 2 * sizeof(*pcm));
    assert(pcm);
    for (size_t done = 0; done < frames; done += block) {
        size_t n = frames - done < block ? frames - done : block;
        tone_gen_render(tone, start + done, pcm + 2 * done, n);
    }
    return pcm;
}

static double goertzel(const int16_t *stereo, size_t frames, double hz)
{
    double k = 2 * cos(2 * PI * hz / RATE), s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; ++i) {
        double s0 = stereo[2 * i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / frames;
}

static void check_frequencies(tone_gen_tone_t tone, double low, double high)
{
    size_t frames = RATE / 10; // 100 ms, inside every tone's first on-period.
    int16_t *pcm = render(tone, 0, frames, 480);
    double a = goertzel(pcm, frames, low), b = goertzel(pcm, frames, high);
    // Each component has peak 2048, so its Goertzel magnitude is about 1024.
    assert(fabs(a - 1024) < 30 && fabs(b - 1024) < 30);
    const double others[] = {350, 440, 480, 620, 1000};
    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); ++i)
        if (others[i] != low && others[i] != high) assert(goertzel(pcm, frames, others[i]) < 20);
    int peak = 0;
    for (size_t i = 0; i < frames; ++i) {
        assert(pcm[2 * i] == pcm[2 * i + 1]);
        if (abs(pcm[2 * i]) > peak) peak = abs(pcm[2 * i]);
    }
    assert(peak <= 2 * TONE_GEN_COMPONENT_PEAK && peak > 3900);
    free(pcm);
}

static bool audible(tone_gen_tone_t tone, uint64_t at_ms)
{
    // A 5 ms window starting at at_ms must contain signal.
    int16_t *pcm = render(tone, at_ms * 48, 240, 240);
    bool any = false;
    for (int i = 0; i < 240; ++i) any |= pcm[2 * i] != 0;
    free(pcm);
    return any;
}

static void check_cadence(tone_gen_tone_t tone, unsigned on_ms, unsigned off_ms)
{
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        uint64_t base = (uint64_t)cycle * (on_ms + off_ms);
        assert(audible(tone, base + 1));
        assert(audible(tone, base + on_ms - 6));
        if (off_ms) {
            assert(!audible(tone, base + on_ms + 1));
            assert(!audible(tone, base + on_ms + off_ms - 6));
        }
    }
}

int main(void)
{
    tone_gen_init();
    assert(!tone_gen_spec(TONE_GEN_NONE) && tone_gen_spec(TONE_GEN_REORDER));
    check_frequencies(TONE_GEN_DIAL, 350, 440);
    check_frequencies(TONE_GEN_RINGBACK, 440, 480);
    check_frequencies(TONE_GEN_BUSY, 480, 620);
    check_frequencies(TONE_GEN_REORDER, 480, 620);
    check_cadence(TONE_GEN_DIAL, 1000, 0);
    check_cadence(TONE_GEN_RINGBACK, 2000, 4000);
    check_cadence(TONE_GEN_BUSY, 500, 500);
    check_cadence(TONE_GEN_REORDER, 250, 250);
    // Long-running position (one hour in) still renders the exact waveform.
    int16_t *early = render(TONE_GEN_DIAL, 0, 960, 960);
    int16_t *late = render(TONE_GEN_DIAL, 3600ULL * RATE, 960, 960);
    assert(!memcmp(early, late, 960 * 2 * sizeof(int16_t)));
    // Block size never changes the output.
    int16_t *whole = render(TONE_GEN_BUSY, 12345, 9600, 9600);
    int16_t *pieces = render(TONE_GEN_BUSY, 12345, 9600, 7);
    assert(!memcmp(whole, pieces, 9600 * 2 * sizeof(int16_t)));
    int16_t *none = render(TONE_GEN_NONE, 0, 480, 480);
    for (int i = 0; i < 960; ++i) assert(none[i] == 0);
    free(early); free(late); free(whole); free(pieces); free(none);
    puts("tone_gen_test: frequencies, cadence, level and block independence passed");
    return 0;
}
