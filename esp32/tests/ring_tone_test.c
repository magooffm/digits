#include "ring_tone.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double two_pi = RING_TONE_TWO_PI;
static unsigned sine_calls;

// Optional instrumentation: compile ring_tone.c alone with
// -Dsinf=ring_tone_test_sinf, then link with this test compiled normally.
// Define RING_TONE_TEST_INSTRUMENTED when compiling the test to require it.
float ring_tone_test_sinf(float radians)
{
    ++sine_calls;
    return sinf(radians);
}

static double spectral_amplitude(const int16_t *pcm, double frequency)
{
    double real = 0.0, imaginary = 0.0;
    // A complete unfaded 25 ms period has exactly 11 and 12 tone cycles.
    for (size_t i = 0; i < RING_TONE_PERIOD_FRAMES; ++i) {
        const double phase = two_pi * frequency * (double)i / RING_TONE_SAMPLE_RATE;
        const int16_t sample = pcm[2 * (2 * RING_TONE_PERIOD_FRAMES + i)];
        real += sample * cos(phase);
        imaginary += sample * sin(phase);
    }
    return 2.0 * sqrt(real * real + imaginary * imaginary) / RING_TONE_PERIOD_FRAMES;
}

int main(void)
{
    // Check the shared value independently before using it in the reference.
    assert(fabs(two_pi - 2.0 * acos(-1.0)) < 1e-15);
    const size_t total = RING_TONE_FRAMES + RING_TONE_PERIOD_FRAMES + DIGITS_AUDIO_DMA_FRAMES;
    const size_t fade = RING_TONE_SAMPLE_RATE / 200;
    int16_t *pcm = malloc(total * 2 * sizeof(*pcm));
    assert(pcm != NULL);
    ring_tone_init();
    const unsigned calls_after_init = sine_calls;
#ifdef RING_TONE_TEST_INSTRUMENTED
    assert(calls_after_init == 2 * RING_TONE_PERIOD_FRAMES);
#endif
    ring_tone_render(pcm, 0, total);
    int peak = 0;
    double energy = 0.0, reference_energy = 0.0, error_energy = 0.0;
    int maximum_error = 0;
    for (size_t frame = 0; frame < total; ++frame) {
        assert(pcm[2 * frame] == pcm[2 * frame + 1]);
        const int level = abs(pcm[2 * frame]);
        assert(level <= 8192);
        if (level > peak) peak = level;
        energy += (double)level * level;
        if (frame >= RING_TONE_FRAMES) {
            assert(level == 0);
        } else {
            double envelope = 1.0;
            if (frame < fade) envelope = (double)frame / fade;
            const size_t remaining = RING_TONE_FRAMES - 1 - frame;
            if (remaining < fade) envelope = (double)remaining / fade;
            const double seconds = (double)frame / RING_TONE_SAMPLE_RATE;
            const int16_t reference = (int16_t)(4096.0 * envelope *
                (sin(two_pi * 440.0 * seconds) + sin(two_pi * 480.0 * seconds)));
            const int difference = abs(pcm[2 * frame] - reference);
            if (difference > maximum_error) maximum_error = difference;
            reference_energy += (double)reference * reference;
            error_energy += (double)difference * difference;
        }
    }
    assert(pcm[0] == 0 && pcm[2 * (RING_TONE_FRAMES - 1)] == 0);
    assert(peak > 7000 && energy > 1e10);
    assert(maximum_error <= 4);
    assert(sqrt(error_energy / RING_TONE_FRAMES) < 1.0);
    assert(fabs(energy / reference_energy - 1.0) < 0.0001);
    const double at_440 = spectral_amplitude(pcm, 440.0);
    const double at_480 = spectral_amplitude(pcm, 480.0);
    assert(at_440 > 4094.0 && at_440 < 4097.0);
    assert(at_480 > 4094.0 && at_480 < 4097.0);
    // The audible 40 Hz beating is the envelope of the two tones, not a third
    // low-frequency tone introduced by a discontinuous table boundary.
    assert(spectral_amplitude(pcm, 40.0) < 1.0);
    for (size_t frame = fade; frame < RING_TONE_FRAMES - fade - RING_TONE_PERIOD_FRAMES; ++frame)
        assert(pcm[2 * frame] == pcm[2 * (frame + RING_TONE_PERIOD_FRAMES)]);

    // Chunked streaming must preserve phase/fades even across table boundaries.
    const size_t chunks[] = {DIGITS_AUDIO_DMA_FRAMES, DIGITS_AUDIO_DMA_FRAMES - 1,
                             160, RING_TONE_PERIOD_FRAMES - 1, RING_TONE_PERIOD_FRAMES + 1};
    int16_t chunk[(RING_TONE_PERIOD_FRAMES + 1) * 2];
    for (size_t k = 0; k < sizeof(chunks) / sizeof(*chunks); ++k) {
        for (size_t first = 0; first < total; first += chunks[k]) {
            const size_t count = total - first < chunks[k] ? total - first : chunks[k];
            ring_tone_render(chunk, first, count);
            assert(memcmp(chunk, pcm + 2 * first, count * 2 * sizeof(*chunk)) == 0);
        }
    }
    ring_tone_render(chunk, SIZE_MAX, 4);
    for (size_t i = 0; i < 8; ++i) assert(chunk[i] == 0);
    ring_tone_render(chunk, RING_TONE_FRAMES - 1, 4);
    for (size_t i = 0; i < 8; ++i) assert(chunk[i] == 0);
    ring_tone_render(NULL, SIZE_MAX, 0);
    assert(sine_calls == calls_after_init);
    printf("PASS: bounded 1s periodic stereo, 440/480 Hz energy, fades, EOF and "
           "chunk continuity; max reference error=%d, init sine calls=%u, render calls=0\n",
           maximum_error, calls_after_init);
    free(pcm);
    return 0;
}
