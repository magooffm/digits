#include "voicemail_store.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *folder(bool heard) { return heard ? "saved" : "new"; }

bool vm_store_parse_name(const char *name, uint32_t *number, char caller[VM_STORE_CALLER_BYTES])
{
    const char *p = name;
    uint64_t value = 0;
    unsigned digits = 0;
    for (; *p >= '0' && *p <= '9'; ++p, ++digits) value = value * 10 + (unsigned)(*p - '0');
    if (digits < 6 || digits > 9 || *p++ != '-' || !value) return false;
    const char *start = p;
    while (*p >= '0' && *p <= '9') ++p;
    size_t length = (size_t)(p - start);
    if (!length || length >= VM_STORE_CALLER_BYTES || strcmp(p, ".opus")) return false;
    *number = (uint32_t)value;
    memcpy(caller, start, length);
    caller[length] = '\0';
    return true;
}

void vm_store_temp_path(const vm_store_t *store, char path[VM_STORE_PATH_BYTES])
{
    snprintf(path, VM_STORE_PATH_BYTES, "%s/recording.tmp", store->root);
}

bool vm_store_path(const vm_store_t *store, const vm_message_t *message, char path[VM_STORE_PATH_BYTES])
{
    int n = snprintf(path, VM_STORE_PATH_BYTES, "%s/%s/%06u-%s.opus", store->root,
                     folder(message->heard), (unsigned)message->number, message->caller);
    return n > 0 && n < VM_STORE_PATH_BYTES;
}

static int index_of(const vm_store_t *store, uint32_t number)
{
    for (unsigned i = 0; i < store->count; ++i)
        if (store->messages[i].number == number) return (int)i;
    return -1;
}

static void remove_at(vm_store_t *store, unsigned i)
{
    memmove(store->messages + i, store->messages + i + 1, (store->count - i - 1) * sizeof(vm_message_t));
    --store->count;
}

// Oldest heard message, else the oldest of all.
static int victim(const vm_store_t *store)
{
    for (unsigned i = 0; i < store->count; ++i)
        if (store->messages[i].heard) return (int)i;
    return store->count ? 0 : -1;
}

static bool delete_at(vm_store_t *store, unsigned i)
{
    char path[VM_STORE_PATH_BYTES];
    if (!vm_store_path(store, &store->messages[i], path) || (unlink(path) && errno != ENOENT)) return false;
    remove_at(store, i);
    return true;
}

static bool make_room(vm_store_t *store)
{
    while (store->count >= store->capacity) {
        int i = victim(store);
        if (i < 0 || !delete_at(store, (unsigned)i)) return false;
    }
    return true;
}

static void insert(vm_store_t *store, const vm_message_t *message)
{
    unsigned at = store->count;
    while (at && store->messages[at - 1].number > message->number) --at;
    memmove(store->messages + at + 1, store->messages + at, (store->count - at) * sizeof(vm_message_t));
    store->messages[at] = *message;
    ++store->count;
    if (message->number >= store->next_number) store->next_number = message->number + 1;
}

static void scan(vm_store_t *store, bool heard)
{
    char path[VM_STORE_PATH_BYTES];
    snprintf(path, sizeof(path), "%s/%s", store->root, folder(heard));
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        vm_message_t message = {.heard = heard};
        if (!vm_store_parse_name(entry->d_name, &message.number, message.caller) ||
            index_of(store, message.number) >= 0) continue;
        if (store->count >= store->capacity) {
            // Keep the newest: drop whichever of the index and this file goes first.
            int i = victim(store);
            bool newer = i >= 0 && ((store->messages[i].heard && !message.heard) ||
                                    message.number > store->messages[i].number);
            if (!newer || !delete_at(store, (unsigned)i)) {
                char old[VM_STORE_PATH_BYTES];
                if (vm_store_path(store, &message, old)) unlink(old);
                if (message.number >= store->next_number) store->next_number = message.number + 1;
                continue;
            }
        }
        insert(store, &message);
    }
    closedir(dir);
}

static bool make_folder(const char *path)
{
    return !mkdir(path, 0700) || errno == EEXIST;
}

bool vm_store_open(vm_store_t *store, const char *root, vm_message_t *storage, unsigned capacity)
{
    memset(store, 0, sizeof(*store));
    if (!capacity || strlen(root) >= sizeof(store->root)) return false;
    strcpy(store->root, root);
    store->messages = storage;
    store->capacity = capacity;
    store->next_number = 1;
    char path[VM_STORE_PATH_BYTES];
    if (!make_folder(root)) return false;
    for (int heard = 0; heard < 2; ++heard) {
        snprintf(path, sizeof(path), "%s/%s", root, folder(heard));
        if (!make_folder(path)) return false;
    }
    vm_store_temp_path(store, path);
    unlink(path);
    scan(store, true);  // Heard first: when over capacity they go first.
    scan(store, false);
    return true;
}

bool vm_store_commit(vm_store_t *store, const char *caller, vm_message_t *added)
{
    vm_message_t message = {.number = store->next_number, .heard = false};
    size_t length = caller ? strlen(caller) : 0;
    bool digits = length && length < VM_STORE_CALLER_BYTES;
    for (size_t i = 0; digits && i < length; ++i) digits = caller[i] >= '0' && caller[i] <= '9';
    strcpy(message.caller, digits ? caller : "0");
    char temp[VM_STORE_PATH_BYTES], path[VM_STORE_PATH_BYTES];
    vm_store_temp_path(store, temp);
    if (!vm_store_path(store, &message, path) || !make_room(store) || rename(temp, path)) return false;
    insert(store, &message);
    if (added) *added = message;
    return true;
}

const vm_message_t *vm_store_find(const vm_store_t *store, uint32_t number)
{
    int i = index_of(store, number);
    return i < 0 ? NULL : &store->messages[i];
}

const vm_message_t *vm_store_next_unheard(const vm_store_t *store)
{
    for (unsigned i = 0; i < store->count; ++i)
        if (!store->messages[i].heard) return &store->messages[i];
    return NULL;
}

unsigned vm_store_unheard(const vm_store_t *store)
{
    unsigned count = 0;
    for (unsigned i = 0; i < store->count; ++i) count += !store->messages[i].heard;
    return count;
}

bool vm_store_mark_heard(vm_store_t *store, uint32_t number)
{
    int i = index_of(store, number);
    if (i < 0) return false;
    vm_message_t *message = &store->messages[i];
    if (message->heard) return true;
    char from[VM_STORE_PATH_BYTES], to[VM_STORE_PATH_BYTES];
    vm_message_t heard = *message;
    heard.heard = true;
    if (!vm_store_path(store, message, from) || !vm_store_path(store, &heard, to) || rename(from, to))
        return false;
    message->heard = true;
    return true;
}

bool vm_store_delete(vm_store_t *store, uint32_t number)
{
    int i = index_of(store, number);
    return i >= 0 && delete_at(store, (unsigned)i);
}
