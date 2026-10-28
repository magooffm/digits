#pragma once

#include <stddef.h>
#include <stdint.h>

// 48 kHz to 16 kHz for the outgoing voice: a 64-tap Blackman-windowed
// lowpass (flat to about 5 kHz, -6 dB at 7.2 kHz, below -70 dB from
// 9.5 kHz), then every third sample. State carries across calls, so a stream
// may be cut into blocks of any multiple of three.

#define DECIMATE_TAPS 64
#define DECIMATE_MAX_INPUT 960 // One 20 ms frame at 48 kHz.

typedef struct {
    float taps[DECIMATE_TAPS];
    float history[DECIMATE_TAPS - 1 + DECIMATE_MAX_INPUT];
} decimate_t;

void decimate_init(decimate_t *state);
void decimate_reset(decimate_t *state); // Silence before the next block.
// samples: a multiple of 3, at most DECIMATE_MAX_INPUT; writes samples / 3.
void decimate_process(decimate_t *state, const int16_t *in, size_t samples, int16_t *out);
