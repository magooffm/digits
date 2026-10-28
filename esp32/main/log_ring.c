#include "log_ring.h"
#include <string.h>

void log_ring_init(log_ring_t *ring, char *storage, size_t capacity)
{
    ring->data = storage;
    ring->capacity = capacity;
    ring->written = 0;
}

void log_ring_append(log_ring_t *ring, const char *text, size_t length)
{
    if (length > ring->capacity) { // Only the newest part fits.
        text += length - ring->capacity;
        ring->written += length - ring->capacity;
        length = ring->capacity;
    }
    size_t at = (size_t)(ring->written % ring->capacity);
    size_t first = ring->capacity - at < length ? ring->capacity - at : length;
    memcpy(ring->data + at, text, first);
    memcpy(ring->data, text + first, length - first);
    ring->written += length;
}

// Two block copies: no per-byte division (slow on the ESP32), since
// callers hold a spinlock while this runs.
static void copy_out(const log_ring_t *ring, uint64_t from, char *out, size_t length)
{
    size_t at = (size_t)(from % ring->capacity);
    size_t first = ring->capacity - at < length ? ring->capacity - at : length;
    memcpy(out, ring->data + at, first);
    memcpy(out + first, ring->data, length - first);
}

size_t log_ring_read(const log_ring_t *ring, uint64_t since, char *out, size_t capacity, uint64_t *next,
                     bool *cut_out)
{
    uint64_t oldest = ring->written > ring->capacity ? ring->written - ring->capacity : 0;
    bool cut = since < oldest;
    uint64_t from = cut ? oldest : since > ring->written ? ring->written : since;
    if (ring->written - from > capacity) {
        from = ring->written - capacity; // Newest part when out is small.
        cut = true;
    }
    size_t length = (size_t)(ring->written - from);
    copy_out(ring, from, out, length);
    *next = ring->written;
    *cut_out = cut;
    return length;
}

size_t log_ring_whole_lines(char *text, size_t length)
{
    size_t skip = 0;
    while (skip < length && text[skip] != '\n') ++skip;
    skip = skip < length ? skip + 1 : length;
    memmove(text, text + skip, length - skip);
    return length - skip;
}
