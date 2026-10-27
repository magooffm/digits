// Host tests for ogg_opus.c. fixtures/tone.opus is 1.5 s of 440 Hz made by
// ffmpeg/libopus, so the reader and the CRC are checked against another
// implementation; files written here are read back by the same reader.
#include "ogg_opus.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PACKETS 128

static uint8_t body[OGG_READER_BODY];
static ogg_writer_t writer;

typedef struct {
    size_t count;
    size_t size[MAX_PACKETS];
    uint8_t data[MAX_PACKETS][1275];
    uint16_t preskip;
} packets_t;

static void fixture_path(char *out, size_t capacity)
{
    const char *file = __FILE__;
    const char *slash = strrchr(file, '/');
    snprintf(out, capacity, "%.*s/fixtures/tone.opus", slash ? (int)(slash - file) : 1, slash ? file : ".");
}

static void read_all(FILE *file, packets_t *out)
{
    ogg_reader_t reader;
    ogg_reader_start(&reader, file, body);
    const uint8_t *packet;
    size_t size;
    out->count = 0;
    while ((size = ogg_reader_next(&reader, &packet))) {
        assert(out->count < MAX_PACKETS && size <= 1275);
        memcpy(out->data[out->count], packet, size);
        out->size[out->count++] = size;
    }
    out->preskip = reader.preskip;
}

static size_t file_bytes(FILE *file, uint8_t *out, size_t capacity)
{
    rewind(file);
    return fread(out, 1, capacity, file);
}

static unsigned count_pages(const uint8_t *data, size_t size)
{
    unsigned pages = 0;
    for (size_t i = 0; i + 4 <= size; ++i) pages += !memcmp(data + i, "OggS", 4);
    return pages;
}

static void test_packet_samples(void)
{
    uint8_t p[2] = {0, 0};
    assert(opus_packet_samples(p, 0) == 0);
    p[0] = 0 << 3;                     // SILK 10 ms.
    assert(opus_packet_samples(p, 1) == 480);
    p[0] = 1 << 3;
    assert(opus_packet_samples(p, 1) == 960);
    p[0] = 3 << 3;
    assert(opus_packet_samples(p, 1) == 2880);
    p[0] = 12 << 3;                    // Hybrid 10 ms.
    assert(opus_packet_samples(p, 1) == 480);
    p[0] = 13 << 3 | 1;                // Hybrid 20 ms, two frames.
    assert(opus_packet_samples(p, 1) == 1920);
    p[0] = 16 << 3;                    // CELT 2.5 ms.
    assert(opus_packet_samples(p, 1) == 120);
    p[0] = 31 << 3 | 3;                // CELT 20 ms, code 3.
    p[1] = 6;
    assert(opus_packet_samples(p, 2) == 5760);
    p[1] = 7;                          // 140 ms is too long.
    assert(opus_packet_samples(p, 2) == 0);
    p[1] = 0;
    assert(opus_packet_samples(p, 2) == 0);
    assert(opus_packet_samples(p, 1) == 0); // Code 3 without its count byte.
}

static void test_fixture(packets_t *fixture)
{
    char path[512];
    fixture_path(path, sizeof(path));
    FILE *file = fopen(path, "rb");
    assert(file);
    read_all(file, fixture);
    assert(fixture->preskip == 312);
    assert(fixture->count >= 75);
    for (size_t i = 0; i < fixture->count; ++i)
        assert(opus_packet_samples(fixture->data[i], fixture->size[i]) == 960);
    static uint8_t bytes[16384];
    size_t size = file_bytes(file, bytes, sizeof(bytes));
    fclose(file);
    uint64_t granule = 0;
    assert(ogg_last_granule(bytes, size, &granule));
    assert(granule - 312 == 72000); // opusinfo: 1.500 s.
    // A truncated last page is skipped for the one before it.
    assert(ogg_last_granule(bytes, size - 1, &granule) && granule < 72312);
    assert(!ogg_last_granule(bytes, 20, &granule));
}

static void test_round_trip(const packets_t *fixture)
{
    FILE *file = tmpfile();
    assert(file);
    assert(ogg_writer_start(&writer, file, 0x1234, "DATE=2026-10-27"));
    for (size_t i = 0; i < fixture->count; ++i)
        assert(ogg_writer_add(&writer, fixture->data[i], fixture->size[i], 960));
    assert(ogg_writer_finish(&writer));
    static uint8_t bytes[16384];
    size_t size = file_bytes(file, bytes, sizeof(bytes));
    // Head, tags, then 50 packets per page with the rest on the last page.
    assert(count_pages(bytes, size) == 2 + (fixture->count + 49) / 50);
    assert(bytes[5] == 0x02);                          // First page.
    uint64_t granule = 0;
    assert(ogg_last_granule(bytes, size, &granule) && granule == fixture->count * 960);
    rewind(file);
    static packets_t back;
    read_all(file, &back);
    fclose(file);
    assert(back.preskip == OGG_OPUS_PRESKIP && back.count == fixture->count);
    for (size_t i = 0; i < back.count; ++i)
        assert(back.size[i] == fixture->size[i] && !memcmp(back.data[i], fixture->data[i], back.size[i]));
}

static void test_lacing(void)
{
    // Exact multiples of 255 need a terminating zero lace.
    static const size_t sizes[] = {1, 254, 255, 256, 510, 1275, 3};
    static uint8_t packet[1275];
    FILE *file = tmpfile();
    assert(ogg_writer_start(&writer, file, 7, NULL));
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        memset(packet, (int)(i + 1), sizes[i]);
        assert(ogg_writer_add(&writer, packet, sizes[i], 960));
    }
    assert(!ogg_writer_add(&writer, packet, 0, 960));
    assert(ogg_writer_finish(&writer));
    rewind(file);
    static packets_t back;
    read_all(file, &back);
    assert(back.count == sizeof(sizes) / sizeof(sizes[0]));
    for (size_t i = 0; i < back.count; ++i) {
        assert(back.size[i] == sizes[i]);
        assert(back.data[i][0] == i + 1 && back.data[i][sizes[i] - 1] == i + 1);
    }
    fclose(file);
}

static void test_page_limits(void)
{
    // 13 packets of 1275 bytes exceed one 16 KiB page; nothing is lost.
    static uint8_t packet[1275];
    FILE *file = tmpfile();
    assert(ogg_writer_start(&writer, file, 9, NULL));
    for (unsigned i = 0; i < 13; ++i) {
        memset(packet, (int)i, sizeof(packet));
        assert(ogg_writer_add(&writer, packet, sizeof(packet), 960));
    }
    assert(ogg_writer_finish(&writer));
    static uint8_t bytes[32768];
    size_t size = file_bytes(file, bytes, sizeof(bytes));
    assert(count_pages(bytes, size) == 4);
    rewind(file);
    static packets_t back;
    read_all(file, &back);
    assert(back.count == 13 && back.data[12][0] == 12);
    fclose(file);
}

static void test_damage_and_tags(const packets_t *fixture)
{
    FILE *file = tmpfile();
    assert(ogg_writer_start(&writer, file, 3, NULL));
    for (size_t i = 0; i < 60 && i < fixture->count; ++i)
        assert(ogg_writer_add(&writer, fixture->data[i], fixture->size[i], 960));
    assert(ogg_writer_finish(&writer));
    static uint8_t bytes[16384];
    size_t size = file_bytes(file, bytes, sizeof(bytes));
    // Flip one byte in the last page's body: reading stops after page one.
    bytes[size - 2] ^= 0x40;
    rewind(file);
    assert(fwrite(bytes, 1, size, file) == size);
    rewind(file);
    static packets_t back;
    read_all(file, &back);
    assert(back.count == 50);
    fclose(file);

    uint8_t tags[64];
    assert(ogg_opus_tags(tags, 20, "digits-esp32", NULL) == 0);
    assert(ogg_opus_tags(tags, sizeof(tags), "v", "A=b") == 8 + 4 + 1 + 4 + 4 + 3);
}

int main(void)
{
    static packets_t fixture;
    test_packet_samples();
    test_fixture(&fixture);
    test_round_trip(&fixture);
    test_lacing();
    test_page_limits();
    test_damage_and_tags(&fixture);
    puts("ogg_opus tests passed");
    return 0;
}
