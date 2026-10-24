#include "ringtone_pack.h"
#include "ringtones.h"
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

#define TAG "ringtone_pack"
#define MAGIC "DRTP"
#define VERSION 1

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version, count;
    uint32_t sample_rate, entries_offset;
} pack_header_t;

typedef struct __attribute__((packed)) {
    uint16_t id, reserved;
    uint32_t offset, samples;
    char name[RINGTONE_NAME_BYTES];
} pack_entry_t;

_Static_assert(sizeof(pack_header_t) == 16 && sizeof(pack_entry_t) == 64, "pack layout");

static const esp_partition_t *partition;

// source is the clip's byte offset in the partition.
static bool read_clip(uint32_t source, uint32_t first, int16_t *out, size_t count)
{
    if (!partition) return false;
    // Samples are little-endian PCM16, the ESP32-S3's native order.
    return esp_partition_read(partition, source + first * sizeof(int16_t), out,
                              count * sizeof(int16_t)) == ESP_OK;
}

esp_err_t digits_ringtone_pack_load(void)
{
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ringtones");
    if (!partition) {
        ESP_LOGW(TAG, "No ringtones partition; only built-in ringtones are available");
        return ESP_OK;
    }
    pack_header_t header;
    esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
    if (err != ESP_OK) return err;
    if (memcmp(header.magic, MAGIC, 4) || header.version != VERSION) {
        ESP_LOGI(TAG, "Ringtones partition holds no clip pack yet");
        return ESP_OK;
    }
    if (!header.sample_rate || header.sample_rate > RINGTONE_SAMPLE_RATE ||
        header.entries_offset < sizeof(header)) {
        ESP_LOGE(TAG, "Invalid clip pack header");
        return ESP_ERR_INVALID_STATE;
    }
    // Static: 2 KiB would overflow app_main's small stack. Boot-time only.
    static ringtones_clip_t clips[RINGTONE_MAX_CLIPS];
    unsigned count = 0;
    for (unsigned i = 0; i < header.count && count < RINGTONE_MAX_CLIPS; ++i) {
        pack_entry_t entry;
        err = esp_partition_read(partition, header.entries_offset + i * sizeof(entry), &entry, sizeof(entry));
        if (err != ESP_OK) return err;
        uint64_t end = (uint64_t)entry.offset + (uint64_t)entry.samples * sizeof(int16_t);
        if (!entry.samples || end > partition->size || entry.offset % 2) {
            ESP_LOGE(TAG, "Clip %u exceeds the partition; skipped", (unsigned)entry.id);
            continue;
        }
        clips[count] = (ringtones_clip_t){.id = entry.id, .samples = entry.samples, .source = entry.offset};
        memcpy(clips[count].name, entry.name, RINGTONE_NAME_BYTES);
        clips[count].name[RINGTONE_NAME_BYTES - 1] = '\0';
        ++count;
    }
    unsigned kept = ringtones_set_clips(clips, count, header.sample_rate, read_clip);
    if (kept != count)
        ESP_LOGW(TAG, "%u clip(s) skipped: duplicate or out-of-range numbers", count - kept);
    ESP_LOGI(TAG, "%u ringtone clip(s) at %u Hz", kept, (unsigned)header.sample_rate);
    return ESP_OK;
}
