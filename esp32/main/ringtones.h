#pragma once

// Incoming-call ringtones chosen by the caller. Pure rendering after
// ringtones_init(); no ESP-IDF dependency. Numbers 0-9 are built in: 0 is the
// standard ring (rendered by ring_tone.c), then the melodies. Numbers 10-99
// are user clips packed into the firmware (ringtone_pack.c registers them).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINGTONE_STANDARD 0
#define RINGTONE_MAX_NOTES 64
#define RINGTONE_SAMPLE_RATE 48000
#define RINGTONE_PAUSE_FRAMES (2 * RINGTONE_SAMPLE_RATE) // Silence between repeats.
#define RINGTONE_FIRST_CLIP 10
#define RINGTONE_LAST_CLIP 99
#define RINGTONE_MAX_CLIPS 32
#define RINGTONE_NAME_BYTES 52

typedef struct {
    uint16_t id;          // RINGTONE_FIRST_CLIP..RINGTONE_LAST_CLIP
    uint32_t samples;     // At the registered clip sample rate.
    uint32_t source;      // Opaque to this module; passed back to the reader.
    char name[RINGTONE_NAME_BYTES];
} ringtones_clip_t;

// Reads count PCM16 samples of the clip registered with `source`, starting
// at sample `first`. Must not block for long; called by players.
typedef bool (*ringtones_clip_read_fn)(uint32_t source, uint32_t first, int16_t *out, size_t count);

// Boot-time, before players start. Clips are copied; sample_rate <= 48000.
// Entries with invalid or duplicate ids are skipped. Returns the number kept.
unsigned ringtones_set_clips(const ringtones_clip_t *clips, unsigned count, uint32_t sample_rate,
                             ringtones_clip_read_fn read);

// Parses the song table and builds the shared waveform tables. Returns false
// if a song is malformed (host tests check every entry).
bool ringtones_init(void);

// Number of built-in entries (standard ring plus melodies), all below 10.
unsigned ringtones_count(void);
// Fills up to max available ids (built-in first, then clips, ascending);
// returns how many exist.
unsigned ringtones_list(unsigned *ids, unsigned max);
const char *ringtones_name(unsigned id); // NULL when unavailable.
bool ringtones_valid(unsigned id);

// Melody or clip length in 48 kHz frames; one ring cycle is that plus
// RINGTONE_PAUSE_FRAMES. 0 for the standard ring.
uint32_t ringtones_melody_frames(unsigned id);

// Signed 16-bit stereo, identical channels, peak at most 4096 (the standard
// ring's level). position counts frames from the melody's first note; frames
// past the melody render silence. Output depends only on position.
void ringtones_render(unsigned id, uint32_t position, int16_t *stereo, size_t frames);
