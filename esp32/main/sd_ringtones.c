#include "sd_ringtones.h"
#include "sdkconfig.h"

#if CONFIG_DIGITS_SD_RINGTONES

#include "ring_tone.h"
#include "ringtones.h"
#include "sd_card.h"
#include "sd_ringtone_util.h"
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define TAG "sd_ringtones"
#define FOLDER DIGITS_SD_MOUNT_POINT "/ringtones"
#define GREETING_NAME "greeting.mp3"
#define GREETING_PATH DIGITS_SD_GREETING_PATH
#define GREETING_SLOT RINGTONE_LOCAL_SLOTS // After the ringtone positions.
#define SLOTS (RINGTONE_LOCAL_SLOTS + 1)
#define HEAD_BYTES 8192   // Enough to find the first frame and its Xing header.
#define READ_BYTES 4096
#define FIRST_OUTPUT_BYTES (1152 * 2 * 2) // One stereo MP3 frame; grows if needed.
#define BUFFER_FRAMES RINGTONE_SAMPLE_RATE // One second of 48 kHz mono in PSRAM.
#define SEND_WAIT_MS 20
#define ABANDON_MS 2000 // Nobody reading this long (call answered): stop decoding.
#define FALLBACK_PERIOD (6 * RINGTONE_SAMPLE_RATE) // Standard ring: 2 s on, 4 s off.
#define FALLBACK_ON (2 * RINGTONE_SAMPLE_RATE)
// esp_audio_codec 2.6.2 README: about 20 KiB of stack for decoding. The
// stack is in PSRAM (the task never writes flash): call audio needs the
// internal RAM.
#define STREAM_STACK_BYTES (24 * 1024)

typedef enum { STATE_STARTING, STATE_NO_CARD, STATE_NO_FOLDER, STATE_READY } state_t;
static int state = STATE_STARTING;
static char names[SLOTS][RINGTONE_NAME_BYTES];
static uint32_t audio_offsets[SLOTS]; // After the ID3v2 tag.
static uint32_t greeting_frames;      // 0: no usable greeting file.

static StreamBufferHandle_t pcm_buffer; // Decoder task -> players, 48 kHz mono.
static TaskHandle_t streamer;

// Players request a (re)start; the decoder acknowledges once the buffer only
// holds the new stream. Generations are written under request_mux.
static portMUX_TYPE request_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    unsigned slot;
    uint32_t generation;     // Requested.
    uint32_t ready;          // Buffer flushed and decoding this generation.
    uint32_t failed;         // This generation could not be played.
    uint32_t finished;       // This generation reached the end of the file.
} request;

// Player side. Only one player reads at a time: a ringtone rings before an
// answer and the greeting plays after it.
static struct {
    int slot;
    uint32_t generation, next;
    unsigned underruns;
} player = {.slot = -1};

static void fallback_ring(uint32_t position, int16_t *mono, size_t frames)
{
    int16_t stereo[2 * 128];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done < 128 ? frames - done : 128;
        uint32_t cycle = (position + (uint32_t)done) % FALLBACK_PERIOD;
        memset(stereo, 0, sizeof(stereo));
        if (cycle < FALLBACK_ON) ring_tone_render(stereo, cycle % RING_TONE_FRAMES, n);
        for (size_t i = 0; i < n; ++i) mono[done + i] = stereo[2 * i] / 2; // Incoming ring level.
        done += n;
    }
}

typedef enum { READ_PLAYING, READ_ENDED, READ_FAILED } read_t;

// Reads the next block of slot, restarting it when position is not where
// the last read stopped. ENDED: the file is done and the buffer drained.
static read_t read_slot(unsigned slot, uint32_t position, int16_t *mono, size_t frames)
{
    if ((int)slot != player.slot || position != player.next) {
        if (player.underruns)
            ESP_LOGW(TAG, "%s: %u buffer underrun(s)", names[player.slot], player.underruns);
        portENTER_CRITICAL(&request_mux);
        request.slot = slot;
        player.generation = ++request.generation;
        portEXIT_CRITICAL(&request_mux);
        player.slot = (int)slot;
        player.underruns = 0;
        xTaskNotifyGive(streamer);
    }
    player.next = position + (uint32_t)frames;
    portENTER_CRITICAL(&request_mux);
    bool ready = request.ready == player.generation;
    bool failed = request.failed == player.generation;
    bool finished = request.finished == player.generation;
    portEXIT_CRITICAL(&request_mux);
    if (failed) return READ_FAILED;
    size_t got = ready ? xStreamBufferReceive(pcm_buffer, mono, frames * sizeof(int16_t), 0) / sizeof(int16_t) : 0;
    if (got < frames) {
        memset(mono + got, 0, (frames - got) * sizeof(int16_t));
        if (finished) return READ_ENDED;
        // The first blocks after a start are expected gaps.
        if (ready && position > RINGTONE_SAMPLE_RATE / 10) ++player.underruns;
    }
    return READ_PLAYING;
}

static void stream(unsigned slot, uint32_t position, int16_t *mono, size_t frames)
{
    if (read_slot(slot, position, mono, frames) == READ_FAILED) fallback_ring(position, mono, frames);
}

// Decoder side: one stream at a time, owned by the streamer task.
typedef struct {
    FILE *file;
    esp_audio_simple_dec_handle_t decoder;
    esp_ae_rate_cvt_handle_t resampler;
    uint8_t *input, *output;
    uint32_t output_size;
    int16_t *resampled;
    uint32_t resampled_frames;
    uint32_t rate;
    unsigned channels;
    bool sounding; // Past the leading silence.
} decoding_t;

static void close_stream(decoding_t *d)
{
    if (d->file) fclose(d->file);
    if (d->decoder) esp_audio_simple_dec_close(d->decoder);
    if (d->resampler) esp_ae_rate_cvt_close(d->resampler);
    free(d->input);
    free(d->output);
    heap_caps_free(d->resampled);
    memset(d, 0, sizeof(*d));
}

static bool current(uint32_t generation)
{
    portENTER_CRITICAL(&request_mux);
    bool same = request.generation == generation;
    portEXIT_CRITICAL(&request_mux);
    return same;
}

// Sends mono 48 kHz samples, waiting for room. False if a newer request came
// or the player stopped reading. Every send and receive is a whole number of
// samples and the buffer size is even, so samples never split.
static bool deliver(const int16_t *pcm, size_t frames, uint32_t generation)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t left = frames * sizeof(int16_t);
    unsigned idle_ms = 0;
    while (left) {
        if (!current(generation) || idle_ms >= ABANDON_MS) return false;
        size_t sent = xStreamBufferSend(pcm_buffer, bytes, left, pdMS_TO_TICKS(SEND_WAIT_MS));
        idle_ms = sent ? 0 : idle_ms + SEND_WAIT_MS;
        bytes += sent;
        left -= sent;
    }
    return true;
}

// Conditions one decoded block in place and delivers it at 48 kHz.
static bool condition(decoding_t *d, const char *name, unsigned level, int16_t *pcm, size_t frames,
                      uint32_t generation)
{
    if (!d->rate) {
        esp_audio_simple_dec_info_t info = {0};
        esp_audio_simple_dec_get_info(d->decoder, &info);
        if (info.bits_per_sample != 16 || !info.channel || !info.sample_rate ||
            info.sample_rate > RINGTONE_SAMPLE_RATE) {
            ESP_LOGE(TAG, "%s: unsupported format (%u Hz, %u bit, %u channels)", name,
                     (unsigned)info.sample_rate, info.bits_per_sample, info.channel);
            return false;
        }
        d->rate = info.sample_rate;
        d->channels = info.channel;
        if (d->rate != RINGTONE_SAMPLE_RATE) {
            esp_ae_rate_cvt_cfg_t config = {
                .src_rate = d->rate, .dest_rate = RINGTONE_SAMPLE_RATE, .channel = 1,
                .bits_per_sample = 16, .complexity = 3, .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            if (esp_ae_rate_cvt_open(&config, &d->resampler) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "%s: cannot resample %u Hz", name, (unsigned)d->rate);
                return false;
            }
        }
    }
    frames /= d->channels;
    sd_ringtone_downmix(pcm, frames, d->channels, pcm);
    if (!d->sounding) {
        size_t skip = sd_ringtone_first_sound(pcm, frames);
        if (skip == frames) return true;
        d->sounding = true;
        pcm += skip;
        frames -= skip;
    }
    sd_ringtone_scale(pcm, frames, level);
    if (!d->resampler) return deliver(pcm, frames, generation);
    uint32_t needed = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(d->resampler, (uint32_t)frames, &needed);
    if (needed > d->resampled_frames) {
        heap_caps_free(d->resampled);
        d->resampled = heap_caps_malloc(needed * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        d->resampled_frames = d->resampled ? needed : 0;
        if (!d->resampled) return false;
    }
    uint32_t produced = d->resampled_frames;
    if (esp_ae_rate_cvt_process(d->resampler, pcm, (uint32_t)frames, d->resampled, &produced) != ESP_AE_ERR_OK)
        return false;
    return deliver(d->resampled, produced, generation);
}

// Decodes the whole file into the buffer. Returns false on a failure before
// any sound (the caller then falls back); a newer request just stops it.
static void slot_path(unsigned slot, char *path, size_t size)
{
    if (slot == GREETING_SLOT) snprintf(path, size, "%s", GREETING_PATH);
    else snprintf(path, size, "%s/%s", FOLDER, names[slot]);
}

static bool play(unsigned slot, uint32_t generation)
{
    const char *name = names[slot];
    char path[sizeof(FOLDER) + RINGTONE_NAME_BYTES + 1];
    slot_path(slot, path, sizeof(path));
    unsigned level = slot == GREETING_SLOT ? CONFIG_DIGITS_VOICEMAIL_GREETING_LEVEL
                                           : CONFIG_DIGITS_SD_RINGTONE_LEVEL;
    decoding_t d = {0};
    esp_audio_simple_dec_cfg_t config = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3};
    d.file = fopen(path, "rb");
    d.input = heap_caps_malloc(READ_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    d.output_size = FIRST_OUTPUT_BYTES;
    d.output = malloc(d.output_size);
    if (!d.file || !d.input || !d.output || fseek(d.file, (long)audio_offsets[slot], SEEK_SET) ||
        esp_audio_simple_dec_open(&config, &d.decoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "%s: cannot open for playing", name);
        close_stream(&d);
        return false;
    }
    bool going = true, sound = false;
    while (going) {
        size_t got = fread(d.input, 1, READ_BYTES, d.file);
        if (!got) break;
        esp_audio_simple_dec_raw_t raw = {.buffer = d.input, .len = got, .eos = got < READ_BYTES};
        // Unconsumed input must stay untouched until the decoder takes it.
        while (going && raw.len) {
            esp_audio_simple_dec_out_t frame = {.buffer = d.output, .len = d.output_size};
            esp_audio_err_t err = esp_audio_simple_dec_process(d.decoder, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *bigger = realloc(d.output, frame.needed_size);
                if (!bigger) {
                    going = false;
                    break;
                }
                d.output = bigger;
                d.output_size = frame.needed_size;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                // Trailing junk after good audio just ends the ringtone.
                if (!sound) ESP_LOGE(TAG, "%s: decoding failed (%d)", name, err);
                going = false;
                break;
            }
            if (frame.decoded_size) {
                if (!condition(&d, name, level, (int16_t *)frame.buffer,
                               frame.decoded_size / sizeof(int16_t), generation))
                    going = false;
                else
                    sound = sound || d.sounding;
            }
            if (!raw.consumed && !frame.decoded_size) break; // Never spin on a stuck parser.
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
        if (raw.eos) break;
    }
    close_stream(&d);
    return sound || !current(generation);
}

static void streamer_task(void *arg)
{
    (void)arg;
    uint32_t handled = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            portENTER_CRITICAL(&request_mux);
            uint32_t generation = request.generation;
            unsigned slot = request.slot;
            portEXIT_CRITICAL(&request_mux);
            if (generation == handled) break;
            handled = generation;
            // Players never block on the buffer, so a reset cannot fail on them.
            xStreamBufferReset(pcm_buffer);
            portENTER_CRITICAL(&request_mux);
            request.ready = generation;
            portEXIT_CRITICAL(&request_mux);
            bool ok = play(slot, generation);
            portENTER_CRITICAL(&request_mux);
            if (ok) request.finished = generation;
            else request.failed = generation;
            portEXIT_CRITICAL(&request_mux);
            if (!ok && slot == GREETING_SLOT) {
                ESP_LOGE(TAG, "%s unreadable: the built-in greeting plays instead", names[slot]);
                __atomic_store_n(&greeting_frames, 0, __ATOMIC_RELAXED);
            } else if (!ok) {
                ESP_LOGE(TAG, "%s unreadable: standard ring now, mapped ringtone for later calls",
                         names[slot]);
                ringtones_clear_local(slot);
            }
        }
    }
}

// Reads the file's length in 48 kHz frames from its headers; 0 if unusable.
static uint32_t measure(unsigned slot, uint32_t *rate_out)
{
    char path[sizeof(FOLDER) + RINGTONE_NAME_BYTES + 1];
    slot_path(slot, path, sizeof(path));
    struct stat info;
    uint8_t *head = malloc(HEAD_BYTES);
    FILE *file = head && !stat(path, &info) ? fopen(path, "rb") : NULL;
    uint32_t rate = 0;
    uint64_t samples = 0;
    bool ok = false;
    if (file) {
        size_t got = fread(head, 1, 10, file);
        uint32_t offset = sd_ringtone_id3_size(head, got);
        uint64_t end = (uint64_t)info.st_size;
        char tag[3];
        if (end >= 128 && !fseek(file, (long)(end - 128), SEEK_SET) && fread(tag, 1, 3, file) == 3 &&
            !memcmp(tag, "TAG", 3))
            end -= 128; // ID3v1 at the end.
        if (offset < end && !fseek(file, (long)offset, SEEK_SET)) {
            got = fread(head, 1, HEAD_BYTES, file);
            ok = sd_ringtone_mp3_length(head, got, end - offset, &rate, &samples);
            audio_offsets[slot] = offset;
        }
        fclose(file);
    }
    free(head);
    uint64_t frames = rate ? samples * RINGTONE_SAMPLE_RATE / rate : 0;
    *rate_out = rate;
    return ok && frames >= RINGTONE_SAMPLE_RATE / 10 && frames <= UINT32_MAX ? (uint32_t)frames : 0;
}

// Registers library slot from names[slot]; false if it is not a playable MP3.
static bool add(unsigned slot)
{
    const char *name = names[slot];
    uint32_t rate = 0, frames = measure(slot, &rate);
    if (!frames || !ringtones_set_local(slot, frames, name, stream)) {
        ESP_LOGE(TAG, "%s: not a playable MP3; not in the library", name);
        return false;
    }
    ESP_LOGI(TAG, "Song %u: %s, %u:%02u at %u Hz", slot + 1, name,
             (unsigned)(frames / RINGTONE_SAMPLE_RATE / 60), (unsigned)(frames / RINGTONE_SAMPLE_RATE % 60),
             (unsigned)rate);
    return true;
}

static int by_name(const void *a, const void *b) { return strcasecmp(a, b); }

// Every MP3 in the folder, alphabetically, under its own name.
static void scan(void)
{
    DIR *dir = opendir(FOLDER);
    if (!dir) {
        ESP_LOGI(TAG, "SD card has no ringtones folder");
        __atomic_store_n(&state, STATE_NO_FOLDER, __ATOMIC_RELAXED);
        return;
    }
    enum { FOUND_MAX = 2 * RINGTONE_LOCAL_SLOTS };
    char (*found)[RINGTONE_NAME_BYTES] = heap_caps_calloc(FOUND_MAX, RINGTONE_NAME_BYTES, MALLOC_CAP_SPIRAM);
    unsigned count = 0;
    struct dirent *entry;
    while (found && (entry = readdir(dir))) {
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.') continue;
        if (!sd_ringtone_name_valid(entry->d_name, RINGTONE_NAME_BYTES)) {
            ESP_LOGW(TAG, "Ignored %s: not an .mp3 name of up to %u characters", entry->d_name,
                     RINGTONE_NAME_BYTES - 1);
            continue;
        }
        if (count < FOUND_MAX) snprintf(found[count++], RINGTONE_NAME_BYTES, "%.*s", RINGTONE_NAME_BYTES - 1, entry->d_name);
    }
    closedir(dir);
    if (found) qsort(found, count, RINGTONE_NAME_BYTES, by_name);
    if (count > RINGTONE_LOCAL_SLOTS)
        ESP_LOGW(TAG, "%u songs on the card; the library keeps the first %u", count, RINGTONE_LOCAL_SLOTS);
    for (unsigned slot = 0; slot < count && slot < RINGTONE_LOCAL_SLOTS; ++slot) {
        memcpy(names[slot], found[slot], RINGTONE_NAME_BYTES);
        if (!add(slot)) names[slot][0] = '\0';
    }
    heap_caps_free(found);
    __atomic_store_n(&state, STATE_READY, __ATOMIC_RELAXED);
}

static void add_greeting(void)
{
    struct stat info;
    if (stat(GREETING_PATH, &info)) return;
    snprintf(names[GREETING_SLOT], RINGTONE_NAME_BYTES, "%s", GREETING_NAME);
    uint32_t rate = 0, frames = measure(GREETING_SLOT, &rate);
    if (!frames) {
        ESP_LOGE(TAG, "voicemail/%s: not a playable MP3; the built-in greeting plays", GREETING_NAME);
        return;
    }
    __atomic_store_n(&greeting_frames, frames, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "Answering machine greeting: voicemail/%s, %u:%02u at %u Hz", GREETING_NAME,
             (unsigned)(frames / RINGTONE_SAMPLE_RATE / 60), (unsigned)(frames / RINGTONE_SAMPLE_RATE % 60),
             (unsigned)rate);
}

// Stops whatever the streamer decodes, so a file can change under it.
static void cancel_stream(void)
{
    portENTER_CRITICAL(&request_mux);
    ++request.generation;
    portEXIT_CRITICAL(&request_mux);
    player.slot = -1;
    if (streamer) xTaskNotifyGive(streamer);
    vTaskDelay(pdMS_TO_TICKS(50)); // Let the decoder see it and close the file.
}

int digits_sd_ringtone_find(const char *name)
{
    for (unsigned slot = 0; slot < RINGTONE_LOCAL_SLOTS; ++slot)
        if (names[slot][0] && !strcasecmp(names[slot], name) && ringtones_valid(RINGTONE_FIRST_LOCAL + slot))
            return (int)(RINGTONE_FIRST_LOCAL + slot);
    return -1;
}

bool digits_sd_ringtone_file(unsigned id, char *out, size_t capacity)
{
    unsigned slot = id - RINGTONE_FIRST_LOCAL;
    if (id < RINGTONE_FIRST_LOCAL || slot >= RINGTONE_LOCAL_SLOTS || !names[slot][0]) return false;
    int n = snprintf(out, capacity, "%s/%s", FOLDER, names[slot]);
    return n > 0 && (size_t)n < capacity;
}

esp_err_t digits_sd_ringtone_added(const char *name, unsigned *id)
{
    if (!pcm_buffer || !sd_ringtone_name_valid(name, RINGTONE_NAME_BYTES)) return ESP_ERR_INVALID_ARG;
    int slot = -1;
    for (unsigned i = 0; i < RINGTONE_LOCAL_SLOTS && slot < 0; ++i)
        if (names[i][0] && !strcasecmp(names[i], name)) slot = (int)i; // Replaced: same number.
    for (unsigned i = 0; i < RINGTONE_LOCAL_SLOTS && slot < 0; ++i)
        if (!names[i][0]) slot = (int)i;
    if (slot < 0) return ESP_ERR_NO_MEM;
    cancel_stream();
    ringtones_clear_local((unsigned)slot);
    snprintf(names[slot], RINGTONE_NAME_BYTES, "%s", name);
    if (!add((unsigned)slot)) {
        names[slot][0] = '\0';
        return ESP_ERR_INVALID_RESPONSE;
    }
    __atomic_store_n(&state, STATE_READY, __ATOMIC_RELAXED);
    if (id) *id = RINGTONE_FIRST_LOCAL + (unsigned)slot;
    return ESP_OK;
}

esp_err_t digits_sd_ringtone_delete(unsigned id)
{
    char path[sizeof(FOLDER) + RINGTONE_NAME_BYTES + 1];
    if (!digits_sd_ringtone_file(id, path, sizeof(path))) return ESP_ERR_NOT_FOUND;
    unsigned slot = id - RINGTONE_FIRST_LOCAL;
    cancel_stream();
    ringtones_clear_local(slot);
    names[slot][0] = '\0';
    ESP_LOGI(TAG, "Deleted %s", path);
    return unlink(path) ? ESP_FAIL : ESP_OK;
}

esp_err_t digits_sd_greeting_reload(void)
{
    if (!pcm_buffer) return ESP_ERR_INVALID_STATE;
    cancel_stream();
    __atomic_store_n(&greeting_frames, 0, __ATOMIC_RELAXED);
    names[GREETING_SLOT][0] = '\0';
    struct stat info;
    if (stat(GREETING_PATH, &info)) return ESP_OK;
    add_greeting();
    return digits_sd_greeting_frames() ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

uint32_t digits_sd_greeting_frames(void)
{
    return __atomic_load_n(&greeting_frames, __ATOMIC_RELAXED);
}

vm_prompt_read_t digits_sd_greeting_read(uint32_t position, int16_t *mono, size_t frames)
{
    if (!digits_sd_greeting_frames()) return VM_PROMPT_FAILED;
    switch (read_slot(GREETING_SLOT, position, mono, frames)) {
    case READ_FAILED: return VM_PROMPT_FAILED;
    case READ_ENDED: return VM_PROMPT_DONE;
    default: return VM_PROMPT_MORE;
    }
}

esp_err_t digits_sd_ringtones_start(void)
{
    esp_err_t err = digits_sd_card_mount();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No SD card ringtones (%s)", esp_err_to_name(err));
        __atomic_store_n(&state, STATE_NO_CARD, __ATOMIC_RELAXED);
        return ESP_OK;
    }
    // The buffer and task exist before any slot is registered, so a player
    // can never reach stream() without them.
    static StaticStreamBuffer_t buffer_state;
    uint8_t *storage = heap_caps_malloc(BUFFER_FRAMES * sizeof(int16_t) + 1, MALLOC_CAP_SPIRAM);
    if (storage)
        pcm_buffer = xStreamBufferCreateStatic(BUFFER_FRAMES * sizeof(int16_t), 1, storage, &buffer_state);
    if (!pcm_buffer || xTaskCreateWithCaps(streamer_task, "digits_sd_ring", STREAM_STACK_BYTES, NULL, 3,
                                           &streamer, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        __atomic_store_n(&state, STATE_NO_CARD, __ATOMIC_RELAXED);
        return ESP_ERR_NO_MEM;
    }
    esp_mp3_dec_register(); // MP3 only; every codec would add about 600 KB.
    scan();
    add_greeting();
    return ESP_OK;
}

const char *digits_sd_ringtones_status(void)
{
    switch (__atomic_load_n(&state, __ATOMIC_RELAXED)) {
    case STATE_STARTING: return "starting";
    case STATE_NO_CARD: return "no card";
    case STATE_NO_FOLDER: return "no ringtones folder";
    default: return "ready";
    }
}

#else

esp_err_t digits_sd_ringtones_start(void) { return ESP_OK; }
const char *digits_sd_ringtones_status(void) { return "disabled"; }
int digits_sd_ringtone_find(const char *name) { return -1; }
bool digits_sd_ringtone_file(unsigned id, char *out, size_t capacity) { return false; }
esp_err_t digits_sd_ringtone_added(const char *name, unsigned *id) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t digits_sd_ringtone_delete(unsigned id) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t digits_sd_greeting_reload(void) { return ESP_ERR_NOT_SUPPORTED; }
uint32_t digits_sd_greeting_frames(void) { return 0; }

vm_prompt_read_t digits_sd_greeting_read(uint32_t position, int16_t *mono, size_t frames)
{
    (void)position; (void)mono; (void)frames;
    return VM_PROMPT_FAILED;
}

#endif
