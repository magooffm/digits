#pragma once

#include <stdbool.h>
#include <stddef.h>
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

// The greeting callers hear: one recorded on the phone (greeting.wav), else
// an uploaded MP3 (greeting.mp3), else the built-in one.
#define DIGITS_VOICEMAIL_GREETING_WAV "/sdcard/voicemail/greeting.wav"
const char *digits_voicemail_greeting_kind(void); // "recorded", "uploaded" or "built-in".
unsigned digits_voicemail_greeting_seconds(void);
// Re-reads greeting.wav after it changed. Only while the line is idle.
esp_err_t digits_voicemail_greeting_reload(void);
void digits_voicemail_stop(void);

// Like a classic answering machine, the speaker plays the greeting and then
// the caller while a message is recorded; both on by default. Saved in NVS
// (set from an internal-RAM task); used from the next call the machine takes.
void digits_voicemail_listen(bool *greeting, bool *caller);
esp_err_t digits_voicemail_set_listen(bool greeting, bool caller);

// Messages for the web page, oldest first. Store changes take a lock and
// touch only the SD card (no flash), so any task may call them.
typedef struct {
    uint32_t number;
    char caller[32];
    bool heard;
    int64_t recorded;   // Unix time from the file date; 0 when the clock was unset.
    uint32_t tenths;    // Length in tenths of a second.
} digits_voicemail_message_t;
// Fills up to max entries; returns how many messages exist.
unsigned digits_voicemail_list(digits_voicemail_message_t *out, unsigned max);
bool digits_voicemail_file(uint32_t number, char *path, size_t capacity);
esp_err_t digits_voicemail_mark_heard(uint32_t number);
esp_err_t digits_voicemail_delete(uint32_t number);

// Console: vm [status] | list | play [N] | greeting | stop | heard N |
// delete N | delete heard. Returns 0 on success, 1 on usage error.
int digits_voicemail_console(int argc, char **argv);
