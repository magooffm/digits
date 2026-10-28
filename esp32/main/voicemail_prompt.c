#include "voicemail_prompt.h"
#include <math.h>
#include <string.h>

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool vm_prompt_wav(const uint8_t *wav, size_t size, const uint8_t **pcm, size_t *samples, uint32_t *rate)
{
    if (!wav || size < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;
    bool format_ok = false;
    uint32_t found_rate = 0;
    for (size_t at = 12; at + 8 <= size;) {
        uint32_t length = get32(wav + at + 4);
        const uint8_t *chunk = wav + at + 8;
        if (length > size - at - 8) return false;
        if (!memcmp(wav + at, "fmt ", 4)) {
            found_rate = length >= 16 ? get32(chunk + 4) : 0;
            format_ok = length >= 16 && get16(chunk) == 1 && get16(chunk + 2) == 1 && get16(chunk + 14) == 16 &&
                        (found_rate == 8000 || found_rate == 16000 || found_rate == 24000 || found_rate == VM_PROMPT_RATE);
        } else if (!memcmp(wav + at, "data", 4)) {
            if (!format_ok) return false;
            *pcm = chunk;
            *samples = length / 2;
            *rate = found_rate;
            return true;
        }
        at += 8 + length + (length & 1);
    }
    return false;
}

void vm_prompt_start(vm_prompt_t *prompt, const uint8_t *builtin, size_t samples, uint32_t rate,
                     vm_prompt_reader_fn custom)
{
    memset(prompt, 0, sizeof(*prompt));
    bool usable = builtin && samples && rate && VM_PROMPT_RATE % rate == 0;
    prompt->builtin = usable ? builtin : NULL;
    prompt->builtin_samples = usable ? samples : 0;
    prompt->builtin_step = usable ? VM_PROMPT_RATE / rate : 1;
    prompt->builtin_frames = prompt->builtin_samples * prompt->builtin_step;
    prompt->custom = custom;
    prompt->phase = custom ? VM_PROMPT_CUSTOM : prompt->builtin_frames ? VM_PROMPT_BUILTIN : VM_PROMPT_BEEP;
    for (unsigned i = 0; i < 48; ++i)
        prompt->beep[i] = (int16_t)lrint(VM_PROMPT_BEEP_PEAK * sin(2 * 3.14159265358979323846 * i / 48.0));
}

static int16_t sample_at(const vm_prompt_t *prompt, size_t index)
{
    const uint8_t *p = prompt->builtin + 2 * index;
    return (int16_t)(p[0] | p[1] << 8);
}

static void next(vm_prompt_t *prompt, vm_prompt_phase_t phase)
{
    prompt->phase = phase;
    prompt->position = 0;
}

static size_t beep(vm_prompt_t *prompt, int16_t *mono, size_t frames)
{
    size_t n = VM_PROMPT_BEEP_FRAMES - prompt->position;
    if (n > frames) n = frames;
    for (size_t i = 0; i < n; ++i) {
        uint32_t at = prompt->position + (uint32_t)i;
        uint32_t edge = at < VM_PROMPT_BEEP_FRAMES - 1 - at ? at : VM_PROMPT_BEEP_FRAMES - 1 - at;
        int32_t sample = prompt->beep[at % 48];
        if (edge < VM_PROMPT_RAMP_FRAMES) sample = sample * (int32_t)edge / VM_PROMPT_RAMP_FRAMES;
        mono[i] = (int16_t)sample;
    }
    prompt->position += (uint32_t)n;
    if (prompt->position >= VM_PROMPT_BEEP_FRAMES) next(prompt, VM_PROMPT_OPEN);
    return n;
}

bool vm_prompt_render(vm_prompt_t *prompt, int16_t *mono, size_t frames)
{
    size_t done = 0;
    while (done < frames) {
        int16_t *out = mono + done;
        size_t left = frames - done, n = left;
        switch (prompt->phase) {
        case VM_PROMPT_CUSTOM: {
            vm_prompt_read_t result = prompt->custom(prompt->position, out, left);
            if (result == VM_PROMPT_FAILED) {
                // Nothing played yet: the built-in greeting instead.
                next(prompt, prompt->position || !prompt->builtin_frames ? VM_PROMPT_PAUSE : VM_PROMPT_BUILTIN);
                continue;
            }
            prompt->position += (uint32_t)left;
            if (result == VM_PROMPT_DONE || prompt->position >= VM_PROMPT_GREETING_LIMIT)
                next(prompt, VM_PROMPT_PAUSE);
            break;
        }
        case VM_PROMPT_BUILTIN:
            if (n > prompt->builtin_frames - prompt->position) n = prompt->builtin_frames - prompt->position;
            if (prompt->builtin_step == 1) {
                memcpy(out, prompt->builtin + 2 * (size_t)prompt->position, n * sizeof(int16_t));
            } else {
                for (size_t i = 0; i < n; ++i) {
                    size_t at = prompt->position + i, index = at / prompt->builtin_step;
                    int32_t a = sample_at(prompt, index);
                    int32_t b = index + 1 < prompt->builtin_samples ? sample_at(prompt, index + 1) : a;
                    out[i] = (int16_t)(a + (b - a) * (int32_t)(at % prompt->builtin_step) / (int32_t)prompt->builtin_step);
                }
            }
            prompt->position += (uint32_t)n;
            if (prompt->position >= prompt->builtin_frames) next(prompt, VM_PROMPT_PAUSE);
            break;
        case VM_PROMPT_PAUSE:
            if (n > VM_PROMPT_PAUSE_FRAMES - prompt->position) n = VM_PROMPT_PAUSE_FRAMES - prompt->position;
            memset(out, 0, n * sizeof(int16_t));
            prompt->position += (uint32_t)n;
            if (prompt->position >= VM_PROMPT_PAUSE_FRAMES) next(prompt, VM_PROMPT_BEEP);
            break;
        case VM_PROMPT_BEEP:
            n = beep(prompt, out, left);
            break;
        case VM_PROMPT_OPEN:
            memset(out, 0, n * sizeof(int16_t));
            break;
        }
        done += n;
    }
    return prompt->phase == VM_PROMPT_OPEN;
}
