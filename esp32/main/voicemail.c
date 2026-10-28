#include "voicemail.h"
#include "sdkconfig.h"

#if CONFIG_DIGITS_VOICEMAIL

#include "audio_board.h"
#include "call_audio_jitter.h"
#include "ogg_opus.h"
#include "sd_card.h"
#include "sd_ringtones.h"
#include "voicemail_prompt.h"
#include "voicemail_store.h"
#include "webrtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_opus_dec.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "voicemail"
#define ROOT DIGITS_SD_MOUNT_POINT "/voicemail"
#define CAPACITY CONFIG_DIGITS_VOICEMAIL_MAX_MESSAGES
#define QUEUE_LENGTH 64     // About 1.3 s of packets while the card is slow.
#define CONTROL_RESERVE 2   // Queue slots kept free for the end and next begin.
#define MAX_SAMPLES ((uint64_t)CONFIG_DIGITS_VOICEMAIL_MAX_SECONDS * OGG_OPUS_RATE)
#define MIN_SAMPLES (OGG_OPUS_RATE / 2) // Shorter: the caller hung up at the beep.
// Both stacks live in PSRAM: neither task writes flash, and internal RAM is
// needed for the call audio workers' stacks.
#define RECORDER_STACK_BYTES (6 * 1024)
#define PLAYER_STACK_BYTES (24 * 1024) // Opus decoder, as in call_audio.c.
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define WRITE_TIMEOUT_MS 50
#define TAIL_BYTES (OGG_WRITER_BODY + OGG_HEADER_MAX)
#define CLOCK_VALID_YEAR 2025

extern const uint8_t greeting_wav_start[] asm("_binary_voicemail_greeting_wav_start");
extern const uint8_t greeting_wav_end[] asm("_binary_voicemail_greeting_wav_end");

typedef enum { ITEM_BEGIN, ITEM_PACKET, ITEM_END } item_kind_t;
typedef struct {
    uint8_t kind;
    uint16_t size;
    uint32_t session;
    uint8_t data[CALL_AUDIO_OPUS_MAX_BYTES]; // Packet, or the caller for BEGIN.
} item_t;

// Message store: recorder, player and console tasks, under store_lock.
static SemaphoreHandle_t store_lock;
static vm_store_t store;
static int unheard = -1;
static bool ready;

static const uint8_t *builtin_pcm;
static size_t builtin_frames;
static uint32_t builtin_rate;

// A greeting recorded on the phone (voicemail/greeting.wav), kept in PSRAM.
// Swapped only while no call or playback uses it (callers check the line).
static uint8_t *recorded_wav;
static const uint8_t *recorded_pcm;
static size_t recorded_samples;
static uint32_t recorded_rate;
#define MAX_RECORDED_BYTES (4 * 1024 * 1024)

// Recorded greeting first, then an uploaded MP3, then the built-in one.
static void start_greeting(vm_prompt_t *p)
{
    if (recorded_pcm) vm_prompt_start(p, recorded_pcm, recorded_samples, recorded_rate, NULL);
    else vm_prompt_start(p, builtin_pcm, builtin_frames, builtin_rate,
                         digits_sd_greeting_frames() ? digits_sd_greeting_read : NULL);
}

const char *digits_voicemail_greeting_kind(void)
{
    return recorded_pcm ? "recorded" : digits_sd_greeting_frames() ? "uploaded" : "built-in";
}

unsigned digits_voicemail_greeting_seconds(void)
{
    if (recorded_pcm) return (unsigned)(recorded_samples / recorded_rate);
    if (digits_sd_greeting_frames()) return (unsigned)(digits_sd_greeting_frames() / VM_PROMPT_RATE);
    return (unsigned)(builtin_frames / (builtin_rate ? builtin_rate : VM_PROMPT_RATE));
}

esp_err_t digits_voicemail_greeting_reload(void)
{
    digits_voicemail_stop();
    uint8_t *old = recorded_wav;
    recorded_wav = NULL;
    recorded_pcm = NULL;
    recorded_samples = 0;
    free(old);
    struct stat info;
    if (stat(DIGITS_VOICEMAIL_GREETING_WAV, &info)) return ESP_OK; // None: the next choice plays.
    if (info.st_size <= 44 || info.st_size > MAX_RECORDED_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *wav = heap_caps_malloc((size_t)info.st_size, PSRAM);
    FILE *file = wav ? fopen(DIGITS_VOICEMAIL_GREETING_WAV, "rb") : NULL;
    bool read = file && fread(wav, 1, (size_t)info.st_size, file) == (size_t)info.st_size;
    if (file) fclose(file);
    const uint8_t *pcm;
    size_t samples;
    uint32_t rate;
    if (!read || !vm_prompt_wav(wav, (size_t)info.st_size, &pcm, &samples, &rate) || !samples) {
        free(wav);
        ESP_LOGE(TAG, "voicemail/greeting.wav is not a usable recording");
        return ESP_ERR_INVALID_RESPONSE;
    }
    recorded_samples = samples;
    recorded_rate = rate;
    recorded_pcm = pcm;
    recorded_wav = wav;
    ESP_LOGI(TAG, "Recorded greeting: %u.%u s at %u Hz", (unsigned)(samples / rate),
             (unsigned)(samples % rate * 10 / rate), (unsigned)rate);
    return ESP_OK;
}

// Answering session. id, scratch and dropped belong to the peer task;
// gate is set by the encoder task, full_session by the recorder.
static QueueHandle_t queue;
static struct {
    uint32_t id, next_id;
    bool gate;
    uint32_t full_session;
    uint32_t dropped;
    item_t *scratch; // PSRAM.
} session;
static vm_prompt_t prompt; // Encoder task once the call audio started.

// Recorder task.
static struct {
    item_t *item; // PSRAM.
    ogg_writer_t *writer;
    uint32_t session;
    char caller[VM_STORE_CALLER_BYTES];
    FILE *file;
    uint64_t samples;
    uint32_t invalid;
    bool failed;
} rec;

// Player requests.
typedef enum { PLAY_NONE, PLAY_UNHEARD, PLAY_ONE, PLAY_GREETING } play_kind_t;
static portMUX_TYPE play_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    play_kind_t kind;
    uint32_t number;
    bool stop;
    bool busy;
} play;
static TaskHandle_t player;

static void lock(void) { xSemaphoreTake(store_lock, portMAX_DELAY); }
static void unlock(void)
{
    __atomic_store_n(&unheard, (int)vm_store_unheard(&store), __ATOMIC_RELAXED);
    xSemaphoreGive(store_lock);
}

bool digits_voicemail_ready(void) { return __atomic_load_n(&ready, __ATOMIC_RELAXED); }
int digits_voicemail_unheard(void) { return __atomic_load_n(&unheard, __ATOMIC_RELAXED); }

// ---- Call hooks -----------------------------------------------------------

static void fill(int16_t *mono, size_t frames)
{
    if (vm_prompt_render(&prompt, mono, frames)) __atomic_store_n(&session.gate, true, __ATOMIC_RELEASE);
}

static bool send_item(item_kind_t kind, const uint8_t *data, size_t size)
{
    session.scratch->kind = (uint8_t)kind;
    session.scratch->session = session.id;
    session.scratch->size = (uint16_t)size;
    if (size) memcpy(session.scratch->data, data, size);
    return xQueueSend(queue, session.scratch, 0) == pdTRUE;
}

static void record(const uint8_t *packet, size_t size)
{
    if (!session.id || !__atomic_load_n(&session.gate, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&session.full_session, __ATOMIC_RELAXED) == session.id)
        return;
    if (size > CALL_AUDIO_OPUS_MAX_BYTES || uxQueueSpacesAvailable(queue) <= CONTROL_RESERVE ||
        !send_item(ITEM_PACKET, packet, size))
        ++session.dropped;
}

static bool greeting_over(void) { return __atomic_load_n(&session.gate, __ATOMIC_ACQUIRE); }

static digits_call_audio_unattended_t hooks = {.fill = fill, .record = record, .greeting_done = greeting_over};

// Classic answering machine: hear the greeting and the caller on the speaker.
#define LISTEN_NAMESPACE "digits_vm"
static bool hear_greeting = true, hear_caller = true;

static void load_listen(void)
{
    nvs_handle_t nvs;
    if (nvs_open(LISTEN_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t value;
    if (nvs_get_u8(nvs, "hear_greeting", &value) == ESP_OK) hear_greeting = value;
    if (nvs_get_u8(nvs, "hear_caller", &value) == ESP_OK) hear_caller = value;
    nvs_close(nvs);
}

void digits_voicemail_listen(bool *greeting, bool *caller)
{
    if (greeting) *greeting = __atomic_load_n(&hear_greeting, __ATOMIC_RELAXED);
    if (caller) *caller = __atomic_load_n(&hear_caller, __ATOMIC_RELAXED);
}

esp_err_t digits_voicemail_set_listen(bool greeting, bool caller)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(LISTEN_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "hear_greeting", greeting);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "hear_caller", caller);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    __atomic_store_n(&hear_greeting, greeting, __ATOMIC_RELAXED);
    __atomic_store_n(&hear_caller, caller, __ATOMIC_RELAXED);
    return ESP_OK;
}

const digits_call_audio_unattended_t *digits_voicemail_begin(const char *caller)
{
    if (!digits_voicemail_ready() || session.id) return NULL;
    digits_voicemail_stop();
    if (++session.next_id == 0) ++session.next_id;
    session.id = session.next_id;
    session.dropped = 0;
    __atomic_store_n(&session.gate, false, __ATOMIC_RELEASE);
    // No encoder runs yet: the previous call's workers were reaped first.
    start_greeting(&prompt);
    digits_voicemail_listen(&hooks.hear_greeting, &hooks.hear_caller); // For this call.
    size_t length = caller ? strnlen(caller, VM_STORE_CALLER_BYTES - 1) : 0;
    uint8_t name[VM_STORE_CALLER_BYTES] = {0};
    memcpy(name, caller, length);
    if (!send_item(ITEM_BEGIN, name, sizeof(name))) {
        session.id = 0;
        return NULL;
    }
    ESP_LOGI(TAG, "Answering call from %s with the %s greeting", caller ? caller : "unknown",
             digits_voicemail_greeting_kind());
    return &hooks;
}

void digits_voicemail_end(void)
{
    if (!session.id) return;
    if (!send_item(ITEM_END, NULL, 0)) ESP_LOGE(TAG, "Recorder queue full at the end of a message");
    if (session.dropped) ESP_LOGW(TAG, "%u packets dropped: the SD card was too slow", (unsigned)session.dropped);
    session.id = 0;
}

bool digits_voicemail_full(void)
{
    return session.id && __atomic_load_n(&session.full_session, __ATOMIC_RELAXED) == session.id;
}

// ---- Recorder -------------------------------------------------------------

static void close_recording(bool keep)
{
    char temp[VM_STORE_PATH_BYTES];
    vm_store_temp_path(&store, temp);
    if (!rec.file) {
        if (rec.failed) ESP_LOGE(TAG, "The message from %s could not be recorded", rec.caller);
        else ESP_LOGI(TAG, "Call from %s ended before a message was left", rec.caller);
        return;
    }
    keep = keep && !rec.failed && rec.samples >= MIN_SAMPLES && ogg_writer_finish(rec.writer) &&
           fsync(fileno(rec.file)) == 0;
    bool closed = fclose(rec.file) == 0;
    rec.file = NULL;
    unsigned tenths = (unsigned)(rec.samples * 10 / OGG_OPUS_RATE);
    if (!keep || !closed) {
        unlink(temp);
        if (rec.failed || !closed) ESP_LOGE(TAG, "Writing the message from %s failed; discarded", rec.caller);
        else ESP_LOGI(TAG, "Message from %s too short (%u.%u s); discarded", rec.caller, tenths / 10, tenths % 10);
        return;
    }
    vm_message_t added;
    lock();
    bool committed = vm_store_commit(&store, rec.caller, &added);
    if (committed) {
        // Known now; saves reading the file's end for the message list.
        for (unsigned i = 0; i < store.count; ++i)
            if (store.messages[i].number == added.number) store.messages[i].tenths = tenths ? tenths : 1;
    }
    unlock();
    if (!committed) {
        unlink(temp);
        ESP_LOGE(TAG, "Saving the message from %s failed", rec.caller);
        return;
    }
    ESP_LOGI(TAG, "New message %u from %s, %u.%u s; %d unheard", (unsigned)added.number, added.caller,
             tenths / 10, tenths % 10, digits_voicemail_unheard());
}

static void open_recording(void)
{
    char temp[VM_STORE_PATH_BYTES];
    vm_store_temp_path(&store, temp);
    rec.file = fopen(temp, "wb");
    char comment[64];
    snprintf(comment, sizeof(comment), "TITLE=Message from %s", rec.caller);
    if (!rec.file || !ogg_writer_start(rec.writer, rec.file, esp_random(), comment)) {
        ESP_LOGE(TAG, "Cannot create %s", temp);
        rec.failed = true;
    }
}

static void recorder_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xQueueReceive(queue, rec.item, portMAX_DELAY) != pdTRUE) continue;
        item_t *item = rec.item;
        switch (item->kind) {
        case ITEM_BEGIN:
            if (rec.session) close_recording(true); // Only if an end was lost.
            rec.session = item->session;
            memcpy(rec.caller, item->data, VM_STORE_CALLER_BYTES);
            rec.caller[VM_STORE_CALLER_BYTES - 1] = '\0';
            if (!rec.caller[0]) strcpy(rec.caller, "0");
            rec.samples = 0;
            rec.invalid = 0;
            rec.failed = false;
            break;
        case ITEM_PACKET: {
            if (item->session != rec.session || rec.failed || rec.samples >= MAX_SAMPLES) break;
            unsigned samples = opus_packet_samples(item->data, item->size);
            if (!samples) {
                ++rec.invalid;
                break;
            }
            if (!rec.file) open_recording();
            if (rec.failed) break;
            if (!ogg_writer_add(rec.writer, item->data, item->size, samples)) {
                rec.failed = true;
                break;
            }
            rec.samples += samples;
            if (rec.samples >= MAX_SAMPLES) {
                ESP_LOGI(TAG, "Message from %s reached %u s; hanging up", rec.caller,
                         (unsigned)CONFIG_DIGITS_VOICEMAIL_MAX_SECONDS);
                __atomic_store_n(&session.full_session, rec.session, __ATOMIC_RELAXED);
            }
            break;
        }
        case ITEM_END:
            if (item->session != rec.session) break;
            close_recording(true);
            rec.session = 0;
            break;
        }
    }
}

// ---- Player ---------------------------------------------------------------

static bool keep_playing(void)
{
    portENTER_CRITICAL(&play_mux);
    bool stop = play.stop;
    portEXIT_CRITICAL(&play_mux);
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    return !stop && call.phase == DIGITS_CALL_IDLE;
}

typedef struct {
    int16_t mono[OGG_OPUS_MAX_PACKET_SAMPLES];
    int16_t stereo[DIGITS_AUDIO_DMA_FRAMES * 2];
    uint8_t body[OGG_READER_BODY];
} player_buffers_t;

static bool speak(player_buffers_t *b, const int16_t *mono, size_t frames)
{
    while (frames) {
        size_t n = frames < DIGITS_AUDIO_DMA_FRAMES ? frames : DIGITS_AUDIO_DMA_FRAMES;
        call_audio_pcm_stereo(mono, b->stereo, n);
        size_t written = 0;
        if (!keep_playing() ||
            digits_audio_speaker_write(b->stereo, n * 2 * sizeof(int16_t), &written, WRITE_TIMEOUT_MS) != ESP_OK ||
            written != n * 2 * sizeof(int16_t))
            return false;
        mono += n;
        frames -= n;
    }
    return true;
}

// Plays one message; true if it was heard to the end.
static bool play_message(player_buffers_t *b, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }
    void *decoder = NULL;
    esp_opus_dec_cfg_t config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    config.sample_rate = OGG_OPUS_RATE;
    config.channel = 1;
    config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID; // Any packet length.
    config.self_delimited = false;
    if (esp_opus_dec_open(&config, sizeof(config), &decoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Cannot create the Opus decoder");
        fclose(file);
        return false;
    }
    ogg_reader_t reader;
    ogg_reader_start(&reader, file, b->body);
    const uint8_t *packet;
    size_t size;
    bool playing = true;
    uint32_t skip = UINT32_MAX;
    while (playing && (size = ogg_reader_next(&reader, &packet))) {
        if (skip == UINT32_MAX) skip = reader.preskip;
        esp_audio_dec_in_raw_t in = {.buffer = (uint8_t *)packet, .len = size};
        esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)b->mono, .len = sizeof(b->mono)};
        esp_audio_dec_info_t info = {0};
        if (esp_opus_dec_decode(decoder, &in, &out, &info) != ESP_AUDIO_ERR_OK) continue;
        size_t frames = out.decoded_size / sizeof(int16_t);
        size_t dropped = frames < skip ? frames : skip;
        skip -= (uint32_t)dropped;
        playing = speak(b, b->mono + dropped, frames - dropped);
    }
    esp_opus_dec_close(decoder);
    fclose(file);
    return playing;
}

static bool play_greeting(player_buffers_t *b)
{
    vm_prompt_t local;
    start_greeting(&local);
    bool open = false;
    while (!open) {
        open = vm_prompt_render(&local, b->mono, 960);
        if (!speak(b, b->mono, 960)) return false;
    }
    return true;
}

// Next message for a request: its path and number, or false when done.
static bool next_message(play_kind_t kind, uint32_t number, uint32_t played, char path[VM_STORE_PATH_BYTES],
                         vm_message_t *out)
{
    lock();
    const vm_message_t *m = kind == PLAY_ONE ? (played ? NULL : vm_store_find(&store, number))
                                             : vm_store_next_unheard(&store);
    bool found = m && vm_store_path(&store, m, path);
    if (found) *out = *m;
    unlock();
    return found;
}

static void player_task(void *arg)
{
    (void)arg;
    player_buffers_t *b = heap_caps_malloc(sizeof(*b), PSRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&play_mux);
        play_kind_t kind = play.kind;
        uint32_t number = play.number;
        play.kind = PLAY_NONE;
        play.stop = false;
        play.busy = kind != PLAY_NONE;
        portEXIT_CRITICAL(&play_mux);
        if (kind == PLAY_NONE) continue;
        if (!b || !digits_audio_speaker_acquire(0)) {
            ESP_LOGW(TAG, "Speaker busy; nothing played");
        } else if (digits_audio_speaker_start() == ESP_OK) {
            if (kind == PLAY_GREETING) {
                ESP_LOGI(TAG, "Playing the %s greeting", digits_voicemail_greeting_kind());
                play_greeting(b);
            } else {
                char path[VM_STORE_PATH_BYTES];
                vm_message_t m;
                uint32_t played = 0;
                while (keep_playing() && next_message(kind, number, played, path, &m)) {
                    ESP_LOGI(TAG, "Playing message %u from %s", (unsigned)m.number, m.caller);
                    ++played;
                    if (!play_message(b, path)) break;
                    lock();
                    vm_store_mark_heard(&store, m.number);
                    unlock();
                    if (kind == PLAY_ONE) break;
                    int16_t gap[960] = {0};
                    speak(b, gap, 960);
                }
                if (!played) ESP_LOGI(TAG, "%s", kind == PLAY_ONE ? "No such message" : "No new messages");
            }
            digits_audio_speaker_silence();
            digits_audio_speaker_release();
        } else {
            digits_audio_speaker_release();
        }
        portENTER_CRITICAL(&play_mux);
        play.busy = false;
        portEXIT_CRITICAL(&play_mux);
    }
}

static esp_err_t request(play_kind_t kind, uint32_t number)
{
    if (!digits_voicemail_ready()) return ESP_ERR_INVALID_STATE;
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    if (call.phase != DIGITS_CALL_IDLE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&play_mux);
    bool busy = play.busy;
    if (busy) play.stop = true;
    play.kind = kind;
    play.number = number;
    portEXIT_CRITICAL(&play_mux);
    xTaskNotifyGive(player);
    return ESP_OK;
}

esp_err_t digits_voicemail_play(uint32_t number) { return request(number ? PLAY_ONE : PLAY_UNHEARD, number); }
esp_err_t digits_voicemail_play_greeting(void) { return request(PLAY_GREETING, 0); }

void digits_voicemail_stop(void)
{
    portENTER_CRITICAL(&play_mux);
    play.stop = play.busy;
    play.kind = PLAY_NONE;
    portEXIT_CRITICAL(&play_mux);
}

// ---- Start ----------------------------------------------------------------

esp_err_t digits_voicemail_start(void)
{
    if (!vm_prompt_wav(greeting_wav_start, (size_t)(greeting_wav_end - greeting_wav_start), &builtin_pcm,
                       &builtin_frames, &builtin_rate))
        ESP_LOGE(TAG, "Built-in greeting unreadable; only the beep plays");
    if (!digits_sd_card_mounted()) {
        ESP_LOGI(TAG, "No SD card: the answering machine is off");
        return ESP_OK;
    }
    store_lock = xSemaphoreCreateMutex();
    vm_message_t *messages = heap_caps_calloc(CAPACITY, sizeof(vm_message_t), PSRAM);
    rec.writer = heap_caps_malloc(sizeof(ogg_writer_t), PSRAM);
    rec.item = heap_caps_malloc(sizeof(item_t), PSRAM);
    session.scratch = heap_caps_malloc(sizeof(item_t), PSRAM);
    static StaticQueue_t queue_state;
    uint8_t *queue_storage = heap_caps_malloc(QUEUE_LENGTH * sizeof(item_t), PSRAM);
    if (!store_lock || !messages || !rec.writer || !rec.item || !session.scratch || !queue_storage)
        return ESP_ERR_NO_MEM;
    queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(item_t), queue_storage, &queue_state);
    if (!vm_store_open(&store, ROOT, messages, CAPACITY)) {
        ESP_LOGE(TAG, "Cannot use %s on the SD card: the answering machine is off", ROOT);
        return ESP_FAIL;
    }
    __atomic_store_n(&unheard, (int)vm_store_unheard(&store), __ATOMIC_RELAXED);
    if (xTaskCreateWithCaps(recorder_task, "digits_vm_rec", RECORDER_STACK_BYTES, NULL, 4, NULL, PSRAM) != pdPASS ||
        xTaskCreateWithCaps(player_task, "digits_vm_play", PLAYER_STACK_BYTES, NULL, 3, &player, PSRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    digits_voicemail_greeting_reload();
    load_listen();
    __atomic_store_n(&ready, true, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "Ready: %u messages, %d unheard, %s greeting", store.count, digits_voicemail_unheard(),
             digits_voicemail_greeting_kind());
    return ESP_OK;
}

// ---- Console --------------------------------------------------------------

// Date and length of one message; tail is a TAIL_BYTES scratch buffer.
static void message_info(const vm_message_t *m, uint8_t *tail, digits_voicemail_message_t *out)
{
    memset(out, 0, sizeof(*out));
    out->number = m->number;
    snprintf(out->caller, sizeof(out->caller), "%s", m->caller);
    out->heard = m->heard;
    out->tenths = m->tenths;
    char path[VM_STORE_PATH_BYTES];
    struct stat info;
    if (!vm_store_path(&store, m, path) || stat(path, &info)) return;
    struct tm local;
    localtime_r(&info.st_mtime, &local);
    if (local.tm_year + 1900 >= CLOCK_VALID_YEAR) out->recorded = (int64_t)info.st_mtime;
    FILE *file = tail && !out->tenths ? fopen(path, "rb") : NULL; // Reads the end once, then cached.
    if (!file) return;
    long size = info.st_size < TAIL_BYTES ? (long)info.st_size : TAIL_BYTES;
    uint64_t granule = 0;
    if (!fseek(file, -size, SEEK_END) && fread(tail, 1, (size_t)size, file) == (size_t)size &&
        ogg_last_granule(tail, (size_t)size, &granule) && granule > OGG_OPUS_PRESKIP)
        out->tenths = (uint32_t)((granule - OGG_OPUS_PRESKIP) * 10 / OGG_OPUS_RATE);
    fclose(file);
}

static void describe(const vm_message_t *m, uint8_t *tail)
{
    digits_voicemail_message_t info;
    message_info(m, tail, &info);
    char when[24] = "date unknown";
    if (info.recorded) {
        time_t t = (time_t)info.recorded;
        struct tm local;
        localtime_r(&t, &local);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
    }
    printf("%6u  %-4s  from %-12s %s  %u.%u s\n", (unsigned)m->number, m->heard ? "" : "new", m->caller, when,
           (unsigned)info.tenths / 10, (unsigned)info.tenths % 10);
}

unsigned digits_voicemail_list(digits_voicemail_message_t *out, unsigned max)
{
    if (!digits_voicemail_ready()) return 0;
    uint8_t *tail = heap_caps_malloc(TAIL_BYTES, PSRAM);
    lock();
    unsigned count = store.count;
    for (unsigned i = 0; i < count && i < max; ++i) {
        message_info(&store.messages[i], tail, &out[i]);
        store.messages[i].tenths = out[i].tenths;
    }
    unlock();
    free(tail);
    return count;
}

bool digits_voicemail_file(uint32_t number, char *path, size_t capacity)
{
    if (!digits_voicemail_ready() || capacity < VM_STORE_PATH_BYTES) return false;
    lock();
    const vm_message_t *m = vm_store_find(&store, number);
    bool found = m && vm_store_path(&store, m, path);
    unlock();
    return found;
}

esp_err_t digits_voicemail_mark_heard(uint32_t number)
{
    if (!digits_voicemail_ready()) return ESP_ERR_INVALID_STATE;
    lock();
    bool ok = vm_store_mark_heard(&store, number);
    unlock();
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t digits_voicemail_delete(uint32_t number)
{
    if (!digits_voicemail_ready()) return ESP_ERR_INVALID_STATE;
    digits_voicemail_stop();
    lock();
    bool ok = vm_store_delete(&store, number);
    unlock();
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool parse_number(const char *text, uint32_t *number)
{
    char *end = NULL;
    unsigned long value = text ? strtoul(text, &end, 10) : 0;
    if (!text || !text[0] || *end || !value || value > UINT32_MAX) return false;
    *number = (uint32_t)value;
    return true;
}

int digits_voicemail_console(int argc, char **argv)
{
    const char *verb = argc >= 2 ? argv[1] : "status";
    uint32_t number = 0;
    if (!strcmp(verb, "status") && argc <= 2) {
        if (!digits_voicemail_ready()) {
            printf("Answering machine: off (%s)\n", digits_sd_card_mounted() ? "SD card unusable" : "no SD card");
            return 0;
        }
        lock();
        unsigned count = store.count;
        unlock();
        printf("Answering machine: ready, %u messages (%d new, limit %u, %u s each)\n", count,
               digits_voicemail_unheard(), (unsigned)CAPACITY, (unsigned)CONFIG_DIGITS_VOICEMAIL_MAX_SECONDS);
        printf("Greeting: %s, %u s\n", digits_voicemail_greeting_kind(), digits_voicemail_greeting_seconds());
        printf("Speaker while answering: greeting %s, caller %s\n", hear_greeting ? "on" : "off",
               hear_caller ? "on" : "off");
        printf("Whether and when it answers is set per line on the server (see 'webrtc status')\n");
        return 0;
    }
    if (!digits_voicemail_ready()) {
        printf("Answering machine is off: no usable SD card\n");
        return 1;
    }
    if (!strcmp(verb, "listen") && argc == 4 && (!strcmp(argv[2], "greeting") || !strcmp(argv[2], "caller")) &&
        (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
        bool on = !strcmp(argv[3], "on"), greeting = hear_greeting, caller = hear_caller;
        if (!strcmp(argv[2], "greeting")) greeting = on;
        else caller = on;
        esp_err_t err = digits_voicemail_set_listen(greeting, caller);
        printf(err == ESP_OK ? "Saved\n" : "Saving failed\n");
        return err != ESP_OK;
    }
    if (!strcmp(verb, "list") && argc == 2) {
        uint8_t *tail = malloc(TAIL_BYTES);
        lock();
        if (!store.count) printf("No messages\n");
        for (unsigned i = 0; i < store.count; ++i) describe(&store.messages[i], tail);
        unlock();
        free(tail);
        return 0;
    }
    if (!strcmp(verb, "play") && (argc == 2 || (argc == 3 && parse_number(argv[2], &number)))) {
        esp_err_t err = digits_voicemail_play(number);
        if (err != ESP_OK) printf("Cannot play now: the line is busy\n");
        return err != ESP_OK;
    }
    if (!strcmp(verb, "greeting") && argc == 2) {
        esp_err_t err = digits_voicemail_play_greeting();
        if (err != ESP_OK) printf("Cannot play now: the line is busy\n");
        return err != ESP_OK;
    }
    if (!strcmp(verb, "stop") && argc == 2) {
        digits_voicemail_stop();
        return 0;
    }
    if (!strcmp(verb, "heard") && argc == 3 && parse_number(argv[2], &number)) {
        lock();
        bool ok = vm_store_mark_heard(&store, number);
        unlock();
        printf(ok ? "Message %u marked heard\n" : "No message %u\n", (unsigned)number);
        return !ok;
    }
    if (!strcmp(verb, "delete") && argc == 3 && !strcmp(argv[2], "heard")) {
        digits_voicemail_stop();
        unsigned deleted = 0;
        lock();
        for (unsigned i = store.count; i-- > 0;)
            if (store.messages[i].heard && vm_store_delete(&store, store.messages[i].number)) ++deleted;
        unlock();
        printf("Deleted %u heard messages\n", deleted);
        return 0;
    }
    if (!strcmp(verb, "delete") && argc == 3 && parse_number(argv[2], &number)) {
        digits_voicemail_stop();
        lock();
        bool ok = vm_store_delete(&store, number);
        unlock();
        printf(ok ? "Message %u deleted\n" : "No message %u\n", (unsigned)number);
        return !ok;
    }
    printf("Usage: vm [status] | vm list | vm play [N] | vm greeting | vm stop | vm heard N | "
           "vm delete N | vm delete heard | vm listen greeting|caller on|off\n");
    return 1;
}

#else

esp_err_t digits_voicemail_start(void) { return ESP_OK; }
bool digits_voicemail_ready(void) { return false; }
int digits_voicemail_unheard(void) { return -1; }
const digits_call_audio_unattended_t *digits_voicemail_begin(const char *caller)
{
    (void)caller;
    return NULL;
}
void digits_voicemail_end(void) {}
bool digits_voicemail_full(void) { return false; }
esp_err_t digits_voicemail_play(uint32_t number)
{
    (void)number;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t digits_voicemail_play_greeting(void) { return ESP_ERR_NOT_SUPPORTED; }
const char *digits_voicemail_greeting_kind(void) { return "built-in"; }
unsigned digits_voicemail_greeting_seconds(void) { return 0; }
esp_err_t digits_voicemail_greeting_reload(void) { return ESP_ERR_NOT_SUPPORTED; }
void digits_voicemail_listen(bool *greeting, bool *caller)
{
    if (greeting) *greeting = false;
    if (caller) *caller = false;
}
esp_err_t digits_voicemail_set_listen(bool greeting, bool caller) { return ESP_ERR_NOT_SUPPORTED; }
void digits_voicemail_stop(void) {}
unsigned digits_voicemail_list(digits_voicemail_message_t *out, unsigned max) { return 0; }
bool digits_voicemail_file(uint32_t number, char *path, size_t capacity) { return false; }
esp_err_t digits_voicemail_mark_heard(uint32_t number) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t digits_voicemail_delete(uint32_t number) { return ESP_ERR_NOT_SUPPORTED; }
int digits_voicemail_console(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Answering machine disabled in this build (CONFIG_DIGITS_VOICEMAIL)\n");
    return 1;
}

#endif
