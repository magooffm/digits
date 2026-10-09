#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_format.h"

#define RING_TONE_SAMPLE_RATE DIGITS_AUDIO_SAMPLE_RATE
#define RING_TONE_FRAMES RING_TONE_SAMPLE_RATE // One second.

// Signed 16-bit stereo, identical channels. Frames outside the tone are silent.
void ring_tone_render(int16_t *pcm, size_t first_frame, size_t frames);
