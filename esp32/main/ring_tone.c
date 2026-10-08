#include "ring_tone.h"
#include <math.h>

void ring_tone_render(int16_t *pcm, size_t first_frame, size_t frames)
{
    const float two_pi = 6.28318530718f;
    const size_t fade_frames = RING_TONE_SAMPLE_RATE / 200; // 5 ms at each end.
    for (size_t i = 0; i < frames; ++i) {
        size_t frame = first_frame + i;
        int16_t sample = 0;
        if (frame < RING_TONE_FRAMES) {
            float envelope = 1.0f;
            if (frame < fade_frames) envelope = (float)frame / fade_frames;
            size_t remaining = RING_TONE_FRAMES - 1 - frame;
            if (remaining < fade_frames) envelope = (float)remaining / fade_frames;
            float seconds = (float)frame / RING_TONE_SAMPLE_RATE;
            // Dual-tone telephone-style ring, peak <= 8192 (25% of full scale).
            sample = (int16_t)(4096.0f * envelope *
                (sinf(two_pi * 440.0f * seconds) + sinf(two_pi * 480.0f * seconds)));
        }
        pcm[2 * i] = sample;
        pcm[2 * i + 1] = sample;
    }
}
