#include "record_playback_pcm.h"

#include <limits.h>
#include <string.h>

static uint32_t integer_sqrt(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

bool record_playback_gain_prepare(const int16_t *mono, size_t samples,
                                  record_playback_gain_t *gain)
{
    if (gain == NULL) return false;
    memset(gain, 0, sizeof(*gain));
    if (mono == NULL || !samples || samples > RECORD_PLAYBACK_MAX_SAMPLES) return false;

    int64_t sum = 0;
    uint64_t raw_squares = 0, ac_squares = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t sample = mono[i];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        sum += sample;
        raw_squares += (uint64_t)magnitude * magnitude;
        if (magnitude > gain->input_peak) gain->input_peak = magnitude;
    }
    gain->dc_offset = (int32_t)(sum / (int64_t)samples);
    for (size_t i = 0; i < samples; ++i) {
        const int32_t centered = (int32_t)mono[i] - gain->dc_offset;
        const uint32_t magnitude = (uint32_t)(centered < 0 ? -centered : centered);
        ac_squares += (uint64_t)magnitude * magnitude;
    }
    gain->raw_rms = integer_sqrt(raw_squares / samples);
    gain->ac_rms = integer_sqrt(ac_squares / samples);
    gain->gain_q8 = 64;
    if (gain->ac_rms >= 16) {
        gain->gain_q8 = (2048U * 256U) / gain->ac_rms;
        if (gain->gain_q8 < 64) gain->gain_q8 = 64;
        if (gain->gain_q8 > 8192) gain->gain_q8 = 8192;
    }
    return true;
}

void record_playback_render_gain(int16_t *stereo, const int16_t *mono,
                                 size_t samples, size_t first_frame, size_t frames,
                                 const record_playback_gain_t *gain,
                                 record_playback_output_t *output)
{
    const bool valid = mono != NULL && gain != NULL && samples > 0 &&
        samples <= RECORD_PLAYBACK_MAX_SAMPLES &&
        gain->dc_offset >= INT16_MIN && gain->dc_offset <= INT16_MAX &&
        gain->gain_q8 >= 64 && gain->gain_q8 <= 8192;
    size_t fade_samples = samples > 1 ? (samples - 1) / 2 : 0;
    const size_t maximum_fade = DIGITS_AUDIO_SAMPLE_RATE / 200;
    if (fade_samples > maximum_fade) fade_samples = maximum_fade;
    for (size_t i = 0; i < frames; ++i) {
        int32_t sample = 0;
        if (valid && first_frame < samples && i < samples - first_frame) {
            const size_t frame = first_frame + i;
            const int32_t centered = (int32_t)mono[frame] - gain->dc_offset;
            // Bound the plan above so even centered magnitude 65535 at 32x
            // fits int32_t. Cast Q8 to signed before multiplying negative PCM.
            sample = centered * (int32_t)gain->gain_q8 / 256;
            const bool limited = sample > 8192 || sample < -8192;
            if (sample > 8192) sample = 8192;
            if (sample < -8192) sample = -8192;
            if (fade_samples) {
                size_t envelope = fade_samples;
                if (frame < envelope) envelope = frame;
                const size_t remaining = samples - 1 - frame;
                if (remaining < envelope) envelope = remaining;
                sample = sample * (int32_t)envelope / (int32_t)fade_samples;
            } else {
                sample = 0; // One or two samples consist only of endpoints.
            }
            if (output != NULL) {
                const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
                output->sum_squares += (uint64_t)magnitude * magnitude;
                ++output->samples;
                output->limited_samples += limited;
                if (magnitude > output->peak) output->peak = magnitude;
            }
        }
        stereo[2 * i] = (int16_t)sample;
        stereo[2 * i + 1] = (int16_t)sample;
    }
}

uint32_t record_playback_output_rms(const record_playback_output_t *output)
{
    if (output == NULL || !output->samples) return 0;
    return integer_sqrt(output->sum_squares / output->samples);
}

void record_playback_render_samples(int16_t *stereo, const int16_t *mono,
                                    size_t samples, size_t first_frame, size_t frames)
{
    size_t fade_samples = samples > 1 ? (samples - 1) / 2 : 0;
    const size_t maximum_fade = DIGITS_AUDIO_SAMPLE_RATE / 200; // 5 ms.
    if (fade_samples > maximum_fade) fade_samples = maximum_fade;
    const int32_t divisor = (int32_t)fade_samples * 4;
    for (size_t i = 0; i < frames; ++i) {
        int16_t sample = 0;
        // Check before adding i so an out-of-range start cannot wrap around
        // and accidentally read the beginning of the recording.
        if (fade_samples && first_frame < samples && i < samples - first_frame) {
            const size_t frame = first_frame + i;
            int32_t envelope = (int32_t)fade_samples;
            if (frame < fade_samples) envelope = (int32_t)frame;
            const size_t remaining = samples - 1 - frame;
            if (remaining < fade_samples) envelope = (int32_t)remaining;
            if (envelope) sample = (int16_t)((int32_t)mono[frame] * envelope / divisor);
        }
        stereo[2 * i] = sample;
        stereo[2 * i + 1] = sample;
    }
}

void record_playback_render(int16_t *stereo, const int16_t *mono,
                            size_t first_frame, size_t frames)
{
    record_playback_render_samples(stereo, mono, RECORD_PLAYBACK_SAMPLES, first_frame, frames);
}
