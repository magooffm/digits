#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "tone_gen.h"

// Call-progress tone player on the board speaker. Boot-only init after the
// audio board; it renders with tone_gen and owns the speaker only while a
// tone is requested.
esp_err_t digits_call_tones_init(void);

// Nonblocking. Requesting the tone already playing keeps its cadence;
// a different tone restarts at its beginning. TONE_GEN_NONE stops.
void digits_call_tones_play(tone_gen_tone_t tone);

// Plays one cycle of a ringtones.h entry, then stops by itself. A different
// tone request (dial tone on lifting the handset) cancels it; repeating
// TONE_GEN_NONE does not. The standard ring previews as 2 s of its tone.
void digits_call_tones_preview(unsigned ringtone);

// Cancels any tone or preview.
void digits_call_tones_stop(void);

// True once no tone is requested and the worker has released the speaker
// (or never held it). Call audio must wait for this before claiming output.
// A tone that cannot get the speaker or fails to play also reports true.
bool digits_call_tones_is_stopped(void);
