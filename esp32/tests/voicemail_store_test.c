// Host tests for voicemail_store.c in a temporary directory.
#define _POSIX_C_SOURCE 200809L
#include "voicemail_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64];

static void touch(const char *relative)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs("x", file);
    fclose(file);
}

static bool exists(const char *relative)
{
    char path[160];
    struct stat info;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return stat(path, &info) == 0;
}

static void record(vm_store_t *store, const char *caller, vm_message_t *added)
{
    char temp[VM_STORE_PATH_BYTES];
    vm_store_temp_path(store, temp);
    FILE *file = fopen(temp, "wb");
    assert(file);
    fputs("audio", file);
    fclose(file);
    assert(vm_store_commit(store, caller, added));
}

static void test_names(void)
{
    uint32_t number;
    char caller[VM_STORE_CALLER_BYTES];
    assert(vm_store_parse_name("000012-1002.opus", &number, caller) && number == 12 && !strcmp(caller, "1002"));
    assert(vm_store_parse_name("1234567-5.opus", &number, caller) && number == 1234567);
    assert(!vm_store_parse_name("12-1002.opus", &number, caller));      // Too few digits.
    assert(!vm_store_parse_name("000000-1002.opus", &number, caller));  // Zero.
    assert(!vm_store_parse_name("000012-1002.mp3", &number, caller));
    assert(!vm_store_parse_name("000012-.opus", &number, caller));
    assert(!vm_store_parse_name("000012-10a2.opus", &number, caller));
    assert(!vm_store_parse_name("000012-1002.opus.bak", &number, caller));
    assert(!vm_store_parse_name("000012-12345678901234567890123456789012.opus", &number, caller));
}

static void test_lifecycle(void)
{
    vm_message_t storage[3];
    vm_store_t store;
    assert(vm_store_open(&store, root, storage, 3));
    assert(store.count == 0 && store.next_number == 1 && exists("new") && exists("saved"));
    vm_message_t a, b, c, d;
    record(&store, "1001", &a);
    record(&store, "1002", &b);
    assert(a.number == 1 && b.number == 2 && exists("new/000001-1001.opus"));
    assert(vm_store_unheard(&store) == 2 && vm_store_next_unheard(&store)->number == 1);
    assert(vm_store_mark_heard(&store, 1) && exists("saved/000001-1001.opus") && !exists("new/000001-1001.opus"));
    assert(vm_store_mark_heard(&store, 1)); // Idempotent.
    assert(!vm_store_mark_heard(&store, 99));
    assert(vm_store_unheard(&store) == 1 && vm_store_next_unheard(&store)->number == 2);
    record(&store, "", &c); // Invalid caller is stored as 0.
    assert(c.number == 3 && !strcmp(c.caller, "0"));
    // Full: the heard message goes first, even though it is not the oldest unheard.
    record(&store, "1004", &d);
    assert(store.count == 3 && !vm_store_find(&store, 1) && !exists("saved/000001-1001.opus"));
    // All unheard: the oldest goes.
    vm_message_t e;
    record(&store, "1005", &e);
    assert(e.number == 5 && store.count == 3 && !vm_store_find(&store, 2) && vm_store_find(&store, 3));
    char path[VM_STORE_PATH_BYTES];
    assert(vm_store_path(&store, vm_store_find(&store, 5), path) && strstr(path, "/new/000005-1005.opus"));
    assert(vm_store_delete(&store, 3) && !exists("new/000003-0.opus") && store.count == 2);
    assert(!vm_store_delete(&store, 3));
    // Without a recording to commit nothing changes.
    assert(!vm_store_commit(&store, "1006", NULL) && store.count == 2);
}

static void test_reopen(void)
{
    // Leftovers from test_lifecycle: new/000004-1004, new/000005-1005.
    touch("recording.tmp");
    touch("saved/000009-1001.opus");
    touch("new/notes.txt");
    vm_message_t storage[8];
    vm_store_t store;
    assert(vm_store_open(&store, root, storage, 8));
    assert(!exists("recording.tmp") && exists("new/notes.txt"));
    assert(store.count == 3 && store.next_number == 10);
    assert(storage[0].number == 4 && storage[1].number == 5 && storage[2].number == 9 && storage[2].heard);
    assert(vm_store_unheard(&store) == 2);
    // A smaller limit keeps unheard messages over heard ones, then the newest.
    touch("new/000011-1002.opus");
    vm_message_t small[2];
    assert(vm_store_open(&store, root, small, 2));
    assert(store.count == 2 && small[0].number == 5 && small[1].number == 11);
    assert(!exists("saved/000009-1001.opus") && !exists("new/000004-1004.opus"));
    assert(store.next_number == 12);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/vm-store-XXXXXX");
    assert(mkdtemp(root));
    strcat(root, "/vm");
    test_names();
    test_lifecycle();
    test_reopen();
    char command[160];
    snprintf(command, sizeof(command), "rm -rf '%.*s'", (int)(strlen(root) - 3), root);
    assert(system(command) == 0);
    puts("voicemail_store tests passed");
    return 0;
}
