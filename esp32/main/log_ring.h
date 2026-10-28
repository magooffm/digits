#pragma once

// Byte ring of recent log text for the status page. Readers ask for
// everything after a position they got before; positions only grow, so a
// reader that fell behind just loses the overwritten part. Pure C, no
// locking (the caller serializes): tests/log_ring_test.c runs on the host.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t capacity;
    uint64_t written; // Total bytes ever appended.
} log_ring_t;

void log_ring_init(log_ring_t *ring, char *storage, size_t capacity);
void log_ring_append(log_ring_t *ring, const char *text, size_t length);

// Copies text after position since (or from the oldest kept byte) into out,
// at most capacity bytes (the newest ones). Returns the bytes copied; *next
// is the position to ask from next; *cut is true when older text was
// skipped, so the copy may begin mid-line. Two block copies, cheap under a
// spinlock.
size_t log_ring_read(const log_ring_t *ring, uint64_t since, char *out, size_t capacity, uint64_t *next,
                     bool *cut);
// Drops a partial first line in place (after a cut read); returns the new length.
size_t log_ring_whole_lines(char *text, size_t length);
