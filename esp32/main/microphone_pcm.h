#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MICROPHONE_PCM_LEFT,
    MICROPHONE_PCM_RIGHT,
    MICROPHONE_PCM_AVERAGE,
} microphone_pcm_slot_t;

typedef struct {
    uint64_t sample_count;
    uint64_t sum_squares;
    uint64_t zero_count;
    uint64_t clipped_count;
    uint32_t peak; // Absolute magnitude; INT16_MIN has magnitude 32768.
} microphone_pcm_level_t;

typedef struct {
    microphone_pcm_level_t left;
    microphone_pcm_level_t right;
    microphone_pcm_level_t mono;
} microphone_pcm_stats_t;

// Begin a fresh statistics window. Reset periodically before counters overflow.
void microphone_pcm_stats_reset(microphone_pcm_stats_t *stats);

// Convert frames of native signed 16-bit [left, right] I2S samples to mono.
// Slots are already aligned: no shifting or gain is applied. AVERAGE uses a
// signed 32-bit sum and divides by two, truncating odd sums toward zero.
// stereo holds 2 * frames samples; mono holds frames. In-place downmixing to
// the start of stereo is supported. slot must be one of the enum values.
// When stats is non-NULL, accumulate both raw slots and the selected mono data;
// clipping means absolute magnitude >= 32760. Windows can span multiple calls.
void microphone_pcm_convert(const int16_t *stereo, int16_t *mono,
                            size_t frames, microphone_pcm_slot_t slot,
                            microphone_pcm_stats_t *stats);

// RMS in signed 16-bit sample units; empty and all-zero windows return zero.
double microphone_pcm_rms(const microphone_pcm_level_t *level);

// RMS dBFS relative to 32768, with a -96 dB floor for silence/empty windows.
double microphone_pcm_dbfs(const microphone_pcm_level_t *level);
