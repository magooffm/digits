#include "sd_ringtone_util.h"
#include <ctype.h>
#include <string.h>

int sd_ringtone_position(const char *name, unsigned max_positions)
{
    static const char prefix[] = "ringtone-", suffix[] = ".mp3";
    if (!name) return -1;
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i)
        if (tolower((unsigned char)name[i]) != prefix[i]) return -1;
    const char *p = name + sizeof(prefix) - 1;
    unsigned value = 0, digits = 0;
    while (isdigit((unsigned char)*p) && digits < 3) {
        value = value * 10 + (unsigned)(*p++ - '0');
        ++digits;
    }
    if (digits < 1 || digits > 2 || value < 1 || value > max_positions) return -1;
    for (size_t i = 0; i < sizeof(suffix); ++i) // Includes the terminator.
        if (tolower((unsigned char)p[i]) != suffix[i]) return -1;
    return (int)value - 1;
}

uint32_t sd_ringtone_id3_size(const uint8_t *header, size_t length)
{
    if (length < 10 || memcmp(header, "ID3", 3)) return 0;
    uint32_t size = 0;
    for (unsigned i = 6; i < 10; ++i) {
        if (header[i] & 0x80) return 0; // Not a syncsafe size: not a tag.
        size = size << 7 | header[i];
    }
    uint32_t footer = header[3] == 4 && (header[5] & 0x10) ? 10 : 0;
    return 10 + size + footer;
}

typedef struct {
    uint32_t rate, kbps, bytes, samples, side;
    unsigned version; // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5.
} frame_t;

static bool frame_header(const uint8_t *h, frame_t *f)
{
    static const uint16_t kbps1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t kbps2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t rates[3] = {44100, 48000, 32000};
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    unsigned version = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    unsigned bitrate = h[2] >> 4, rate = (h[2] >> 2) & 3, padding = (h[2] >> 1) & 1;
    if (version == 1 || layer != 1 || rate == 3 || !bitrate || bitrate == 15) return false;
    bool mpeg1 = version == 3, mono = (h[3] >> 6) == 3;
    f->version = version;
    f->rate = rates[rate] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
    f->kbps = (mpeg1 ? kbps1 : kbps2)[bitrate];
    f->samples = mpeg1 ? 1152 : 576;
    f->bytes = (mpeg1 ? 144 : 72) * f->kbps * 1000 / f->rate + padding;
    f->side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    return true;
}

static uint32_t big_endian(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool sd_ringtone_mp3_length(const uint8_t *head, size_t length, uint64_t audio_bytes,
                            uint32_t *sample_rate, uint64_t *samples)
{
    for (size_t at = 0; at + 4 <= length; ++at) {
        frame_t f, next;
        if (!frame_header(head + at, &f)) continue;
        // A second header right after confirms the sync (when it fits).
        size_t following = at + f.bytes;
        if (following + 4 <= length &&
            (!frame_header(head + following, &next) || next.rate != f.rate || next.version != f.version))
            continue;
        *sample_rate = f.rate;
        size_t xing = at + 4 + f.side, vbri = at + 4 + 32;
        if (xing + 12 <= length && (!memcmp(head + xing, "Xing", 4) || !memcmp(head + xing, "Info", 4)) &&
            (big_endian(head + xing + 4) & 1)) {
            *samples = (uint64_t)big_endian(head + xing + 8) * f.samples;
            return true;
        }
        if (vbri + 18 <= length && !memcmp(head + vbri, "VBRI", 4)) {
            *samples = (uint64_t)big_endian(head + vbri + 14) * f.samples;
            return true;
        }
        uint64_t bytes = audio_bytes > at ? audio_bytes - at : 0;
        *samples = bytes * 8 * f.rate / (f.kbps * 1000);
        return true;
    }
    return false;
}

size_t sd_ringtone_downmix(const int16_t *in, size_t frames, unsigned channels, int16_t *out)
{
    if (!channels) return 0;
    for (size_t f = 0; f < frames; ++f) {
        int32_t sum = 0;
        for (unsigned c = 0; c < channels; ++c) sum += in[f * channels + c];
        out[f] = (int16_t)(sum / (int32_t)channels);
    }
    return frames;
}

size_t sd_ringtone_first_sound(const int16_t *pcm, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (pcm[i] > SD_RINGTONE_SILENCE || pcm[i] < -SD_RINGTONE_SILENCE) return i;
    return count;
}

void sd_ringtone_scale(int16_t *pcm, size_t count, unsigned percent)
{
    if (percent >= 100) return;
    for (size_t i = 0; i < count; ++i) pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)percent / 100);
}

bool sd_ringtone_name_valid(const char *name, size_t max_length)
{
    size_t length = strlen(name);
    if (length < 5 || length >= max_length || name[0] == '.' || name[length - 1] == ' ') return false;
    const char *ext = name + length - 4;
    if (ext[0] != '.' || (ext[1] | 0x20) != 'm' || (ext[2] | 0x20) != 'p' || ext[3] != '3') return false;
    for (const char *p = name; *p; ++p)
        if ((unsigned char)*p < 0x20 || strchr("/\\:*?\"<>|", *p)) return false;
    return true;
}
