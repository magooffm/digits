#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "voicemail_prompt.h"

// Ringtones from the SD card. At boot the card is mounted and stays mounted;
// each MP3 in /ringtones becomes a library song (ringtones.h), with its
// length read from the MP3 headers. Playing streams the file: a
// decoder task fills a one-second buffer the ring players read without
// blocking. No card, no folder or a bad file only logs; built-in ringtones
// still work, and a file that fails while ringing falls back to the
// standard ring.
esp_err_t digits_sd_ringtones_start(void);

// "disabled", "starting", "no card", "no ringtones folder" or "ready".
const char *digits_sd_ringtones_status(void);

// The library: every MP3 in /ringtones (alphabetical, up to
// RINGTONE_LOCAL_SLOTS), as ringtones RINGTONE_FIRST_LOCAL and up under
// their file names. Numbers stay valid until the next restart; phone.c
// saves selections by name. Changes only while nothing rings or previews:
// the caller checks the line is idle.
#define DIGITS_SD_RINGTONE_FOLDER "/sdcard/ringtones"
int digits_sd_ringtone_find(const char *name); // Ringtone number, or -1.
bool digits_sd_ringtone_file(unsigned id, char *out, size_t capacity);
// After writing DIGITS_SD_RINGTONE_FOLDER/name: adds it, or replaces the song
// of the same name. ESP_ERR_INVALID_RESPONSE: not a playable MP3;
// ESP_ERR_NO_MEM: the library is full.
esp_err_t digits_sd_ringtone_added(const char *name, unsigned *id);
// Deletes the file and withdraws the song.
esp_err_t digits_sd_ringtone_delete(unsigned id);
// Re-reads /voicemail/greeting.mp3 (DIGITS_SD_GREETING_PATH) after it changed.
#define DIGITS_SD_GREETING_PATH "/sdcard/voicemail/greeting.mp3"
esp_err_t digits_sd_greeting_reload(void);

// Answering-machine greeting /voicemail/greeting.mp3, streamed the same way
// at CONFIG_DIGITS_VOICEMAIL_GREETING_LEVEL. Length in 48 kHz frames, or 0
// without a usable file (also after it failed once while playing).
uint32_t digits_sd_greeting_frames(void);
// vm_prompt_reader_fn for the encoder task; never blocks. Not while a
// ringtone streams: answering stops the ringing first.
vm_prompt_read_t digits_sd_greeting_read(uint32_t position, int16_t *mono, size_t frames);
