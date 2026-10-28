#pragma once

// Answering-machine messages on the SD card. Plain POSIX file calls, no
// ESP-IDF and no locking (the caller serializes), so
// tests/voicemail_store_test.c runs on the host.
//
//   <root>/new/000012-1002.opus     unheard, from 1002
//   <root>/saved/000011-1001.opus   heard
//   <root>/recording.tmp            message being recorded
//
// Numbers increase with every message and order them. Hearing a message
// moves it to saved/. Deleting a file on a computer simply removes it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define VM_STORE_CALLER_BYTES 32
#define VM_STORE_PATH_BYTES 128

typedef struct {
    uint32_t number;
    char caller[VM_STORE_CALLER_BYTES];
    bool heard;
    uint32_t tenths; // Length cache for the caller; 0 when not known yet.
} vm_message_t;

typedef struct {
    char root[48];
    vm_message_t *messages; // Ascending by number.
    unsigned count, capacity;
    uint32_t next_number;
} vm_store_t;

// Creates the folders, deletes an unfinished recording, indexes existing
// messages and removes the oldest beyond capacity (the configured maximum).
bool vm_store_open(vm_store_t *store, const char *root, vm_message_t *storage, unsigned capacity);

void vm_store_temp_path(const vm_store_t *store, char path[VM_STORE_PATH_BYTES]);
bool vm_store_path(const vm_store_t *store, const vm_message_t *message, char path[VM_STORE_PATH_BYTES]);

// Moves the finished recording into new/. When full, removes the oldest
// heard message, or the oldest unheard one if all are unheard.
bool vm_store_commit(vm_store_t *store, const char *caller, vm_message_t *added);

const vm_message_t *vm_store_find(const vm_store_t *store, uint32_t number);
// Oldest unheard message, or NULL.
const vm_message_t *vm_store_next_unheard(const vm_store_t *store);
unsigned vm_store_unheard(const vm_store_t *store);

bool vm_store_mark_heard(vm_store_t *store, uint32_t number);
bool vm_store_delete(vm_store_t *store, uint32_t number);

// "000012-1002.opus" -> 12, "1002". False for any other name.
bool vm_store_parse_name(const char *name, uint32_t *number, char caller[VM_STORE_CALLER_BYTES]);
