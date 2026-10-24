#include "tone_gen.h"
#include <math.h>

// Every frequency is a multiple of 10 Hz, so sin(2*pi*f*n/48000) repeats
// over 4800 samples: index = (n * f/10) mod 4800. A quarter of that period
// (1200 steps, plus the endpoint) holds all distinct magnitudes.
#define CYCLE 4800U
#define QUARTER (CYCLE / 4)

static int16_t quarter[QUARTER + 1];

static const tone_gen_spec_t specs[] = {
    [TONE_GEN_DIAL] = {350, 440, 1000, 0},
    [TONE_GEN_RINGBACK] = {440, 480, 2000, 4000},
    [TONE_GEN_BUSY] = {480, 620, 500, 500},
    [TONE_GEN_REORDER] = {480, 620, 250, 250},
};

void tone_gen_init(void)
{
    for (unsigned i = 0; i <= QUARTER; ++i)
        quarter[i] = (int16_t)lround(32767.0 * sin(6.283185307179586 * i / CYCLE));
}

const tone_gen_spec_t *tone_gen_spec(tone_gen_tone_t tone)
{
    if (tone <= TONE_GEN_NONE || tone > TONE_GEN_REORDER) return NULL;
    return &specs[tone];
}

static int32_t sine(uint32_t index)
{
    uint32_t q = index / QUARTER, r = index % QUARTER;
    switch (q) {
    case 0: return quarter[r];
    case 1: return quarter[QUARTER - r];
    case 2: return -quarter[r];
    default: return -quarter[QUARTER - r];
    }
}

void tone_gen_render(tone_gen_tone_t tone, uint64_t position, int16_t *stereo, size_t frames)
{
    const tone_gen_spec_t *spec = tone_gen_spec(tone);
    uint64_t on = spec ? (uint64_t)spec->on_ms * (TONE_GEN_SAMPLE_RATE / 1000) : 0;
    uint64_t period = spec ? on + (uint64_t)spec->off_ms * (TONE_GEN_SAMPLE_RATE / 1000) : 0;
    for (size_t i = 0; i < frames; ++i) {
        uint64_t n = position + i;
        int16_t value = 0;
        if (spec && (!spec->off_ms || n % period < on)) {
            uint32_t low = (uint32_t)((n % CYCLE) * (spec->low_hz / 10) % CYCLE);
            uint32_t high = (uint32_t)((n % CYCLE) * (spec->high_hz / 10) % CYCLE);
            value = (int16_t)((sine(low) + sine(high)) * TONE_GEN_COMPONENT_PEAK / 32767);
        }
        stereo[2 * i] = stereo[2 * i + 1] = value;
    }
}
