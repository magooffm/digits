// Host tests for greeting_pcm.c, read back through voicemail_prompt.c.
#include "greeting_pcm.h"
#include "voicemail_prompt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int16_t in[6] = {300, 600, 900, -3, -3, -3}, out[2];
    assert(greeting_pcm_downsample(in, 6, out) == 2 && out[0] == 600 && out[1] == -3);

    int16_t quiet[4] = {1100, 900, 1100, 900};        // Offset 1000, peak 100.
    assert(greeting_pcm_normalize(quiet, 4, 1600, 64) == 256); // 16x.
    assert(quiet[0] == 1600 && quiet[1] == -1600);
    int16_t hiss[4] = {2, -2, 2, -2};
    assert(greeting_pcm_normalize(hiss, 4, 29000, 8) == 128 && hiss[0] == 16); // Capped at 8x.
    int16_t loud[2] = {32767, -32768};
    greeting_pcm_normalize(loud, 2, 29000, 8);
    assert(loud[0] <= 29001 && loud[1] >= -29001);     // Turned down to the target.
    int16_t silence[3] = {0, 0, 0};
    greeting_pcm_normalize(silence, 3, 29000, 8);
    assert(!silence[0] && !silence[2]);

    uint8_t wav[GREETING_PCM_HEADER_BYTES + 6];
    greeting_pcm_wav_header(wav, GREETING_PCM_RATE, 3);
    int16_t samples[3] = {0, 300, 600};
    memcpy(wav + GREETING_PCM_HEADER_BYTES, samples, sizeof(samples));
    const uint8_t *pcm;
    size_t count;
    uint32_t rate;
    assert(vm_prompt_wav(wav, sizeof(wav), &pcm, &count, &rate) && rate == 16000 && count == 3);
    assert(pcm == wav + GREETING_PCM_HEADER_BYTES);
    puts("greeting_pcm tests passed");
    return 0;
}
