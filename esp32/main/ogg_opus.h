#pragma once

// Ogg Opus files (RFC 3533, RFC 7845) for answering-machine messages: the
// received Opus packets are stored unchanged, so any player opens them.
// Pure C plus stdio, no ESP-IDF: tests/ogg_opus_test.c runs on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define OGG_OPUS_RATE 48000
#define OGG_OPUS_PRESKIP 312 // libopus encoder lookahead at 48 kHz.
#define OGG_HEADER_MAX (27 + 255)
#define OGG_WRITER_BODY 16384
#define OGG_WRITER_PACKETS 50 // One page per second of 20 ms packets.
#define OGG_READER_BODY (255 * 255)
#define OGG_OPUS_MAX_PACKET_SAMPLES 5760 // 120 ms.

uint32_t ogg_crc(uint32_t crc, const uint8_t *data, size_t size);

// Samples at 48 kHz in one Opus packet (RFC 6716 section 3.1), or 0 if the
// packet is malformed or longer than 120 ms.
unsigned opus_packet_samples(const uint8_t *packet, size_t size);

// Identification and comment headers. tags returns 0 if capacity is short.
size_t ogg_opus_head(uint8_t out[19]);
size_t ogg_opus_tags(uint8_t *out, size_t capacity, const char *vendor, const char *comment);

// Writes one logical stream: header pages, then audio pages of up to one
// second. Packets never span pages. A write error sticks: every later call
// returns false and the file should be discarded.
typedef struct {
    FILE *file;
    uint32_t serial, sequence;
    uint64_t granule; // Including pre-skip, up to the last packet added.
    uint8_t segments[255];
    unsigned segment_count, packets;
    size_t body_size;
    bool failed;
    uint8_t body[OGG_WRITER_BODY];
} ogg_writer_t;

bool ogg_writer_start(ogg_writer_t *w, FILE *file, uint32_t serial, const char *comment);
bool ogg_writer_add(ogg_writer_t *w, const uint8_t *packet, size_t size, unsigned samples);
// Flushes the last page with the end-of-stream flag; does not close the file.
bool ogg_writer_finish(ogg_writer_t *w);

// Reads packets of the first logical stream, skipping its two headers.
// Packets spanning pages (never written above) are skipped.
typedef struct {
    FILE *file;
    uint32_t serial;
    bool have_serial;
    unsigned headers;    // Header packets seen.
    uint8_t segments[255];
    unsigned segment_count, segment;
    size_t body_size, offset;
    bool continued;      // Current packet began on an earlier page.
    uint16_t preskip;
    uint8_t *body;       // OGG_READER_BODY bytes, supplied by the caller.
} ogg_reader_t;

void ogg_reader_start(ogg_reader_t *r, FILE *file, uint8_t *body);
// Returns the next audio packet's size and points *packet into r->body, or
// 0 at the end of the stream or on a damaged page.
size_t ogg_reader_next(ogg_reader_t *r, const uint8_t **packet);

// Finds the granule position of the last complete, valid page in the final
// size bytes of a file. Audio length is that minus the pre-skip.
bool ogg_last_granule(const uint8_t *tail, size_t size, uint64_t *granule);
