#include "decimate.h"

#include <math.h>
#include <string.h>

#define FACTOR 3
#define CUTOFF_HZ 7200.0
#define RATE_HZ 48000.0

void decimate_init(decimate_t *state)
{
    const double pi = 3.14159265358979323846, center = (DECIMATE_TAPS - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < DECIMATE_TAPS; ++i) {
        double x = i - center, phase = 2 * pi * i / (DECIMATE_TAPS - 1);
        double sinc = 2 * CUTOFF_HZ / RATE_HZ * (x ? sin(2 * pi * CUTOFF_HZ / RATE_HZ * x) / (2 * pi * CUTOFF_HZ / RATE_HZ * x) : 1);
        double window = 0.42 - 0.5 * cos(phase) + 0.08 * cos(2 * phase);
        state->taps[i] = (float)(sinc * window);
        sum += sinc * window;
    }
    for (int i = 0; i < DECIMATE_TAPS; ++i) state->taps[i] = (float)(state->taps[i] / sum); // 0 dB at DC.
    decimate_reset(state);
}

void decimate_reset(decimate_t *state)
{
    memset(state->history, 0, sizeof(state->history));
}

void decimate_process(decimate_t *state, const int16_t *in, size_t samples, int16_t *out)
{
    // history = the last TAPS-1 inputs, then this block.
    float *x = state->history;
    for (size_t i = 0; i < samples; ++i) x[DECIMATE_TAPS - 1 + i] = in[i];
    for (size_t k = 0; k < samples / FACTOR; ++k) {
        const float *window = x + k * FACTOR;
        float acc = 0;
        for (int i = 0; i < DECIMATE_TAPS; ++i) acc += state->taps[i] * window[i];
        acc = acc >= 0 ? acc + 0.5f : acc - 0.5f;
        out[k] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)acc;
    }
    memmove(x, x + samples, (DECIMATE_TAPS - 1) * sizeof(float));
}
