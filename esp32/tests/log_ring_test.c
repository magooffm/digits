// Host tests for log_ring.c.
#include "log_ring.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Read as the status page does: copy, then trim a cut first line.
static size_t read_lines(const log_ring_t *ring, uint64_t since, char *out, size_t capacity, uint64_t *next)
{
    bool cut;
    size_t n = log_ring_read(ring, since, out, capacity, next, &cut);
    return cut ? log_ring_whole_lines(out, n) : n;
}

int main(void)
{
    char storage[16], out[64];
    log_ring_t ring;
    uint64_t next;
    log_ring_init(&ring, storage, sizeof(storage));
    assert(read_lines(&ring, 0, out, sizeof(out), &next) == 0 && next == 0);
    log_ring_append(&ring, "one\ntwo\n", 8);
    size_t n = read_lines(&ring, 0, out, sizeof(out), &next);
    assert(n == 8 && !memcmp(out, "one\ntwo\n", 8) && next == 8);
    assert(read_lines(&ring, next, out, sizeof(out), &next) == 0 && next == 8); // Nothing new.
    log_ring_append(&ring, "three\nfour\n", 11); // Wraps; "one\n" and part of "two" are gone.
    n = read_lines(&ring, 8, out, sizeof(out), &next);
    assert(n == 11 && !memcmp(out, "three\nfour\n", 11) && next == 19);
    n = read_lines(&ring, 0, out, sizeof(out), &next); // Behind: from the oldest whole line.
    assert(n == 15 && !memcmp(out, "two\nthree\nfour\n", 15));
    n = read_lines(&ring, 0, out, 6, &next);           // Small buffer: newest whole lines only.
    assert(n == 5 && !memcmp(out, "four\n", 5));
    log_ring_append(&ring, "0123456789abcdefXYZ\n", 20); // Longer than the ring.
    n = read_lines(&ring, 0, out, sizeof(out), &next);
    assert(next == 39 && n == 0); // The kept 16 bytes hold no line start: nothing partial is shown.
    log_ring_append(&ring, "ok\n", 3);
    n = read_lines(&ring, 39, out, sizeof(out), &next);
    assert(n == 3 && !memcmp(out, "ok\n", 3));
    assert(read_lines(&ring, 1000, out, sizeof(out), &next) == 0 && next == 42); // Future position.
    puts("log_ring tests passed");
    return 0;
}
