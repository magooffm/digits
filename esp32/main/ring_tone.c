#include "ring_tone.h"
#include <math.h>

_Static_assert(RING_TONE_SAMPLE_RATE % 40 == 0,
               "440+480 Hz needs a whole 25 ms sample period");

static int16_t tone_period[RING_TONE_PERIOD_FRAMES];

void ring_tone_init(void)
{
    const float two_pi = (float)RING_TONE_TWO_PI;
    // In 25 ms, 440 Hz completes 11 cycles and 480 Hz completes 12. Generate
    // this period once at startup, before the playback worker starts.
    for (size_t frame = 0; frame < RING_TONE_PERIOD_FRAMES; ++frame) {
        const float seconds = (float)frame / RING_TONE_SAMPLE_RATE;
        tone_period[frame] = (int16_t)(4096.0f *
            (sinf(two_pi * 440.0f * seconds) + sinf(two_pi * 480.0f * seconds)));
    }
}

void ring_tone_render(int16_t *pcm, size_t first_frame, size_t frames)
{
    const size_t fade_frames = RING_TONE_SAMPLE_RATE / 200; // 5 ms at each end.
    size_t period_frame = first_frame % RING_TONE_PERIOD_FRAMES;
    for (size_t i = 0; i < frames; ++i) {
        int16_t sample = 0;
        // Check before addition: an out-of-range start, even SIZE_MAX, must
        // stay silent rather than wrap into the start of the waveform.
        if (first_frame < RING_TONE_FRAMES && i < RING_TONE_FRAMES - first_frame) {
            const size_t frame = first_frame + i;
            size_t envelope = fade_frames;
            if (frame < envelope) envelope = frame;
            const size_t remaining = RING_TONE_FRAMES - 1 - frame;
            if (remaining < envelope) envelope = remaining;
            sample = tone_period[period_frame];
            if (envelope < fade_frames)
                sample = (int16_t)((int32_t)sample * (int32_t)envelope / (int32_t)fade_frames);
        }
        pcm[2 * i] = sample;
        pcm[2 * i + 1] = sample;
        if (++period_frame == RING_TONE_PERIOD_FRAMES) period_frame = 0;
    }
}
