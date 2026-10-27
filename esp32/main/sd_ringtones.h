#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "voicemail_prompt.h"

// Ringtones from the SD card. At boot the card is mounted and stays mounted;
// each /ringtones/ringtone-NN.mp3 becomes local ringtone NN (ringtones.h),
// with its length read from the MP3 headers. Playing streams the file: a
// decoder task fills a one-second buffer the ring players read without
// blocking. No card, no folder or a bad file only logs; built-in ringtones
// still work, and a file that fails while ringing falls back to the
// standard ring.
esp_err_t digits_sd_ringtones_start(void);

// "disabled", "starting", "no card", "no ringtones folder" or "ready".
const char *digits_sd_ringtones_status(void);

// Answering-machine greeting /voicemail/greeting.mp3, streamed the same way
// at CONFIG_DIGITS_VOICEMAIL_GREETING_LEVEL. Length in 48 kHz frames, or 0
// without a usable file (also after it failed once while playing).
uint32_t digits_sd_greeting_frames(void);
// vm_prompt_reader_fn for the encoder task; never blocks. Not while a
// ringtone streams: answering stops the ringing first.
vm_prompt_read_t digits_sd_greeting_read(uint32_t position, int16_t *mono, size_t frames);
