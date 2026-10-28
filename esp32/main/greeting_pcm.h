#pragma once

// Pure helpers for recording a greeting: 48 kHz capture to 16 kHz, level
// and WAV header. No ESP-IDF: tests/greeting_pcm_test.c runs on the host.

#include <stddef.h>
#include <stdint.h>

#define GREETING_PCM_RATE 16000
#define GREETING_PCM_HEADER_BYTES 44

// 48 kHz to 16 kHz: the mean of each three samples (a gentle low-pass
// first). frames must be a multiple of 3; returns frames / 3.
size_t greeting_pcm_downsample(const int16_t *in, size_t frames, int16_t *out);

// Removes the DC offset, then scales so the peak reaches target (at most
// max_gain times louder: a quiet room is not turned into hiss). Returns the
// applied gain in 1/16 steps (16 = unchanged).
unsigned greeting_pcm_normalize(int16_t *pcm, size_t count, int16_t target, unsigned max_gain);

// 44-byte header for mono 16-bit PCM.
void greeting_pcm_wav_header(uint8_t out[GREETING_PCM_HEADER_BYTES], uint32_t rate, uint32_t samples);
