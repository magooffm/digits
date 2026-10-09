#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio_format.h"

#define RECORD_PLAYBACK_SAMPLES (DIGITS_AUDIO_SAMPLE_RATE * 2) // Two seconds.
#define RECORD_PLAYBACK_MAX_SAMPLES (DIGITS_AUDIO_SAMPLE_RATE * 10)

typedef struct {
    int32_t dc_offset;
    uint32_t raw_rms;
    uint32_t ac_rms;
    uint32_t input_peak; // Raw absolute PCM magnitude, including 32768.
    uint32_t gain_q8; // One fixed recording gain: 64..8192 means 0.25..32x.
} record_playback_gain_t;

typedef struct {
    uint64_t sum_squares;
    uint32_t samples;
    uint32_t peak;
    uint32_t limited_samples;
} record_playback_output_t;

// Analyze 1..RECORD_PLAYBACK_MAX_SAMPLES stored mono samples. Remove the
// integer mean DC offset and target AC RMS 2048, with gain limited to 0.25..32x.
// AC RMS below 16 retains quarter gain. RMS values are integer square roots.
// Returns false for invalid arguments, clearing gain when it is non-NULL.
bool record_playback_gain_prepare(const int16_t *mono, size_t samples,
                                  record_playback_gain_t *gain);

// Apply prepared DC removal/gain, clamp to +/-8192, then apply the same fades
// as the legacy renderer below. Invalid lengths/plans and frames beyond the
// recording produce silence. stereo holds 2 * frames samples. Optional output
// accumulates mono statistics after fading; initialize it to zero per recording.
// Count limiting before fading only for valid recording samples, including
// endpoints. Silent padding beyond the recording does not affect statistics.
void record_playback_render_gain(int16_t *stereo, const int16_t *mono,
                                 size_t samples, size_t first_frame, size_t frames,
                                 const record_playback_gain_t *gain,
                                 record_playback_output_t *output);

// Integer mono output RMS; empty/NULL statistics return zero.
uint32_t record_playback_output_rms(const record_playback_output_t *output);

// Render stored signed 16-bit mono into identical left/right stereo samples.
// mono holds samples samples; stereo holds 2 * frames samples.
// Apply quarter amplitude and linear fades of at most 5 ms, shortened to fit
// short recordings. Both endpoints are zero; 0/1/2-sample recordings are silent.
// Frames beyond the recording are silent and never read mono. Gain arithmetic
// uses signed 32-bit integers; divisions truncate toward zero.
void record_playback_render_samples(int16_t *stereo, const int16_t *mono,
                                    size_t samples, size_t first_frame, size_t frames);

// Compatibility wrapper for the fixed two-second recording.
void record_playback_render(int16_t *stereo, const int16_t *mono,
                            size_t first_frame, size_t frames);
