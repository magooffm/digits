#include "microphone_pcm.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void assert_near(double actual, double expected)
{
    assert(fabs(actual - expected) < 1e-9);
}

static void assert_level_equal(const microphone_pcm_level_t *a,
                               const microphone_pcm_level_t *b)
{
    assert(a->sample_count == b->sample_count);
    assert(a->sum_squares == b->sum_squares);
    assert(a->zero_count == b->zero_count);
    assert(a->clipped_count == b->clipped_count);
    assert(a->peak == b->peak);
}

static void test_slots_and_extremes(void)
{
    const int16_t stereo[] = {
        INT16_MIN, INT16_MAX,
        INT16_MAX, INT16_MAX,
        INT16_MIN, INT16_MIN,
        1, -2,
        -3, 2,
        0, 0,
    };
    const int16_t expected[][6] = {
        { INT16_MIN, INT16_MAX, INT16_MIN, 1, -3, 0 },
        { INT16_MAX, INT16_MAX, INT16_MIN, -2, 2, 0 },
        { 0, INT16_MAX, INT16_MIN, 0, 0, 0 },
    };
    int16_t mono[6];
    for (int source = MICROPHONE_PCM_LEFT;
         source <= MICROPHONE_PCM_AVERAGE; ++source) {
        microphone_pcm_stats_t stats;
        microphone_pcm_stats_reset(&stats);
        microphone_pcm_convert(stereo, mono, 6,
                               (microphone_pcm_slot_t)source, &stats);
        assert(memcmp(mono, expected[source], sizeof(mono)) == 0);
        assert(stats.left.sample_count == 6);
        assert(stats.left.sum_squares == UINT64_C(3221159947));
        assert(stats.left.zero_count == 1);
        assert(stats.left.clipped_count == 3);
        assert(stats.left.peak == 32768);
        assert(stats.right.sample_count == 6);
        assert(stats.right.sum_squares == UINT64_C(3221094410));
        assert(stats.right.zero_count == 1);
        assert(stats.right.clipped_count == 3);
        assert(stats.right.peak == 32768);
        if (source == MICROPHONE_PCM_LEFT) {
            assert_level_equal(&stats.left, &stats.mono);
        } else if (source == MICROPHONE_PCM_RIGHT) {
            assert_level_equal(&stats.right, &stats.mono);
        } else {
            assert(stats.mono.sample_count == 6);
            assert(stats.mono.sum_squares == UINT64_C(2147418113));
            assert(stats.mono.zero_count == 4);
            assert(stats.mono.clipped_count == 2);
            assert(stats.mono.peak == 32768);
        }
    }
    // Statistics are optional; in-place downmixing must retain unread slots.
    int16_t inplace[12];
    memcpy(inplace, stereo, sizeof(inplace));
    microphone_pcm_convert(inplace, inplace, 6, MICROPHONE_PCM_RIGHT, NULL);
    assert(memcmp(inplace, expected[MICROPHONE_PCM_RIGHT], sizeof(mono)) == 0);
}

static void test_clipping_threshold(void)
{
    const int16_t stereo[] = {
        -32760, -32760, -32759, -32759,
        32759, 32759, 32760, 32760,
        INT16_MIN, INT16_MIN, INT16_MAX, INT16_MAX, 0, 0,
    };
    int16_t mono[7];
    microphone_pcm_stats_t stats;
    microphone_pcm_stats_reset(&stats);
    microphone_pcm_convert(stereo, mono, 7, MICROPHONE_PCM_AVERAGE, &stats);
    assert(stats.mono.sample_count == 7);
    assert(stats.mono.clipped_count == 4);
    assert(stats.mono.zero_count == 1);
    assert(stats.mono.peak == 32768);
    assert_level_equal(&stats.left, &stats.mono);
    assert_level_equal(&stats.right, &stats.mono);
}

static void test_energy_and_dbfs(void)
{
    microphone_pcm_stats_t stats;
    microphone_pcm_stats_reset(&stats);
    assert_near(microphone_pcm_rms(&stats.mono), 0.0);
    assert_near(microphone_pcm_dbfs(&stats.mono), -96.0);
    microphone_pcm_convert(NULL, NULL, 0, MICROPHONE_PCM_LEFT, &stats);
    assert(stats.mono.sample_count == 0);

    const int16_t zero[] = { 0, 0, 0, 0 };
    int16_t mono[2];
    microphone_pcm_convert(zero, mono, 2, MICROPHONE_PCM_LEFT, &stats);
    assert(stats.mono.sample_count == 2 && stats.mono.zero_count == 2);
    assert_near(microphone_pcm_rms(&stats.mono), 0.0);
    assert_near(microphone_pcm_dbfs(&stats.mono), -96.0);

    const int16_t small[] = { 3, 3, -4, -4 };
    microphone_pcm_stats_reset(&stats);
    microphone_pcm_convert(small, mono, 2, MICROPHONE_PCM_LEFT, &stats);
    assert(stats.mono.sum_squares == 25);
    assert_near(microphone_pcm_rms(&stats.mono), sqrt(12.5));

    const int16_t half_scale[] = { -16384, -16384, 16384, 16384 };
    microphone_pcm_stats_reset(&stats);
    microphone_pcm_convert(half_scale, mono, 2, MICROPHONE_PCM_AVERAGE, &stats);
    assert_near(microphone_pcm_rms(&stats.mono), 16384.0);
    assert_near(microphone_pcm_dbfs(&stats.mono), -6.020599913279624);

    const int16_t full_scale[] = { INT16_MIN, INT16_MIN };
    microphone_pcm_stats_reset(&stats);
    microphone_pcm_convert(full_scale, mono, 1, MICROPHONE_PCM_LEFT, &stats);
    assert_near(microphone_pcm_rms(&stats.mono), 32768.0);
    assert_near(microphone_pcm_dbfs(&stats.mono), 0.0);

    const int16_t almost_silent[] = { 1, 1, -1, -1 };
    microphone_pcm_stats_reset(&stats);
    microphone_pcm_convert(almost_silent, mono, 2, MICROPHONE_PCM_LEFT, &stats);
    assert_near(microphone_pcm_rms(&stats.mono), 1.0);
    assert(stats.mono.zero_count == 0);
    assert_near(microphone_pcm_dbfs(&stats.mono), -90.30899869919436);

    // The dBFS floor also applies to very low nonzero window energy.
    const microphone_pcm_level_t sparse = {
        .sample_count = 1000, .sum_squares = 1, .peak = 1,
    };
    assert_near(microphone_pcm_dbfs(&sparse), -96.0);
}

static void test_chunk_continuity(void)
{
    const int16_t stereo[] = {
        INT16_MIN, INT16_MAX, 0, 0, 23, -25, -401, 803,
        32760, -32760, -3, -2, 1, 0, 32759, -32759,
        -20000, -21000, 7, 8, INT16_MAX, INT16_MAX,
        INT16_MIN, INT16_MIN, 20, 30, -20, -30, 600, 0,
        0, -700, -1, 2,
    };
    const size_t chunks[] = { 1, 6, 2, 8 };
    int16_t full[17], chunked[17];
    for (int source = MICROPHONE_PCM_LEFT;
         source <= MICROPHONE_PCM_AVERAGE; ++source) {
        microphone_pcm_stats_t whole, parts;
        microphone_pcm_stats_reset(&whole);
        microphone_pcm_stats_reset(&parts);
        microphone_pcm_convert(stereo, full, 17,
                               (microphone_pcm_slot_t)source, &whole);
        size_t offset = 0;
        for (size_t chunk = 0; chunk < 4; ++chunk) {
            microphone_pcm_convert(stereo + 2 * offset, chunked + offset,
                                   chunks[chunk], (microphone_pcm_slot_t)source,
                                   &parts);
            offset += chunks[chunk];
        }
        assert(offset == 17);
        assert(memcmp(full, chunked, sizeof(full)) == 0);
        assert_level_equal(&whole.left, &parts.left);
        assert_level_equal(&whole.right, &parts.right);
        assert_level_equal(&whole.mono, &parts.mono);
        assert_near(microphone_pcm_rms(&whole.mono),
                    microphone_pcm_rms(&parts.mono));
        assert_near(microphone_pcm_dbfs(&whole.mono),
                    microphone_pcm_dbfs(&parts.mono));
    }
}

int main(void)
{
    test_slots_and_extremes();
    test_clipping_threshold();
    test_energy_and_dbfs();
    test_chunk_continuity();
    puts("PASS: microphone slot selection, signed extremes, averaging, "
         "statistics, clipping, dBFS and chunk continuity");
    return 0;
}
