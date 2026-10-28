#include "decimate.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define FRAMES 50 // One second.

// Level of a sine after decimation, relative to its input level, in dB.
static double gain_db(double hz)
{
    static decimate_t state;
    decimate_init(&state);
    int16_t in[DECIMATE_MAX_INPUT], out[DECIMATE_MAX_INPUT / 3];
    double sum_in = 0, sum_out = 0;
    for (int frame = 0; frame < FRAMES; ++frame) {
        for (int i = 0; i < DECIMATE_MAX_INPUT; ++i) {
            double t = (frame * DECIMATE_MAX_INPUT + i) / 48000.0;
            in[i] = (int16_t)lrint(16000 * sin(2 * 3.14159265358979323846 * hz * t));
            if (frame) sum_in += (double)in[i] * in[i] / 3;
        }
        decimate_process(&state, in, DECIMATE_MAX_INPUT, out);
        for (int i = 0; frame && i < DECIMATE_MAX_INPUT / 3; ++i) sum_out += (double)out[i] * out[i];
    }
    return 10 * log10((sum_out + 1e-9) / sum_in);
}

static void test_response(void)
{
    assert(fabs(gain_db(300)) < 0.1);
    assert(fabs(gain_db(1000)) < 0.1);
    assert(fabs(gain_db(3400)) < 0.2);
    assert(gain_db(5000) > -1.0);
    assert(gain_db(7200) < -4.0 && gain_db(7200) > -8.0);
    // Above 8 kHz would fold back into the voice band at 16 kHz.
    assert(gain_db(9500) < -60);
    assert(gain_db(12000) < -60);
    assert(gain_db(20000) < -60);
}

// Blocks of different sizes give the same output as one long block.
static void test_blocks(void)
{
    static decimate_t whole, parts;
    decimate_init(&whole);
    decimate_init(&parts);
    int16_t in[DECIMATE_MAX_INPUT], a[DECIMATE_MAX_INPUT / 3], b[DECIMATE_MAX_INPUT / 3];
    unsigned seed = 1;
    for (int i = 0; i < DECIMATE_MAX_INPUT; ++i) {
        seed = seed * 1103515245 + 12345;
        in[i] = (int16_t)(seed >> 16);
    }
    decimate_process(&whole, in, DECIMATE_MAX_INPUT, a);
    decimate_process(&parts, in, 3, b);
    decimate_process(&parts, in + 3, 477, b + 1);
    decimate_process(&parts, in + 480, 480, b + 160);
    assert(!memcmp(a, b, sizeof(a)));
}

// Full-scale input saturates instead of wrapping.
static void test_clamp(void)
{
    static decimate_t state;
    decimate_init(&state);
    int16_t in[DECIMATE_MAX_INPUT], out[DECIMATE_MAX_INPUT / 3];
    for (int i = 0; i < DECIMATE_MAX_INPUT; ++i) in[i] = i % 2 ? 32767 : -32768;
    decimate_process(&state, in, DECIMATE_MAX_INPUT, out);
    for (int i = 0; i < DECIMATE_MAX_INPUT; ++i) in[i] = 32767;
    decimate_process(&state, in, DECIMATE_MAX_INPUT, out);
    assert(out[DECIMATE_MAX_INPUT / 3 - 1] > 32000);
    decimate_reset(&state);
    memset(in, 0, sizeof(in));
    decimate_process(&state, in, DECIMATE_MAX_INPUT, out);
    for (int i = 0; i < DECIMATE_MAX_INPUT / 3; ++i) assert(out[i] == 0);
}

int main(void)
{
    test_response();
    test_blocks();
    test_clamp();
    printf("decimate_test: %.2f dB @1k, %.2f dB @7.2k, %.1f dB @9.5k\n", gain_db(1000), gain_db(7200), gain_db(9500));
    return 0;
}
