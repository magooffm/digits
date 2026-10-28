#include "voice_style.h"
#include <math.h>
#include <string.h>

static void section(voice_style_section_t *s, unsigned rate, double f0, int high_pass)
{
    const double q = sqrt(2.0) / 2, w0 = 2 * 3.14159265358979323846 * f0 / rate;
    const double cos_w = cos(w0), alpha = sin(w0) / (2 * q), a0 = 1 + alpha;
    double b0 = high_pass ? (1 + cos_w) / 2 : (1 - cos_w) / 2;
    double b1 = high_pass ? -(1 + cos_w) : 1 - cos_w;
    memset(s, 0, sizeof(*s));
    s->b0 = (float)(b0 / a0);
    s->b1 = (float)(b1 / a0);
    s->b2 = (float)(b0 / a0);
    s->a1 = (float)(-2 * cos_w / a0);
    s->a2 = (float)((1 - alpha) / a0);
}

void voice_style_copper_init(voice_style_copper_t *filter, unsigned sample_rate)
{
    section(&filter->section[0], sample_rate, 300, 1);
    section(&filter->section[1], sample_rate, 300, 1);
    section(&filter->section[2], sample_rate, 3400, 0);
    section(&filter->section[3], sample_rate, 3400, 0);
}

void voice_style_copper_reset(voice_style_copper_t *filter)
{
    for (unsigned i = 0; i < VOICE_STYLE_SECTIONS; ++i) {
        voice_style_section_t *s = &filter->section[i];
        s->x1 = s->x2 = s->y1 = s->y2 = 0;
    }
}

void voice_style_copper_process(voice_style_copper_t *filter, int16_t *pcm, size_t count)
{
    for (size_t n = 0; n < count; ++n) {
        float v = pcm[n];
        for (unsigned i = 0; i < VOICE_STYLE_SECTIONS; ++i) {
            voice_style_section_t *s = &filter->section[i];
            float y = s->b0 * v + s->b1 * s->x1 + s->b2 * s->x2 - s->a1 * s->y1 - s->a2 * s->y2;
            s->x2 = s->x1;
            s->x1 = v;
            s->y2 = s->y1;
            s->y1 = y;
            v = y;
        }
        pcm[n] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v); // Truncates, as in Go.
    }
}
