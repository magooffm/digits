#pragma once

// What the answering machine sends before recording: the greeting, a short
// pause and a 500 ms, 1 kHz beep (as on the Pi), then silence. Pure C, no
// ESP-IDF: tests/voicemail_prompt_test.c runs on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VM_PROMPT_RATE 48000
#define VM_PROMPT_PAUSE_FRAMES (VM_PROMPT_RATE / 5)
#define VM_PROMPT_BEEP_FRAMES (VM_PROMPT_RATE / 2)
#define VM_PROMPT_BEEP_PEAK 8192 // About -12 dBFS.
#define VM_PROMPT_RAMP_FRAMES 240 // 5 ms fades against clicks.
#define VM_PROMPT_GREETING_LIMIT (120 * VM_PROMPT_RATE) // A stuck reader ends here.

typedef enum { VM_PROMPT_MORE, VM_PROMPT_DONE, VM_PROMPT_FAILED } vm_prompt_read_t;

// Custom greeting: fills all frames (zero padded) at position. DONE means
// these frames hold the end; FAILED means nothing usable was written.
typedef vm_prompt_read_t (*vm_prompt_reader_fn)(uint32_t position, int16_t *mono, size_t frames);

typedef enum {
    VM_PROMPT_CUSTOM, VM_PROMPT_BUILTIN, VM_PROMPT_PAUSE, VM_PROMPT_BEEP, VM_PROMPT_OPEN,
} vm_prompt_phase_t;

typedef struct {
    const uint8_t *builtin; // 16-bit little-endian mono PCM, any alignment.
    size_t builtin_frames;
    vm_prompt_reader_fn custom;
    vm_prompt_phase_t phase;
    uint32_t position; // Within the phase.
    int16_t beep[48];  // One 1 kHz period.
} vm_prompt_t;

// custom may be NULL. Without either greeting the beep comes first.
void vm_prompt_start(vm_prompt_t *prompt, const uint8_t *builtin, size_t builtin_frames,
                     vm_prompt_reader_fn custom);

// Fills frames of 48 kHz mono. True once the beep has ended: recording may start.
bool vm_prompt_render(vm_prompt_t *prompt, int16_t *mono, size_t frames);

// Finds 48 kHz mono 16-bit PCM in a WAV file.
bool vm_prompt_wav(const uint8_t *wav, size_t size, const uint8_t **pcm, size_t *frames);
