#pragma once

#include "esp_err.h"

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
