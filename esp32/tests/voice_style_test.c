// Host tests for voice_style.c: the telephone band and agreement with a
// double-precision copy of the Pi's chain.
#include "voice_style.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define N (RATE / 2) // Half a second per tone.

// The Pi's arithmetic, in double precision.
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } reference_t;
static void reference_section(reference_t *r, double f0, int high_pass)
{
    double w0 = 2 * 3.14159265358979323846 * f0 / RATE, cos_w = cos(w0), alpha = sin(w0) / (2 * (sqrt(2.0) / 2)), a0 = 1 + alpha;
    double b0 = high_pass ? (1 + cos_w) / 2 : (1 - cos_w) / 2;
    *r = (reference_t){.b0 = b0 / a0, .b1 = (high_pass ? -(1 + cos_w) : 1 - cos_w) / a0, .b2 = b0 / a0,
                       .a1 = -2 * cos_w / a0, .a2 = (1 - alpha) / a0};
}

static int16_t tone[N], filtered[N], expected[N];

static double gain_db(double frequency)
{
    for (int i = 0; i < N; ++i) tone[i] = (int16_t)(16000 * sin(2 * 3.14159265358979323846 * frequency * i / RATE));
    memcpy(filtered, tone, sizeof(tone));
    voice_style_copper_t filter;
    voice_style_copper_init(&filter, RATE);
    voice_style_copper_process(&filter, filtered, N);
    reference_t ref[4];
    reference_section(&ref[0], 300, 1);
    reference_section(&ref[1], 300, 1);
    reference_section(&ref[2], 3400, 0);
    reference_section(&ref[3], 3400, 0);
    for (int i = 0; i < N; ++i) {
        double v = tone[i];
        for (int s = 0; s < 4; ++s) {
            reference_t *r = &ref[s];
            double y = r->b0 * v + r->b1 * r->x1 + r->b2 * r->x2 - r->a1 * r->y1 - r->a2 * r->y2;
            r->x2 = r->x1; r->x1 = v; r->y2 = r->y1; r->y1 = y; v = y;
        }
        expected[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    double in = 0, out = 0;
    int worst = 0;
    for (int i = N / 4; i < N; ++i) { // After the filters settle.
        in += (double)tone[i] * tone[i];
        out += (double)filtered[i] * filtered[i];
        int difference = abs(filtered[i] - expected[i]);
        if (difference > worst) worst = difference;
    }
    assert(worst <= 2); // Single precision matches the Pi's double within rounding.
    return 10 * log10(out / in);
}

int main(void)
{
    double g100 = gain_db(100), g300 = gain_db(300), g1000 = gain_db(1000), g3400 = gain_db(3400),
           g6000 = gain_db(6000), g10000 = gain_db(10000);
    printf("Copper gain: 100 Hz %.1f, 300 Hz %.1f, 1 kHz %.1f, 3.4 kHz %.1f, 6 kHz %.1f, 10 kHz %.1f dB\n",
           g100, g300, g1000, g3400, g6000, g10000);
    assert(g1000 > -0.5 && g1000 < 0.5);   // Speech band untouched.
    assert(g300 > -6.6 && g300 < -5.4);    // Two -3 dB sections at the corner.
    assert(g3400 > -6.6 && g3400 < -5.4);
    assert(g100 < -35);                    // 4th order: about 38 dB down.
    assert(g6000 < -18 && g10000 < -40);

    voice_style_copper_t filter;
    voice_style_copper_init(&filter, RATE);
    int16_t loud[4] = {32767, -32768, 32767, -32768};
    voice_style_copper_process(&filter, loud, 4); // Clamped, never wrapped.
    voice_style_copper_reset(&filter);
    for (int i = 0; i < 4; ++i) assert(filter.section[i].x1 == 0 && filter.section[i].y1 == 0);
    puts("voice_style tests passed");
    return 0;
}
