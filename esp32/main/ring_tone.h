#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_format.h"

#define RING_TONE_SAMPLE_RATE DIGITS_AUDIO_SAMPLE_RATE
#define RING_TONE_FRAMES RING_TONE_SAMPLE_RATE // One second.
#define RING_TONE_PERIOD_FRAMES (RING_TONE_SAMPLE_RATE / 40) // 25 ms.
#define RING_TONE_TWO_PI 6.283185307179586476925286766559

// Prepare the 440+480 Hz mono period before starting any rendering tasks.
// Initialization uses floating-point sine; render is allocation-free integer
// work only. Call init before concurrent readers and do not reinitialize while
// rendering. At 48 kHz the table holds 1200 samples (2400 bytes).
void ring_tone_init(void);

// Signed 16-bit stereo, identical channels. Frames outside the tone are silent.
void ring_tone_render(int16_t *pcm, size_t first_frame, size_t frames);
