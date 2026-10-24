#pragma once

// Call-progress tones, North American precise tone plan, matching the Pi's
// pi/tones/tone_*.wav: dial 350+440 Hz continuous, ringback 440+480 Hz 2 s on
// 4 s off, busy 480+620 Hz 0.5/0.5 s, reorder 480+620 Hz 0.25/0.25 s.
// Pure integer rendering after tone_gen_init(); no ESP-IDF dependency.

#include <stddef.h>
#include <stdint.h>

#define TONE_GEN_SAMPLE_RATE 48000
// Per frequency component; the two-tone peak (4096) matches the incoming
// ringtone level on the board speaker.
#define TONE_GEN_COMPONENT_PEAK 2048

typedef enum {
    TONE_GEN_NONE,
    TONE_GEN_DIAL,
    TONE_GEN_RINGBACK,
    TONE_GEN_BUSY,
    TONE_GEN_REORDER,
} tone_gen_tone_t;

typedef struct {
    uint16_t low_hz, high_hz;
    uint16_t on_ms, off_ms; // off_ms 0 means continuous.
} tone_gen_spec_t;

// Builds the shared quarter-wave table. Call once before any render.
void tone_gen_init(void);

// NULL for TONE_GEN_NONE or an unknown value.
const tone_gen_spec_t *tone_gen_spec(tone_gen_tone_t tone);

// Signed 16-bit stereo with identical channels. position is the frame index
// since the tone started; samples depend only on it, so rendering is exact
// and drift-free for any block size. Cadence pauses and NONE render silence.
void tone_gen_render(tone_gen_tone_t tone, uint64_t position, int16_t *stereo, size_t frames);
