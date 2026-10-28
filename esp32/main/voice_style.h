#pragma once

// "Copper Wire" voice style: the Pi's telephone-band character chain on the
// outgoing microphone audio, two 300 Hz high-pass and two 3,400 Hz low-pass
// RBJ Butterworth sections (Q = 1/sqrt 2), direct form I, truncated and
// clamped to 16 bits like pi/digitsd/internal/audio/filters.go
// (NewPOTSCharacterChain). Single precision: the ESP32-S3 has no double FPU.
// Pure C: tests/voice_style_test.c runs on the host.

#include <stddef.h>
#include <stdint.h>

#define VOICE_STYLE_SECTIONS 4

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} voice_style_section_t;

typedef struct {
    voice_style_section_t section[VOICE_STYLE_SECTIONS];
} voice_style_copper_t;

void voice_style_copper_init(voice_style_copper_t *filter, unsigned sample_rate);
// Clears the history: a fresh start when Copper is chosen again.
void voice_style_copper_reset(voice_style_copper_t *filter);
void voice_style_copper_process(voice_style_copper_t *filter, int16_t *pcm, size_t count);
