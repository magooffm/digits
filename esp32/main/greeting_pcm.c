#include "greeting_pcm.h"
#include <string.h>

size_t greeting_pcm_downsample(const int16_t *in, size_t frames, int16_t *out)
{
    size_t n = frames / 3;
    for (size_t i = 0; i < n; ++i) out[i] = (int16_t)(((int32_t)in[3 * i] + in[3 * i + 1] + in[3 * i + 2]) / 3);
    return n;
}

unsigned greeting_pcm_normalize(int16_t *pcm, size_t count, int16_t target, unsigned max_gain)
{
    if (!count) return 16;
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum += pcm[i];
    int32_t offset = (int32_t)(sum / (int64_t)count), peak = 1;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = pcm[i] - offset;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    uint32_t gain = (uint32_t)target * 16 / (uint32_t)peak; // In 1/16 steps.
    if (gain > max_gain * 16) gain = max_gain * 16;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = (pcm[i] - offset) * (int32_t)gain / 16;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return gain;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

void greeting_pcm_wav_header(uint8_t out[GREETING_PCM_HEADER_BYTES], uint32_t rate, uint32_t samples)
{
    uint32_t data = samples * 2;
    memcpy(out, "RIFF", 4);
    put32(out + 4, 36 + data);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    put16(out + 20, 1);        // PCM.
    put16(out + 22, 1);        // Mono.
    put32(out + 24, rate);
    put32(out + 28, rate * 2); // Bytes per second.
    put16(out + 32, 2);        // Block align.
    put16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put32(out + 40, data);
}
