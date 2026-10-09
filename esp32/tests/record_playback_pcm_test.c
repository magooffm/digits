#include "record_playback_pcm.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_signed_gain(int16_t *mono)
{
    const size_t first = DIGITS_AUDIO_SAMPLE_RATE / 200;
    mono[first] = INT16_MIN;
    mono[first + 1] = INT16_MAX;
    mono[first + 2] = -12345;
    mono[first + 3] = 12345;
    const int16_t expected[] = {-8192, 8191, -3086, 3086};
    int16_t stereo[8];
    record_playback_render(stereo, mono, first, 4);
    for (size_t i = 0; i < 4; ++i) {
        assert(stereo[2 * i] == expected[i]);
        assert(stereo[2 * i + 1] == expected[i]);
    }
}

static void test_fades_and_duration(int16_t *mono, int16_t *stereo)
{
    const size_t fade = DIGITS_AUDIO_SAMPLE_RATE / 200;
    const size_t total = RECORD_PLAYBACK_SAMPLES + DIGITS_AUDIO_DMA_FRAMES;
    assert(RECORD_PLAYBACK_SAMPLES == 96000);
    for (size_t i = 0; i < RECORD_PLAYBACK_SAMPLES; ++i) mono[i] = INT16_MIN;
    record_playback_render(stereo, mono, 0, total);
    assert(stereo[0] == 0);
    assert(stereo[2 * (RECORD_PLAYBACK_SAMPLES - 1)] == 0);
    assert(stereo[2 * (fade / 2)] == -4096);
    assert(stereo[2 * (RECORD_PLAYBACK_SAMPLES - 1 - fade / 2)] == -4096);
    int32_t peak = 0;
    for (size_t i = 0; i < total; ++i) {
        assert(stereo[2 * i] == stereo[2 * i + 1]);
        const int32_t magnitude = -(int32_t)stereo[2 * i];
        assert(magnitude >= 0 && magnitude <= 8192);
        if (magnitude > peak) peak = magnitude;
        if (i >= RECORD_PLAYBACK_SAMPLES) {
            assert(magnitude == 0);
        } else if (i > 0 && i < RECORD_PLAYBACK_SAMPLES - 1) {
            assert(magnitude > 0);
            if (i >= fade && i < RECORD_PLAYBACK_SAMPLES - fade) {
                assert(magnitude == 8192);
            }
        }
    }
    assert(peak == 8192);
    for (size_t i = 1; i <= fade; ++i) {
        const int32_t ramp = -(int32_t)stereo[2 * i];
        const int32_t previous = -(int32_t)stereo[2 * (i - 1)];
        assert(ramp >= previous && ramp - previous <= 35);
        assert(stereo[2 * i] == stereo[2 * (RECORD_PLAYBACK_SAMPLES - 1 - i)]);
    }
}

static void test_outside_recording(void)
{
    int16_t stereo[8];
    // NULL mono proves these positions never attempt a recording read.
    memset(stereo, 1, sizeof(stereo));
    record_playback_render(stereo, NULL, RECORD_PLAYBACK_SAMPLES, 4);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
    memset(stereo, 1, sizeof(stereo));
    record_playback_render(stereo, NULL, SIZE_MAX, 4);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
    record_playback_render(NULL, NULL, 0, 0);
}

static void test_chunk_continuity(int16_t *mono, int16_t *stereo)
{
    const int16_t pattern[] = {INT16_MIN, INT16_MAX, -12345, 12345, 0};
    const size_t total = RECORD_PLAYBACK_SAMPLES + DIGITS_AUDIO_DMA_FRAMES;
    for (size_t i = 0; i < RECORD_PLAYBACK_SAMPLES; ++i) mono[i] = pattern[i % 5];
    record_playback_render(stereo, mono, 0, total);
    // Use both DMA-sized chunks and chunks crossing DMA/fade boundaries.
    for (size_t chunk_size = DIGITS_AUDIO_DMA_FRAMES - 1;
         chunk_size <= DIGITS_AUDIO_DMA_FRAMES; ++chunk_size) {
        int16_t chunk[DIGITS_AUDIO_DMA_FRAMES * 2];
        for (size_t first = 0; first < total; first += chunk_size) {
            const size_t count = total - first < chunk_size ? total - first : chunk_size;
            record_playback_render(chunk, mono, first, count);
            assert(memcmp(chunk, stereo + 2 * first,
                          count * 2 * sizeof(*chunk)) == 0);
        }
    }
}

static void test_empty_and_tiny_recordings(void)
{
    int16_t stereo[16];
    for (size_t samples = 0; samples <= 2; ++samples) {
        memset(stereo, 1, sizeof(stereo));
        record_playback_render_samples(stereo, NULL, samples, 0, 8);
        for (size_t i = 0; i < 16; ++i) assert(stereo[i] == 0);
    }
    const int16_t mono[] = {INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN};
    const int16_t expected[][5] = {
        {0, -8192, 0, 0, 0},
        {0, -8192, -8192, 0, 0},
        {0, -4096, -8192, -4096, 0},
    };
    for (size_t samples = 3; samples <= 5; ++samples) {
        record_playback_render_samples(stereo, mono, samples, 0, 8);
        for (size_t i = 0; i < 8; ++i) {
            const int16_t sample = i < 5 ? expected[samples - 3][i] : 0;
            assert(stereo[2 * i] == sample && stereo[2 * i + 1] == sample);
        }
    }
    record_playback_render_samples(NULL, NULL, 0, 0, 0);
    record_playback_render_samples(NULL, NULL, SIZE_MAX, 0, 0);
}

static void test_variable_bounds(void)
{
    int16_t stereo[8];
    const size_t lengths[] = {0, 1, 5, 12345, RECORD_PLAYBACK_SAMPLES + 17, SIZE_MAX};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i) {
        memset(stereo, 1, sizeof(stereo));
        record_playback_render_samples(stereo, NULL, lengths[i], lengths[i], 4);
        for (size_t j = 0; j < 8; ++j) assert(stereo[j] == 0);
        memset(stereo, 1, sizeof(stereo));
        record_playback_render_samples(stereo, NULL, lengths[i], SIZE_MAX, 4);
        for (size_t j = 0; j < 8; ++j) assert(stereo[j] == 0);
    }
    // The final endpoint is zero and must not overflow when followed by silence.
    record_playback_render_samples(stereo, NULL, SIZE_MAX, SIZE_MAX - 1, 4);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
}

static void test_variable_length(size_t samples)
{
    const size_t total = samples + DIGITS_AUDIO_DMA_FRAMES;
    int16_t *mono = malloc(samples * sizeof(*mono));
    int16_t *stereo = malloc(total * 2 * sizeof(*stereo));
    assert(mono != NULL && stereo != NULL);
    for (size_t i = 0; i < samples; ++i) mono[i] = INT16_MIN;
    record_playback_render_samples(stereo, mono, samples, 0, total);
    int32_t peak = 0;
    for (size_t i = 0; i < total; ++i) {
        assert(stereo[2 * i] == stereo[2 * i + 1]);
        const int32_t magnitude = -(int32_t)stereo[2 * i];
        assert(magnitude >= 0 && magnitude <= 8192);
        if (magnitude > peak) peak = magnitude;
        if (i >= samples) assert(magnitude == 0);
        else {
            assert(stereo[2 * i] == stereo[2 * (samples - 1 - i)]);
            if (i > 0 && i < samples - 1) assert(magnitude > 0);
        }
    }
    assert(stereo[0] == 0 && stereo[2 * (samples - 1)] == 0);
    assert(peak == 8192);
    if (samples > 2 * (DIGITS_AUDIO_SAMPLE_RATE / 200)) {
        assert(stereo[2 * (DIGITS_AUDIO_SAMPLE_RATE / 200)] == -8192);
        assert(stereo[2 * (samples - 1 - DIGITS_AUDIO_SAMPLE_RATE / 200)] == -8192);
    }

    const int16_t pattern[] = {INT16_MIN, INT16_MAX, -12345, 12345, 0};
    for (size_t i = 0; i < samples; ++i) mono[i] = pattern[i % 5];
    record_playback_render_samples(stereo, mono, samples, 0, total);
    const size_t chunk_sizes[] = {3, DIGITS_AUDIO_DMA_FRAMES - 1, DIGITS_AUDIO_DMA_FRAMES};
    for (size_t k = 0; k < sizeof(chunk_sizes) / sizeof(*chunk_sizes); ++k) {
        int16_t chunk[DIGITS_AUDIO_DMA_FRAMES * 2];
        for (size_t first = 0; first < total; first += chunk_sizes[k]) {
            const size_t count = total - first < chunk_sizes[k] ? total - first : chunk_sizes[k];
            record_playback_render_samples(chunk, mono, samples, first, count);
            assert(memcmp(chunk, stereo + 2 * first, count * 2 * sizeof(*chunk)) == 0);
        }
    }
    free(stereo);
    free(mono);
}

static void assert_gain_cleared(const record_playback_gain_t *gain)
{
    assert(!gain->dc_offset && !gain->raw_rms && !gain->ac_rms &&
           !gain->input_peak && !gain->gain_q8);
}

static void test_gain_prepare(void)
{
    int16_t mono[960];
    record_playback_gain_t gain;
    assert(!record_playback_gain_prepare(mono, 1, NULL));
    memset(&gain, 1, sizeof(gain));
    assert(!record_playback_gain_prepare(NULL, 1, &gain));
    assert_gain_cleared(&gain);
    memset(&gain, 1, sizeof(gain));
    assert(!record_playback_gain_prepare(mono, 0, &gain));
    assert_gain_cleared(&gain);
    memset(&gain, 1, sizeof(gain));
    assert(!record_playback_gain_prepare(mono, RECORD_PLAYBACK_MAX_SAMPLES + 1, &gain));
    assert_gain_cleared(&gain);
    memset(&gain, 1, sizeof(gain));
    assert(!record_playback_gain_prepare(mono, SIZE_MAX, &gain));
    assert_gain_cleared(&gain);

    memset(mono, 0, sizeof(mono));
    assert(record_playback_gain_prepare(mono, 960, &gain));
    assert(gain.dc_offset == 0 && gain.raw_rms == 0 && gain.ac_rms == 0);
    assert(gain.input_peak == 0 && gain.gain_q8 == 64);
    for (size_t i = 0; i < 960; ++i) mono[i] = -3000;
    assert(record_playback_gain_prepare(mono, 960, &gain));
    assert(gain.dc_offset == -3000 && gain.raw_rms == 3000 && gain.ac_rms == 0);
    assert(gain.input_peak == 3000 && gain.gain_q8 == 64);
    int16_t stereo[8];
    record_playback_render_gain(stereo, mono, 960, 240, 4, &gain, NULL);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);

    for (size_t i = 0; i < 960; ++i) mono[i] = (int16_t)(1000 + (i % 2 ? -8 : 8));
    assert(record_playback_gain_prepare(mono, 960, &gain));
    assert(gain.dc_offset == 1000 && gain.raw_rms == 1000 && gain.ac_rms == 8);
    assert(gain.input_peak == 1008 && gain.gain_q8 == 64);
    record_playback_render_gain(stereo, mono, 960, 240, 4, &gain, NULL);
    assert(stereo[0] == 2 && stereo[2] == -2 && stereo[4] == 2 && stereo[6] == -2);
    const int16_t noise[] = {-12, -4, 4, 12};
    assert(record_playback_gain_prepare(noise, 4, &gain));
    assert(gain.dc_offset == 0 && gain.raw_rms == 8 && gain.ac_rms == 8 && gain.gain_q8 == 64);
    for (size_t i = 0; i < 960; ++i) mono[i] = i % 2 ? -16 : 16;
    assert(record_playback_gain_prepare(mono, 960, &gain));
    assert(gain.ac_rms == 16 && gain.gain_q8 == 8192);
    record_playback_render_gain(stereo, mono, 960, 240, 4, &gain, NULL);
    assert(stereo[0] == 512 && stereo[2] == -512);

    int16_t *maximum = malloc(RECORD_PLAYBACK_MAX_SAMPLES * sizeof(*maximum));
    assert(maximum != NULL);
    for (size_t i = 0; i < RECORD_PLAYBACK_MAX_SAMPLES; ++i) maximum[i] = INT16_MIN;
    assert(record_playback_gain_prepare(maximum, RECORD_PLAYBACK_MAX_SAMPLES, &gain));
    assert(gain.dc_offset == INT16_MIN && gain.raw_rms == 32768 && !gain.ac_rms);
    assert(gain.input_peak == 32768 && gain.gain_q8 == 64);
    // A single opposite-polarity sample exercises squared centered magnitudes
    // greater than INT32_MAX without biasing the mean toward the outlier.
    maximum[240] = INT16_MAX;
    assert(record_playback_gain_prepare(maximum, RECORD_PLAYBACK_MAX_SAMPLES, &gain));
    assert(gain.dc_offset == -32767 && gain.ac_rms > 16 && gain.input_peak == 32768);
    record_playback_render_gain(stereo, maximum, RECORD_PLAYBACK_MAX_SAMPLES, 240, 1, &gain, NULL);
    assert(stereo[0] == 8192 && stereo[1] == 8192);
    free(maximum);
}

static void test_gain_limits_and_metrics(void)
{
    record_playback_gain_t gain = {.gain_q8 = 256};
    const int16_t mono[] = {16384, 16384, 16384, 16384, 16384};
    const int16_t expected[] = {0, 4096, 8192, 4096, 0, 0, 0, 0};
    int16_t stereo[16];
    record_playback_output_t output = {0};
    record_playback_render_gain(stereo, mono, 5, 0, 8, &gain, &output);
    for (size_t i = 0; i < 8; ++i)
        assert(stereo[2 * i] == expected[i] && stereo[2 * i + 1] == expected[i]);
    // Limiting occurs before the fade; both zero endpoints are valid samples.
    assert(output.samples == 5 && output.peak == 8192 && output.limited_samples == 5);
    assert(output.sum_squares == UINT64_C(100663296));
    assert(record_playback_output_rms(&output) == 4486);

    int16_t extremes[512] = {0};
    gain.dc_offset = INT16_MIN;
    gain.gain_q8 = 8192;
    extremes[240] = INT16_MAX; // Centered +65535 at the maximum 32x gain.
    memset(&output, 0, sizeof(output));
    record_playback_render_gain(stereo, extremes, 512, 240, 1, &gain, &output);
    assert(stereo[0] == 8192 && output.limited_samples == 1);
    gain.dc_offset = INT16_MAX;
    extremes[240] = INT16_MIN; // Centered -65535, with signed multiplication.
    record_playback_render_gain(stereo, extremes, 512, 240, 1, &gain, &output);
    assert(stereo[0] == -8192 && output.limited_samples == 2);
    gain.dc_offset = 0;
    extremes[240] = 256;
    extremes[241] = 257;
    extremes[242] = -256;
    extremes[243] = -257;
    memset(&output, 0, sizeof(output));
    record_playback_render_gain(stereo, extremes, 512, 240, 4, &gain, &output);
    assert(stereo[0] == 8192 && stereo[2] == 8192 && stereo[4] == -8192 && stereo[6] == -8192);
    assert(output.samples == 4 && output.limited_samples == 2);
    assert(record_playback_output_rms(&output) == 8192);

    const record_playback_output_t large = {.sum_squares = UINT64_MAX, .samples = 1};
    assert(record_playback_output_rms(&large) == UINT32_MAX);
    const record_playback_output_t empty = {0};
    assert(record_playback_output_rms(&empty) == 0 && record_playback_output_rms(NULL) == 0);
}

static void test_voice_gain_with_click(int16_t *mono, int16_t *stereo)
{
    for (size_t i = 0; i < RECORD_PLAYBACK_SAMPLES; ++i) mono[i] = i % 2 ? -200 : 200;
    mono[1000] = 6479;
    record_playback_gain_t gain;
    assert(record_playback_gain_prepare(mono, RECORD_PLAYBACK_SAMPLES, &gain));
    assert(gain.dc_offset == 0 && gain.ac_rms == 201 && gain.raw_rms == 201);
    assert(gain.input_peak == 6479 && gain.gain_q8 == 2608);
    record_playback_output_t output = {0};
    record_playback_render_gain(stereo, mono, RECORD_PLAYBACK_SAMPLES, 0,
                                RECORD_PLAYBACK_SAMPLES, &gain, &output);
    assert(stereo[2 * 240] == 2037 && stereo[2 * 241] == -2037);
    assert(stereo[2 * 1000] == 8192 && output.limited_samples == 1);
    assert(output.samples == RECORD_PLAYBACK_SAMPLES && output.peak == 8192);
    assert(record_playback_output_rms(&output) > 1900 && record_playback_output_rms(&output) < 2200);
}

static void test_gain_bounds(void)
{
    const int16_t mono[] = {123};
    record_playback_gain_t gain = {.gain_q8 = 256};
    record_playback_output_t output = {0};
    int16_t stereo[8];
    record_playback_render_gain(stereo, mono, 1, SIZE_MAX, 4, &gain, &output);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
    record_playback_render_gain(stereo, NULL, SIZE_MAX, SIZE_MAX - 1, 4, &gain, &output);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
    record_playback_render_gain(stereo, mono, 0, 0, 4, &gain, &output);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
    gain.dc_offset = INT32_MIN;
    gain.gain_q8 = UINT32_MAX;
    record_playback_render_gain(stereo, mono, 1, 0, 4, &gain, &output);
    for (size_t i = 0; i < 8; ++i) assert(stereo[i] == 0);
    assert(!output.samples && !output.sum_squares && !output.peak && !output.limited_samples);
    record_playback_render_gain(NULL, NULL, 0, 0, 0, NULL, NULL);
}

static void test_gain_chunk_continuity(size_t samples)
{
    const size_t total = samples + DIGITS_AUDIO_DMA_FRAMES;
    int16_t *mono = malloc(samples * sizeof(*mono));
    int16_t *stereo = malloc(total * 2 * sizeof(*stereo));
    assert(mono != NULL && stereo != NULL);
    for (size_t i = 0; i < samples; ++i) mono[i] = (int16_t)(1000 + (i % 2 ? -200 : 200));
    if (samples > 480) mono[240] = 6479;
    record_playback_gain_t gain;
    assert(record_playback_gain_prepare(mono, samples, &gain));
    record_playback_output_t whole = {0};
    record_playback_render_gain(stereo, mono, samples, 0, total, &gain, &whole);
    assert(whole.samples == samples);
    assert(stereo[0] == 0 && stereo[2 * (samples - 1)] == 0);
    uint64_t measured_squares = 0;
    uint32_t measured_peak = 0;
    for (size_t i = 0; i < total; ++i) {
        assert(stereo[2 * i] == stereo[2 * i + 1]);
        const int32_t sample = stereo[2 * i];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        assert(magnitude <= 8192);
        if (i >= samples) assert(sample == 0);
        else {
            measured_squares += (uint64_t)magnitude * magnitude;
            if (magnitude > measured_peak) measured_peak = magnitude;
        }
    }
    assert(whole.sum_squares == measured_squares && whole.peak == measured_peak);
    const size_t chunk_sizes[] = {3, DIGITS_AUDIO_DMA_FRAMES - 1, DIGITS_AUDIO_DMA_FRAMES};
    for (size_t k = 0; k < sizeof(chunk_sizes) / sizeof(*chunk_sizes); ++k) {
        record_playback_output_t parts = {0};
        int16_t chunk[DIGITS_AUDIO_DMA_FRAMES * 2];
        for (size_t first = 0; first < total; first += chunk_sizes[k]) {
            const size_t count = total - first < chunk_sizes[k] ? total - first : chunk_sizes[k];
            record_playback_render_gain(chunk, mono, samples, first, count, &gain, &parts);
            assert(memcmp(chunk, stereo + 2 * first, count * 2 * sizeof(*chunk)) == 0);
        }
        assert(parts.samples == whole.samples && parts.sum_squares == whole.sum_squares);
        assert(parts.peak == whole.peak && parts.limited_samples == whole.limited_samples);
        assert(record_playback_output_rms(&parts) == record_playback_output_rms(&whole));
    }
    free(stereo);
    free(mono);
}

int main(void)
{
    // Allocate exactly the documented mono length: ASan catches a read past it
    // while the stereo renderer intentionally continues into trailing silence.
    int16_t *mono = malloc(RECORD_PLAYBACK_SAMPLES * sizeof(*mono));
    int16_t *stereo = malloc((RECORD_PLAYBACK_SAMPLES + DIGITS_AUDIO_DMA_FRAMES)
                            * 2 * sizeof(*stereo));
    assert(mono != NULL && stereo != NULL);
    test_signed_gain(mono);
    test_fades_and_duration(mono, stereo);
    test_outside_recording();
    test_chunk_continuity(mono, stereo);
    test_empty_and_tiny_recordings();
    test_variable_bounds();
    const size_t variable_lengths[] = {3, 4, 5, 17, 240, 480, 481, 12345, RECORD_PLAYBACK_SAMPLES + 73};
    for (size_t i = 0; i < sizeof(variable_lengths) / sizeof(*variable_lengths); ++i)
        test_variable_length(variable_lengths[i]);
    test_gain_prepare();
    test_gain_limits_and_metrics();
    test_voice_gain_with_click(mono, stereo);
    test_gain_bounds();
    const size_t gain_lengths[] = {1, 2, 3, 5, 17, 481, 12345, RECORD_PLAYBACK_MAX_SAMPLES};
    for (size_t i = 0; i < sizeof(gain_lengths) / sizeof(*gain_lengths); ++i)
        test_gain_chunk_continuity(gain_lengths[i]);
    free(stereo);
    free(mono);
    puts("PASS: legacy quarter playback, integer DC/RMS gain, silence guard, "
         "voice/click limiting, signed extremes, metrics, fades and chunk continuity");
    return 0;
}
