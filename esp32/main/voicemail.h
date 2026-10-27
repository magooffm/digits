#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "call_audio.h"
#include "esp_err.h"

// Answering machine on the SD card (voicemail_store.h for the layout).
// When the server's voicemail setting is on, an unanswered call is picked
// up after its ring timeout: the caller hears the greeting and a beep and
// their audio is stored as an Ogg Opus file. The room microphone is never
// sent and nothing is played while the machine answers. Lifting the
// handset keeps what was recorded and continues as a normal call.

// Boot, after the SD card was mounted (digits_sd_ringtones_start).
esp_err_t digits_voicemail_start(void);
// The card holds the message store and the recorder runs.
bool digits_voicemail_ready(void);
// Unheard messages, or -1 without a message store.
int digits_voicemail_unheard(void);

// Peer task only. begin returns the hooks for the call audio, or NULL if
// no recording can be made; end keeps what was recorded. full: the
// message reached CONFIG_DIGITS_VOICEMAIL_MAX_SECONDS, hang up.
const digits_call_audio_unattended_t *digits_voicemail_begin(const char *caller);
void digits_voicemail_end(void);
bool digits_voicemail_full(void);

// Plays on the speaker while the line is idle; an incoming call stops it.
// number 0 plays every unheard message, oldest first. A message heard to
// the end is marked heard.
esp_err_t digits_voicemail_play(uint32_t number);
esp_err_t digits_voicemail_play_greeting(void);
void digits_voicemail_stop(void);

// Console: vm [status] | list | play [N] | greeting | stop | heard N |
// delete N | delete heard. Returns 0 on success, 1 on usage error.
int digits_voicemail_console(int argc, char **argv);
