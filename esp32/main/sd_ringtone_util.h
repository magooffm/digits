#pragma once

// Pure helpers for SD card ringtones: file naming, ID3 tags, MP3 length and
// PCM conditioning. No ESP-IDF dependency; tests/sd_ringtone_util_test.c runs
// them on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SD_RINGTONE_SILENCE 64 // Leading samples at or below this are skipped (-54 dBFS).

// "ringtone-03.mp3" -> 2 (0-based selector position). Case-insensitive;
// one or two digits, 1..max_positions. -1 for any other name.
int sd_ringtone_position(const char *name, unsigned max_positions);

// Bytes to skip at the start of a file for an ID3v2 tag (header, body and
// optional footer), or 0 if header (length >= 10) holds none.
uint32_t sd_ringtone_id3_size(const uint8_t *header, size_t length);

// Length of an MP3 stream without decoding it. head holds the first bytes
// after any ID3v2 tag (a few KB); audio_bytes counts the stream from there to
// its end (without an ID3v1 tag). Uses the Xing/Info or VBRI frame count when
// present, else the constant bitrate. False if no MPEG Layer III frame found.
bool sd_ringtone_mp3_length(const uint8_t *head, size_t length, uint64_t audio_bytes,
                            uint32_t *sample_rate, uint64_t *samples);

// Averages interleaved channels into mono; returns frames written to out.
// out may equal in.
size_t sd_ringtone_downmix(const int16_t *in, size_t frames, unsigned channels, int16_t *out);

// Index of the first sample above SD_RINGTONE_SILENCE, or count if none.
size_t sd_ringtone_first_sound(const int16_t *pcm, size_t count);

// Multiplies by percent / 100 (1..100), rounding toward zero.
void sd_ringtone_scale(int16_t *pcm, size_t count, unsigned percent);
