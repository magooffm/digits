// Host test for caller-selectable ringtones: every song parses, pitches are
// right (Goertzel), level is bounded, rendering is block-size independent
// and notes neither click nor overrun. Run by esp32/tests/run_host_tests.sh.
#include "ringtones.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

static double goertzel(const int16_t *stereo, size_t frames, double hz)
{
    double k = 2 * cos(2 * PI * hz / RINGTONE_SAMPLE_RATE), s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; ++i) {
        double s0 = stereo[2 * i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / frames;
}

static int16_t *render(unsigned id, uint32_t start, size_t frames, size_t block)
{
    int16_t *pcm = malloc(frames * 2 * sizeof(*pcm));
    assert(pcm);
    for (size_t done = 0; done < frames; done += block) {
        size_t n = frames - done < block ? frames - done : block;
        ringtones_render(id, start + (uint32_t)done, pcm + 2 * done, n);
    }
    return pcm;
}

// Fake clip storage: source selects a waveform; 24 kHz like the packer.
static unsigned reads, failing_source = 999;
static int16_t clip_sample(uint32_t source, uint32_t n)
{
    return source == 1 ? (int16_t)(n * 4 % 8000 - 4000) : (int16_t)(2000 * sin(2 * PI * 600 * n / 24000.0));
}
static bool fake_read(uint32_t source, uint32_t first, int16_t *out, size_t count)
{
    ++reads;
    if (source == failing_source) return false;
    assert(count <= 130);
    uint32_t limit = source == 1 ? 24000 : 48000; // Clip lengths below.
    assert(first + count <= limit); // Never reads past the clip.
    for (size_t i = 0; i < count; ++i) out[i] = clip_sample(source, first + (uint32_t)i);
    return true;
}

static void test_clips(void)
{
    ringtones_clip_t clips[] = {
        {.id = 42, .samples = 48000, .source = 2, .name = "Sine"},
        {.id = 12, .samples = 24000, .source = 1, .name = "Ramp"},
        {.id = 12, .samples = 100, .source = 3, .name = "Duplicate"},
        {.id = 5, .samples = 100, .source = 3, .name = "Built-in number"},
        {.id = 100, .samples = 100, .source = 3, .name = "Too high"},
        {.id = 50, .samples = 0, .source = 3, .name = "Empty"},
    };
    assert(ringtones_set_clips(clips, 6, 48001, fake_read) == 0); // Rate above 48 kHz.
    assert(ringtones_set_clips(clips, 6, 24000, fake_read) == 2);
    unsigned ids[16];
    unsigned count = ringtones_list(ids, 16);
    assert(count == ringtones_count() + 2 && ids[count - 2] == 12 && ids[count - 1] == 42);
    assert(ringtones_valid(12) && ringtones_valid(42) && !ringtones_valid(5 + 10) && !ringtones_valid(100));
    assert(!strcmp(ringtones_name(12), "Ramp") && ringtones_name(50) == NULL);
    assert(ringtones_melody_frames(12) == 48000 && ringtones_melody_frames(42) == 96000);

    // 24 kHz -> 48 kHz: even frames are clip samples, odd frames midpoints.
    int16_t *pcm = render(12, 0, 48000 + 960, 480);
    for (uint32_t i = 0; i < 47998; i += 2) {
        assert(pcm[2 * i] == clip_sample(1, i / 2) && pcm[2 * i] == pcm[2 * i + 1]);
        int a = clip_sample(1, i / 2), b = clip_sample(1, i / 2 + 1);
        assert(abs(pcm[2 * (i + 1)] - (a + b) / 2) <= 1);
    }
    for (uint32_t i = 48000; i < 48960; ++i) assert(pcm[2 * i] == 0); // Past the end: silence.
    int16_t *pieces = render(12, 0, 48000 + 960, 7);
    assert(!memcmp(pcm, pieces, (48000 + 960) * 2 * sizeof(int16_t)));
    free(pcm);
    free(pieces);
    // Pitch survives upsampling.
    int16_t *sine = render(42, 1000, 9600, 480);
    assert(goertzel(sine, 9600, 600) > 900 && goertzel(sine, 9600, 1200) < 20);
    free(sine);
    // A failed read renders silence instead of stale data.
    failing_source = 2;
    int16_t *failed = render(42, 0, 480, 480);
    for (int i = 0; i < 960; ++i) assert(failed[i] == 0);
    free(failed);
    failing_source = 999;
    // Clearing the clips leaves only the built-ins.
    assert(ringtones_set_clips(NULL, 0, 24000, fake_read) == 0 && !ringtones_valid(12));
}

int main(void)
{
    assert(ringtones_init());
    assert(ringtones_count() >= 4 && ringtones_count() <= 6);
    assert(!strcmp(ringtones_name(0), "Standard ring") && !ringtones_name(ringtones_count()));
    assert(ringtones_melody_frames(RINGTONE_STANDARD) == 0);

    for (unsigned id = 1; id < ringtones_count(); ++id) {
        uint32_t frames = ringtones_melody_frames(id);
        // Between 2 and 8 seconds: long enough to recognize, short enough to repeat.
        assert(frames >= 2 * RINGTONE_SAMPLE_RATE && frames <= 8 * RINGTONE_SAMPLE_RATE);
        int16_t *whole = render(id, 0, frames + 4800, frames + 4800);
        int16_t *pieces = render(id, 0, frames + 4800, 480);
        assert(!memcmp(whole, pieces, (frames + 4800) * 2 * sizeof(int16_t)));
        int peak = 0, max_step = 0;
        for (uint32_t i = 0; i < frames + 4800; ++i) {
            assert(whole[2 * i] == whole[2 * i + 1]);
            if (abs(whole[2 * i]) > peak) peak = abs(whole[2 * i]);
            if (i && abs(whole[2 * i] - whole[2 * i - 2]) > max_step)
                max_step = abs(whole[2 * i] - whole[2 * i - 2]);
        }
        assert(peak > 1500 && peak <= 4096);
        // Highest note (D6/E6, ~1.3 kHz) plus 3rd harmonic stays smooth; a
        // click between notes would jump far more.
        assert(max_step < 3000);
        for (uint32_t i = frames; i < frames + 4800; ++i) assert(whole[2 * i] == 0);
        printf("ringtone %u \"%s\": %.2f s, peak %d\n", id, ringtones_name(id),
               frames / (double)RINGTONE_SAMPLE_RATE, peak);
        free(whole);
        free(pieces);
    }

    // Fur Elise starts on E6 (1318.5 Hz): its fundamental dominates D#6.
    int16_t *first = render(1, 200, 2400, 2400);
    assert(goertzel(first, 2400, 1318.5) > 5 * goertzel(first, 2400, 1244.5));
    free(first);
    // Ode to Joy starts on E5 (659.3 Hz).
    int16_t *ode = render(2, 200, 4800, 4800);
    assert(goertzel(ode, 4800, 659.3) > 5 * goertzel(ode, 4800, 698.5));
    free(ode);
    // Westminster starts on G#5 (830.6 Hz).
    int16_t *bell = render(3, 200, 4800, 4800);
    assert(goertzel(bell, 4800, 830.6) > 5 * goertzel(bell, 4800, 739.99));
    free(bell);

    // Out-of-range ids and the standard ring render silence here.
    int16_t *none = render(99, 0, 480, 480);
    int16_t *standard = render(RINGTONE_STANDARD, 0, 480, 480);
    for (int i = 0; i < 960; ++i) assert(!none[i] && !standard[i]);
    free(none);
    free(standard);
    test_clips();
    puts("ringtones_test: melodies (parse, pitch, level, continuity, blocks) and clips (upsampling, bounds, ordering, failures) passed");
    return 0;
}
