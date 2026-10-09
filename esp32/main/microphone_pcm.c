#include "microphone_pcm.h"

#include <math.h>
#include <string.h>

static void accumulate_sample(microphone_pcm_level_t *level, int16_t sample)
{
    const int32_t signed_value = sample;
    const uint32_t magnitude = (uint32_t)(signed_value < 0
                                            ? -signed_value : signed_value);

    ++level->sample_count;
    level->sum_squares += (uint64_t)magnitude * magnitude;
    level->zero_count += magnitude == 0;
    level->clipped_count += magnitude >= 32760;
    if (magnitude > level->peak) {
        level->peak = magnitude;
    }
}

void microphone_pcm_stats_reset(microphone_pcm_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
}

void microphone_pcm_convert(const int16_t *stereo, int16_t *mono,
                            size_t frames, microphone_pcm_slot_t slot,
                            microphone_pcm_stats_t *stats)
{
    for (size_t frame = 0; frame < frames; ++frame) {
        const int16_t left = stereo[2 * frame];
        const int16_t right = stereo[2 * frame + 1];
        int16_t sample;
        switch (slot) {
        case MICROPHONE_PCM_RIGHT:
            sample = right;
            break;
        case MICROPHONE_PCM_AVERAGE:
            sample = (int16_t)(((int32_t)left + (int32_t)right) / 2);
            break;
        case MICROPHONE_PCM_LEFT:
        default:
            sample = left;
            break;
        }
        mono[frame] = sample;
        if (stats != NULL) {
            accumulate_sample(&stats->left, left);
            accumulate_sample(&stats->right, right);
            accumulate_sample(&stats->mono, sample);
        }
    }
}

double microphone_pcm_rms(const microphone_pcm_level_t *level)
{
    if (level->sample_count == 0) {
        return 0.0;
    }
    return sqrt((double)level->sum_squares / (double)level->sample_count);
}

double microphone_pcm_dbfs(const microphone_pcm_level_t *level)
{
    const double rms = microphone_pcm_rms(level);
    if (rms == 0.0) {
        return -96.0;
    }
    const double dbfs = 20.0 * log10(rms / 32768.0);
    return dbfs < -96.0 ? -96.0 : dbfs;
}
