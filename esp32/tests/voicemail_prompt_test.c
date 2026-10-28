// Host tests for voicemail_prompt.c, including the Pi's default greeting.
#include "voicemail_prompt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOTAL (3 * VM_PROMPT_RATE)
static int16_t out[TOTAL];

static size_t put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i); return 4; }
static size_t put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return 2; }

// RIFF/WAVE with an odd-sized chunk before fmt, as some editors write.
static size_t make_wav(uint8_t *w, uint32_t rate, uint16_t channels, size_t frames, int16_t value)
{
    size_t at = 0;
    memcpy(w, "RIFF", 4); at += 4;
    at += put32(w + at, 0);
    memcpy(w + at, "WAVELIST", 8); at += 8;
    at += put32(w + at, 3);
    memcpy(w + at, "abc\0", 4); at += 4; // Padding byte.
    memcpy(w + at, "fmt ", 4); at += 4;
    at += put32(w + at, 16);
    at += put16(w + at, 1);
    at += put16(w + at, channels);
    at += put32(w + at, rate);
    at += put32(w + at, rate * 2 * channels);
    at += put16(w + at, (uint16_t)(2 * channels));
    at += put16(w + at, 16);
    memcpy(w + at, "data", 4); at += 4;
    at += put32(w + at, (uint32_t)(frames * 2));
    for (size_t i = 0; i < frames; ++i) at += put16(w + at, (uint16_t)value);
    return at;
}

static void render_in_blocks(vm_prompt_t *prompt, size_t total, size_t block, size_t *open_at)
{
    *open_at = 0;
    for (size_t done = 0; done < total; done += block) {
        size_t n = total - done < block ? total - done : block;
        bool open = vm_prompt_render(prompt, out + done, n);
        if (open && !*open_at) *open_at = done + n;
    }
}

static void check_beep_after(size_t greeting, size_t open_at, size_t block)
{
    size_t beep = greeting + VM_PROMPT_PAUSE_FRAMES, end = beep + VM_PROMPT_BEEP_FRAMES;
    for (size_t i = greeting; i < beep; ++i) assert(out[i] == 0);
    int peak = 0;
    for (size_t i = beep; i < end; ++i) peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    assert(peak > VM_PROMPT_BEEP_PEAK - 10 && peak <= VM_PROMPT_BEEP_PEAK);
    assert(abs(out[beep]) < 50 && abs(out[end - 1]) < 50); // Faded edges.
    assert(out[beep + 48 * 21 + 12] > 8000);                // Quarter period peak.
    for (size_t i = end; i < TOTAL; ++i) assert(out[i] == 0);
    // Open is reported in the block where the beep ends.
    assert(open_at >= end && open_at < end + block);
}

static void test_wav(void)
{
    static uint8_t wav[4096];
    const uint8_t *pcm;
    size_t frames;
    uint32_t rate;
    size_t size = make_wav(wav, VM_PROMPT_RATE, 1, 100, 1234);
    assert(vm_prompt_wav(wav, size, &pcm, &frames, &rate) && frames == 100 && rate == VM_PROMPT_RATE);
    assert(pcm[0] == (1234 & 0xff) && pcm[1] == 1234 >> 8);
    assert(!vm_prompt_wav(wav, size - 50, &pcm, &frames, &rate)); // Truncated data chunk.
    size = make_wav(wav, 44100, 1, 10, 0);
    assert(!vm_prompt_wav(wav, size, &pcm, &frames, &rate));
    size = make_wav(wav, VM_PROMPT_RATE, 2, 10, 0);
    assert(!vm_prompt_wav(wav, size, &pcm, &frames, &rate));
    assert(!vm_prompt_wav(wav, 8, &pcm, &frames, &rate));
    size = make_wav(wav, 16000, 1, 10, 0); // A recorded greeting.
    assert(vm_prompt_wav(wav, size, &pcm, &frames, &rate) && rate == 16000 && frames == 10);
}

static uint8_t *load_pi_greeting(size_t *size)
{
    const char *file = __FILE__;
    const char *slash = strrchr(file, '/');
    char path[512];
    snprintf(path, sizeof(path), "%.*s/../../pi/tones/voicemail_greeting.wav", (int)(slash - file), file);
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    rewind(f);
    uint8_t *data = malloc(*size);
    assert(data && fread(data, 1, *size, f) == *size);
    fclose(f);
    return data;
}

static void test_builtin(void)
{
    size_t size;
    uint8_t *wav = load_pi_greeting(&size);
    const uint8_t *pcm;
    size_t frames;
    uint32_t rate;
    assert(vm_prompt_wav(wav, size, &pcm, &frames, &rate) && rate == VM_PROMPT_RATE);
    assert(frames > VM_PROMPT_RATE && frames < 3 * VM_PROMPT_RATE); // About 2.2 s.
    // Shorten it so the whole sequence fits the output buffer.
    size_t greeting = 30000;
    vm_prompt_t prompt;
    vm_prompt_start(&prompt, pcm + 1, greeting, VM_PROMPT_RATE, NULL); // Odd address on purpose.
    size_t open_at;
    render_in_blocks(&prompt, TOTAL, 960, &open_at);
    assert(!memcmp(out, pcm + 1, greeting * 2));
    check_beep_after(greeting, open_at, 960);
    free(wav);
}

static unsigned custom_frames;
static int custom_fail_at = -1;
static vm_prompt_read_t custom(uint32_t position, int16_t *mono, size_t frames)
{
    if ((int)position == custom_fail_at) return VM_PROMPT_FAILED;
    for (size_t i = 0; i < frames; ++i) mono[i] = position + i < custom_frames ? 777 : 0;
    return position + frames >= custom_frames ? VM_PROMPT_DONE : VM_PROMPT_MORE;
}

static void test_custom(void)
{
    static const uint8_t builtin[8] = {1, 0, 2, 0, 3, 0, 4, 0};
    vm_prompt_t prompt;
    size_t open_at;
    // Ends inside a block: the rest of that block is silence, then the pause.
    custom_frames = 10000;
    vm_prompt_start(&prompt, builtin, 4, VM_PROMPT_RATE, custom);
    render_in_blocks(&prompt, TOTAL, 960, &open_at);
    assert(out[0] == 777 && out[9999] == 777 && out[10000] == 0);
    check_beep_after(10560, open_at, 960); // 11 blocks of greeting.
    // Fails at once: the built-in greeting plays.
    custom_fail_at = 0;
    vm_prompt_start(&prompt, builtin, 4, VM_PROMPT_RATE, custom);
    render_in_blocks(&prompt, TOTAL, 7, &open_at);
    assert(out[0] == 1 && out[3] == 4);
    check_beep_after(4, open_at, 7);
    // Fails later: straight to the beep.
    custom_fail_at = 1920;
    custom_frames = 100000;
    vm_prompt_start(&prompt, builtin, 4, VM_PROMPT_RATE, custom);
    render_in_blocks(&prompt, TOTAL, 960, &open_at);
    assert(out[1919] == 777);
    check_beep_after(1920, open_at, 960);
    custom_fail_at = -1;
    // Nothing at all: beep first.
    vm_prompt_start(&prompt, NULL, 0, VM_PROMPT_RATE, NULL);
    assert(prompt.phase == VM_PROMPT_BEEP);
    assert(!vm_prompt_render(&prompt, out, VM_PROMPT_BEEP_FRAMES - 1));
    assert(vm_prompt_render(&prompt, out, 1));
    assert(vm_prompt_render(&prompt, out, 960) && out[959] == 0);
}

static void test_upsampling(void)
{
    // 16 kHz: three output frames per sample, interpolated, the last held.
    static const uint8_t pcm[] = {0, 0, 0x2c, 0x01, 0x58, 0x02}; // 0, 300, 600.
    vm_prompt_t prompt;
    vm_prompt_start(&prompt, pcm, 3, 16000, NULL);
    assert(prompt.builtin_frames == 9);
    int16_t out[9];
    vm_prompt_render(&prompt, out, 4);
    vm_prompt_render(&prompt, out + 4, 5); // Across a block boundary.
    static const int16_t expected[9] = {0, 100, 200, 300, 400, 500, 600, 600, 600};
    assert(!memcmp(out, expected, sizeof(out)) && prompt.phase == VM_PROMPT_PAUSE);
    vm_prompt_start(&prompt, pcm, 3, 44100, NULL); // Not a divisor of 48 kHz: no greeting.
    assert(prompt.phase == VM_PROMPT_BEEP);
}

int main(void)
{
    test_upsampling();
    test_wav();
    test_builtin();
    test_custom();
    puts("voicemail_prompt tests passed");
    return 0;
}
