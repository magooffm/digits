#include "ringtones.h"
#include "ringtone_songs.h"
#include <math.h>
#include <string.h>

// Music-box timbre: fundamental plus two quieter harmonics, a 2 ms attack,
// exponential decay and an 8 ms release so repeated notes never click.
#define SINE_BITS 10
#define SINE_SIZE (1U << SINE_BITS)
#define ATTACK_FRAMES 96U
#define RELEASE_FRAMES 384U
#define DECAY_STEP_FRAMES 384U // 8 ms per table entry.
#define DECAY_STEPS 256U
#define DECAY_TAU_SECONDS 0.4
#define PEAK 4096
#define HARMONIC_SUM_X20 31 // 20 * (1 + 0.4 + 0.15): the waveform's largest magnitude factor.

typedef struct {
    const char *name;
    unsigned bpm;
    const char *notes;
} song_def_t;

#define DEFINE_SONG(name, bpm, notes) {name, bpm, notes},
static const song_def_t defs[] = {RINGTONE_SONGS(DEFINE_SONG)};
#define SONG_COUNT (sizeof(defs) / sizeof(defs[0]))

typedef struct {
    uint32_t start, length;
    uint32_t phase_step; // 0 for a rest.
} note_t;

typedef struct {
    note_t notes[RINGTONE_MAX_NOTES];
    unsigned count;
    uint32_t frames;
} song_t;

_Static_assert(SONG_COUNT <= RINGTONE_FIRST_CLIP, "built-in ringtones must stay below 10");

static song_t songs[SONG_COUNT];
static ringtones_clip_t clips[RINGTONE_MAX_CLIPS];
static unsigned clip_count;
static uint32_t clip_rate;
static ringtones_clip_read_fn clip_read;

// Local ringtones: frames is published last (release) and read with acquire,
// so a renderer never sees a half-registered slot.
typedef struct {
    ringtones_stream_fn stream;
    uint32_t frames;
    char name[RINGTONE_NAME_BYTES];
} local_t;
static local_t locals[RINGTONE_LOCAL_SLOTS];

static const local_t *local_get(unsigned id)
{
    if (id < RINGTONE_FIRST_LOCAL || id >= RINGTONE_FIRST_LOCAL + RINGTONE_LOCAL_SLOTS) return NULL;
    const local_t *local = &locals[id - RINGTONE_FIRST_LOCAL];
    return __atomic_load_n(&local->frames, __ATOMIC_ACQUIRE) ? local : NULL;
}

static int clip_index(unsigned id)
{
    for (unsigned i = 0; i < clip_count; ++i)
        if (clips[i].id == id) return (int)i;
    return -1;
}
static int16_t sine[SINE_SIZE];
static uint16_t decay[DECAY_STEPS + 1]; // Q15.

static bool parse_pitch(const char *text, size_t length, double *hz)
{
    static const int semitone[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G from C.
    if (length < 2 || text[0] < 'A' || text[0] > 'G') return false;
    int note = semitone[text[0] - 'A'];
    size_t i = 1;
    if (text[i] == '#') { ++note; ++i; }
    else if (text[i] == 'b') { --note; ++i; }
    if (i != length - 1 || text[i] < '0' || text[i] > '8') return false;
    int midi = 12 * (text[i] - '0' + 1) + note;
    *hz = 440.0 * pow(2.0, (midi - 69) / 12.0);
    return true;
}

// Token: NOTE/LEN or NOTE/LEN. (dotted), NOTE = pitch or R.
static bool parse_note(const char *token, size_t length, unsigned bpm, note_t *note)
{
    const char *slash = memchr(token, '/', length);
    if (!slash || slash == token) return false;
    size_t pitch_length = (size_t)(slash - token);
    const char *value = slash + 1, *end = token + length;
    bool dotted = end > value && end[-1] == '.';
    if (dotted) --end;
    unsigned denominator = 0;
    for (const char *c = value; c < end; ++c) {
        if (*c < '0' || *c > '9') return false;
        denominator = denominator * 10 + (unsigned)(*c - '0');
    }
    if (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 &&
        denominator != 16)
        return false;
    double seconds = 60.0 / bpm * 4.0 / denominator * (dotted ? 1.5 : 1.0);
    note->length = (uint32_t)lround(seconds * RINGTONE_SAMPLE_RATE);
    if (pitch_length == 1 && token[0] == 'R') {
        note->phase_step = 0;
        return true;
    }
    double hz;
    if (!parse_pitch(token, pitch_length, &hz) || hz >= RINGTONE_SAMPLE_RATE / 6.0) return false;
    note->phase_step = (uint32_t)lround(hz * 4294967296.0 / RINGTONE_SAMPLE_RATE);
    return true;
}

static bool parse_song(const song_def_t *def, song_t *song)
{
    memset(song, 0, sizeof(*song));
    if (!def->notes[0]) return def->bpm == 0; // Standard ring: rendered elsewhere.
    if (def->bpm < 30 || def->bpm > 300) return false;
    const char *p = def->notes;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') ++p;
        if (song->count == RINGTONE_MAX_NOTES) return false;
        note_t *note = &song->notes[song->count];
        if (!parse_note(start, (size_t)(p - start), def->bpm, note)) return false;
        note->start = song->frames;
        song->frames += note->length;
        ++song->count;
    }
    return song->count > 0;
}

bool ringtones_init(void)
{
    // Boot-time only and idempotent: later callers reuse the built tables.
    static bool built, valid;
    if (built) return valid;
    for (unsigned i = 0; i < SINE_SIZE; ++i)
        sine[i] = (int16_t)lround(32767.0 * sin(6.283185307179586 * i / SINE_SIZE));
    for (unsigned i = 0; i <= DECAY_STEPS; ++i)
        decay[i] = (uint16_t)lround(32767.0 * exp(-(double)(i * DECAY_STEP_FRAMES) /
                                                    (DECAY_TAU_SECONDS * RINGTONE_SAMPLE_RATE)));
    bool ok = true;
    for (unsigned i = 0; i < SONG_COUNT; ++i) ok &= parse_song(&defs[i], &songs[i]);
    built = true;
    valid = ok;
    return ok;
}

unsigned ringtones_set_clips(const ringtones_clip_t *source, unsigned count, uint32_t sample_rate,
                             ringtones_clip_read_fn read)
{
    clip_count = 0;
    if (!source || !read || !sample_rate || sample_rate > RINGTONE_SAMPLE_RATE) return 0;
    clip_rate = sample_rate;
    clip_read = read;
    for (unsigned i = 0; i < count && clip_count < RINGTONE_MAX_CLIPS; ++i) {
        ringtones_clip_t clip = source[i];
        if (clip.id < RINGTONE_FIRST_CLIP || clip.id > RINGTONE_LAST_CLIP || !clip.samples ||
            clip_index(clip.id) >= 0)
            continue;
        clip.name[RINGTONE_NAME_BYTES - 1] = '\0';
        clips[clip_count++] = clip;
    }
    // Keep ids ascending; insertion sort over at most 32 entries.
    for (unsigned i = 1; i < clip_count; ++i)
        for (unsigned j = i; j > 0 && clips[j - 1].id > clips[j].id; --j) {
            ringtones_clip_t swap = clips[j];
            clips[j] = clips[j - 1];
            clips[j - 1] = swap;
        }
    return clip_count;
}

bool ringtones_set_local(unsigned slot, uint32_t frames, const char *name, ringtones_stream_fn stream)
{
    if (slot >= RINGTONE_LOCAL_SLOTS || !frames || !stream) return false;
    local_t *local = &locals[slot];
    if (__atomic_load_n(&local->frames, __ATOMIC_ACQUIRE) || (local->stream && local->stream != stream)) return false;
    local->stream = stream;
    strncpy(local->name, name ? name : "", RINGTONE_NAME_BYTES - 1);
    local->name[RINGTONE_NAME_BYTES - 1] = '\0';
    __atomic_store_n(&local->frames, frames, __ATOMIC_RELEASE);
    return true;
}

void ringtones_clear_local(unsigned slot)
{
    // stream stays set, so the slot is never reused under a reader.
    if (slot < RINGTONE_LOCAL_SLOTS) __atomic_store_n(&locals[slot].frames, 0, __ATOMIC_RELEASE);
}

unsigned ringtones_count(void) { return SONG_COUNT; }

unsigned ringtones_list(unsigned *ids, unsigned max)
{
    unsigned total = 0;
    for (unsigned id = 0; id < SONG_COUNT; ++id, ++total)
        if (ids && total < max) ids[total] = id;
    for (unsigned i = 0; i < clip_count; ++i, ++total)
        if (ids && total < max) ids[total] = clips[i].id;
    return total;
}

bool ringtones_sendable(unsigned id) { return id < SONG_COUNT || clip_index(id) >= 0; }

bool ringtones_valid(unsigned id) { return ringtones_sendable(id) || local_get(id); }

const char *ringtones_name(unsigned id)
{
    if (id < SONG_COUNT) return defs[id].name;
    int index = clip_index(id);
    if (index >= 0) return clips[index].name;
    const local_t *local = local_get(id);
    return local ? local->name : NULL;
}

uint32_t ringtones_melody_frames(unsigned id)
{
    if (id < SONG_COUNT) return songs[id].frames;
    int index = clip_index(id);
    if (index >= 0) return (uint32_t)((uint64_t)clips[index].samples * RINGTONE_SAMPLE_RATE / clip_rate);
    const local_t *local = local_get(id);
    return local ? local->frames : 0;
}

// Clips: linear interpolation from the clip rate up to 48 kHz. The frame at
// output position p sits at clip sample p * rate / 48000 (16.16 fixed point).
static void render_clip(int index, uint32_t position, int16_t *stereo, size_t frames)
{
    enum { CHUNK = 128 };
    const ringtones_clip_t *clip = &clips[index];
    uint64_t step = ((uint64_t)clip_rate << 16) / RINGTONE_SAMPLE_RATE;
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done < CHUNK ? frames - done : CHUNK;
        uint64_t start = (uint64_t)(position + done) * step;
        uint32_t first = (uint32_t)(start >> 16);
        uint32_t last = (uint32_t)(((uint64_t)(position + done + n - 1) * step) >> 16) + 1;
        int16_t source[CHUNK + 2] = {0}; // 128 frames span at most 129 clip samples.
        if (first < clip->samples) {
            uint32_t end = last < clip->samples ? last + 1 : clip->samples;
            if (!clip_read(clip->source, first, source, end - first))
                memset(source, 0, sizeof(source));
        }
        for (size_t i = 0; i < n; ++i) {
            uint64_t at = (uint64_t)(position + done + i) * step;
            uint32_t sample = (uint32_t)(at >> 16), fraction = (uint32_t)(at & 0xffff);
            int32_t a = sample < clip->samples ? source[sample - first] : 0;
            int32_t b = sample + 1 < clip->samples ? source[sample + 1 - first] : 0;
            int16_t value = (int16_t)(a + (((b - a) * (int32_t)fraction) >> 16));
            stereo[2 * (done + i)] = stereo[2 * (done + i) + 1] = value;
        }
        done += n;
    }
}

// Local ringtones arrive at 48 kHz; only the fades and the end are added here.
static void render_local(unsigned id, const local_t *local, uint32_t position, int16_t *stereo,
                         size_t frames)
{
    enum { CHUNK = 128 };
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done < CHUNK ? frames - done : CHUNK;
        uint32_t at = position + (uint32_t)done;
        int16_t mono[CHUNK] = {0};
        size_t audible = at >= local->frames ? 0 : local->frames - at < n ? local->frames - at : n;
        if (audible) local->stream(id - RINGTONE_FIRST_LOCAL, at, mono, audible);
        for (size_t i = 0; i < n; ++i) {
            uint32_t frame = at + (uint32_t)i;
            int32_t value = mono[i];
            uint32_t left = frame < local->frames ? local->frames - 1 - frame : 0;
            if (frame < RINGTONE_LOCAL_FADE_FRAMES)
                value = value * (int32_t)frame / RINGTONE_LOCAL_FADE_FRAMES;
            if (left < RINGTONE_LOCAL_FADE_FRAMES)
                value = value * (int32_t)left / RINGTONE_LOCAL_FADE_FRAMES;
            stereo[2 * (done + i)] = stereo[2 * (done + i) + 1] = (int16_t)value;
        }
        done += n;
    }
}

static int32_t envelope(uint32_t offset, uint32_t length)
{
    uint32_t step = offset / DECAY_STEP_FRAMES;
    int32_t level;
    if (step >= DECAY_STEPS) {
        level = decay[DECAY_STEPS];
    } else {
        int32_t a = decay[step], b = decay[step + 1];
        level = a + (b - a) * (int32_t)(offset % DECAY_STEP_FRAMES) / (int32_t)DECAY_STEP_FRAMES;
    }
    if (offset < ATTACK_FRAMES) level = level * (int32_t)offset / (int32_t)ATTACK_FRAMES;
    uint32_t left = length - offset;
    if (left < RELEASE_FRAMES) level = level * (int32_t)left / (int32_t)RELEASE_FRAMES;
    return level;
}

void ringtones_render(unsigned id, uint32_t position, int16_t *stereo, size_t frames)
{
    if (id >= SONG_COUNT) {
        int index = clip_index(id);
        const local_t *local = local_get(id);
        if (index >= 0) {
            render_clip(index, position, stereo, frames);
        } else if (local) {
            render_local(id, local, position, stereo, frames);
        } else {
            memset(stereo, 0, frames * 2 * sizeof(*stereo));
        }
        return;
    }
    const song_t *song = id < SONG_COUNT ? &songs[id] : NULL;
    unsigned index = 0;
    // Binary search for the first note; later frames advance linearly.
    if (song && song->count && position < song->frames) {
        unsigned low = 0, high = song->count - 1;
        while (low < high) {
            unsigned mid = (low + high + 1) / 2;
            if (song->notes[mid].start <= position) low = mid;
            else high = mid - 1;
        }
        index = low;
    }
    for (size_t i = 0; i < frames; ++i) {
        uint32_t n = position + (uint32_t)i;
        int16_t value = 0;
        if (song && n < song->frames) {
            while (n >= song->notes[index].start + song->notes[index].length) ++index;
            const note_t *note = &song->notes[index];
            if (note->phase_step) {
                uint32_t offset = n - note->start;
                uint32_t phase = note->phase_step * offset; // Wraps modulo 2^32 by design.
                int32_t wave = sine[phase >> (32 - SINE_BITS)] * 20 +
                               sine[(2 * phase) >> (32 - SINE_BITS)] * 8 +
                               sine[(3 * phase) >> (32 - SINE_BITS)] * 3;
                int64_t scaled = (int64_t)wave * envelope(offset, note->length) * PEAK;
                value = (int16_t)(scaled / ((int64_t)32767 * HARMONIC_SUM_X20 * 32767));
            }
        }
        stereo[2 * i] = stereo[2 * i + 1] = value;
    }
}
