#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Records a new answering-machine greeting with the phone's microphone (the
// board microphone today, the handset's once it is wired): a beep, then up to
// CONFIG_DIGITS_GREETING_RECORD_SECONDS until stopped. The result waits in
// voicemail/greeting-new.wav until kept (it becomes the greeting callers
// hear) or discarded. An incoming call cancels a recording.

#define DIGITS_GREETING_NEW_PATH "/sdcard/voicemail/greeting-new.wav"

typedef enum {
    DIGITS_GREETING_RECORD_IDLE,      // Nothing pending.
    DIGITS_GREETING_RECORD_STARTING,  // Beep.
    DIGITS_GREETING_RECORD_RECORDING,
    DIGITS_GREETING_RECORD_READY,     // greeting-new.wav waits for keep or discard.
    DIGITS_GREETING_RECORD_FAILED,    // message says why; start again or discard.
} digits_greeting_record_state_t;

typedef struct {
    digits_greeting_record_state_t state;
    uint32_t tenths;     // Recorded so far, or the result's length.
    const char *message; // For FAILED.
} digits_greeting_record_status_t;

// Only while the line is idle and the answering machine has its card.
esp_err_t digits_greeting_record_start(void);
void digits_greeting_record_stop(void);
digits_greeting_record_status_t digits_greeting_record_status(void);
// The pending recording becomes the greeting (replacing an uploaded MP3).
esp_err_t digits_greeting_record_keep(void);
void digits_greeting_record_discard(void);
