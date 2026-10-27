#include "ogg_opus.h"
#include <string.h>

#define TYPE_CONTINUED 0x01
#define TYPE_FIRST 0x02
#define TYPE_LAST 0x04
#define NO_GRANULE UINT64_MAX

uint32_t ogg_crc(uint32_t crc, const uint8_t *data, size_t size)
{
    // Polynomial 0x04c11db7, no reflection, zero start and no final XOR.
    // Bitwise is fast enough for a few kilobytes per second.
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; ++bit)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

unsigned opus_packet_samples(const uint8_t *packet, size_t size)
{
    static const unsigned silk[4] = {480, 960, 1920, 2880};
    static const unsigned celt[4] = {120, 240, 480, 960};
    if (!packet || !size) return 0;
    unsigned config = packet[0] >> 3;
    unsigned frame = config < 12 ? silk[config & 3] : config < 16 ? (config & 1 ? 960 : 480)
                                                                  : celt[config & 3];
    unsigned count;
    switch (packet[0] & 3) {
    case 0: count = 1; break;
    case 1:
    case 2: count = 2; break;
    default:
        if (size < 2) return 0;
        count = packet[1] & 0x3f;
    }
    unsigned samples = frame * count;
    return samples && samples <= OGG_OPUS_MAX_PACKET_SAMPLES ? samples : 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

size_t ogg_opus_head(uint8_t out[19])
{
    memcpy(out, "OpusHead", 8);
    out[8] = 1;  // Version.
    out[9] = 1;  // Mono.
    put16(out + 10, OGG_OPUS_PRESKIP);
    put32(out + 12, OGG_OPUS_RATE);
    put16(out + 16, 0); // Output gain.
    out[18] = 0;        // Channel mapping family.
    return 19;
}

size_t ogg_opus_tags(uint8_t *out, size_t capacity, const char *vendor, const char *comment)
{
    size_t vendor_size = strlen(vendor), comment_size = comment ? strlen(comment) : 0;
    size_t size = 8 + 4 + vendor_size + 4 + (comment ? 4 + comment_size : 0);
    if (size > capacity) return 0;
    memcpy(out, "OpusTags", 8);
    put32(out + 8, (uint32_t)vendor_size);
    memcpy(out + 12, vendor, vendor_size);
    uint8_t *p = out + 12 + vendor_size;
    put32(p, comment ? 1 : 0);
    if (comment) {
        put32(p + 4, (uint32_t)comment_size);
        memcpy(p + 8, comment, comment_size);
    }
    return size;
}

static bool flush_page(ogg_writer_t *w, uint8_t type)
{
    if (w->failed) return false;
    uint8_t header[OGG_HEADER_MAX];
    memcpy(header, "OggS", 4);
    header[4] = 0;
    header[5] = type;
    put64(header + 6, w->granule);
    put32(header + 14, w->serial);
    put32(header + 18, w->sequence);
    put32(header + 22, 0);
    header[26] = (uint8_t)w->segment_count;
    memcpy(header + 27, w->segments, w->segment_count);
    size_t header_size = 27 + w->segment_count;
    put32(header + 22, ogg_crc(ogg_crc(0, header, header_size), w->body, w->body_size));
    if (fwrite(header, 1, header_size, w->file) != header_size ||
        fwrite(w->body, 1, w->body_size, w->file) != w->body_size) {
        w->failed = true;
        return false;
    }
    ++w->sequence;
    w->segment_count = w->packets = 0;
    w->body_size = 0;
    return true;
}

static bool fits(const ogg_writer_t *w, size_t size)
{
    return w->segment_count + size / 255 + 1 <= 255 && w->body_size + size <= OGG_WRITER_BODY;
}

static void append(ogg_writer_t *w, const uint8_t *packet, size_t size)
{
    for (size_t left = size; ; left -= 255) {
        w->segments[w->segment_count++] = (uint8_t)(left < 255 ? left : 255);
        if (left < 255) break;
    }
    memcpy(w->body + w->body_size, packet, size);
    w->body_size += size;
    ++w->packets;
}

bool ogg_writer_start(ogg_writer_t *w, FILE *file, uint32_t serial, const char *comment)
{
    memset(w, 0, offsetof(ogg_writer_t, body));
    w->file = file;
    w->serial = serial;
    uint8_t header[256];
    append(w, header, ogg_opus_head(header));
    if (!flush_page(w, TYPE_FIRST)) return false;
    size_t tags = ogg_opus_tags(header, sizeof(header), "digits-esp32", comment);
    if (!tags) tags = ogg_opus_tags(header, sizeof(header), "digits-esp32", NULL);
    append(w, header, tags);
    return flush_page(w, 0);
}

bool ogg_writer_add(ogg_writer_t *w, const uint8_t *packet, size_t size, unsigned samples)
{
    if (w->failed || !size || size > OGG_WRITER_BODY - 255) return false;
    if ((w->packets >= OGG_WRITER_PACKETS || !fits(w, size)) && !flush_page(w, 0)) return false;
    append(w, packet, size);
    w->granule += samples;
    return true;
}

bool ogg_writer_finish(ogg_writer_t *w)
{
    return flush_page(w, TYPE_LAST) && fflush(w->file) == 0;
}

void ogg_reader_start(ogg_reader_t *r, FILE *file, uint8_t *body)
{
    memset(r, 0, sizeof(*r));
    r->file = file;
    r->body = body;
}

// Reads the next valid page of the stream into the reader.
static bool read_page(ogg_reader_t *r)
{
    for (;;) {
        uint8_t header[OGG_HEADER_MAX];
        if (fread(header, 1, 27, r->file) != 27 || memcmp(header, "OggS", 4) || header[4]) return false;
        unsigned count = header[26];
        if (fread(header + 27, 1, count, r->file) != count) return false;
        size_t body = 0;
        for (unsigned i = 0; i < count; ++i) body += header[27 + i];
        if (fread(r->body, 1, body, r->file) != body) return false;
        uint32_t stored = get32(header + 22);
        put32(header + 22, 0);
        if (ogg_crc(ogg_crc(0, header, 27 + count), r->body, body) != stored) return false;
        uint32_t serial = get32(header + 14);
        if (!r->have_serial) {
            r->serial = serial;
            r->have_serial = true;
        } else if (serial != r->serial) {
            continue; // Another multiplexed stream.
        }
        memcpy(r->segments, header + 27, count);
        r->segment_count = count;
        r->segment = 0;
        r->body_size = body;
        r->offset = 0;
        r->continued = header[5] & TYPE_CONTINUED;
        return true;
    }
}

size_t ogg_reader_next(ogg_reader_t *r, const uint8_t **packet)
{
    for (;;) {
        if (r->segment >= r->segment_count && !read_page(r)) return 0;
        size_t start = r->offset, size = 0;
        bool complete = false;
        while (r->segment < r->segment_count) {
            uint8_t lace = r->segments[r->segment++];
            size += lace;
            if (lace < 255) {
                complete = true;
                break;
            }
        }
        r->offset += size;
        bool skip = r->continued || !complete;
        r->continued = false;
        if (skip) continue;
        const uint8_t *data = r->body + start;
        if (r->headers == 0) {
            if (size < 19 || memcmp(data, "OpusHead", 8) || data[8] > 15) return 0;
            r->preskip = get16(data + 10);
            ++r->headers;
            continue;
        }
        if (r->headers == 1) {
            if (size < 8 || memcmp(data, "OpusTags", 8)) return 0;
            ++r->headers;
            continue;
        }
        if (!size) continue;
        *packet = data;
        return size;
    }
}

bool ogg_last_granule(const uint8_t *tail, size_t size, uint64_t *granule)
{
    for (size_t i = size >= 27 ? size - 27 + 1 : 0; i-- > 0;) {
        if (memcmp(tail + i, "OggS", 4)) continue;
        unsigned count = tail[i + 26];
        if (i + 27 + count > size) continue;
        size_t body = 0;
        for (unsigned s = 0; s < count; ++s) body += tail[i + 27 + s];
        if (i + 27 + count + body > size) continue;
        uint8_t header[OGG_HEADER_MAX];
        memcpy(header, tail + i, 27 + count);
        put32(header + 22, 0);
        if (ogg_crc(ogg_crc(0, header, 27 + count), tail + i + 27 + count, body) != get32(tail + i + 22))
            continue;
        uint64_t value = get64(tail + i + 6);
        if (value == NO_GRANULE) continue;
        *granule = value;
        return true;
    }
    return false;
}
