#include "handset.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "audio_board.h"
#include "microphone.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define TAG "handset"
#define WM8978_ADDRESS 0x1A
#define RATE 48000
#define FRAMES 240 // 5 ms of stereo 16-bit samples per transfer.
#define PIN_MCK GPIO_NUM_3
#define PIN_SCK GPIO_NUM_4
#define PIN_FSA GPIO_NUM_5
#define PIN_DAT GPIO_NUM_6
#define PIN_SDB GPIO_NUM_7
#define DEFAULT_TEST_DB (-9)
#define STREAM_BYTES (6 * DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t))
// Kept in hand: the microphone task takes a 20 ms frame at a time, with up
// to 10 ms of jitter against this codec's 5 ms blocks. Less than a frame of
// margin either way caused clicks in calls (underruns and drops).
#define PRIME_BYTES (DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t))
#define EXCESS_BYTES (3 * DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t))
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    int peak[2];
    double sum[2];
    unsigned samples;
} levels_t;

static struct {
    i2c_master_dev_handle_t codec;
    i2s_chan_handle_t tx, rx;
    int adc_gpio;
    handset_input_t input;
    int test_db;
    bool running;
    StreamBufferHandle_t stream;
    int16_t *scratch; // PSRAM, DIGITS_MIC_FRAME_SAMPLES.
    bool primed;
    volatile bool echo;
    int16_t *record; // PSRAM, stereo frames; the reader fills it while record_at < record_samples.
    size_t record_samples;
    volatile size_t record_at;
    levels_t levels;
    unsigned rx_overflows, underruns, drops;
    unsigned retried_writes, failed_writes;
    int output_level; // Last value written to R52/R53, -1 unknown.
    int wanted_level; // What R52/R53 should hold, -1 nothing yet.
    SemaphoreHandle_t output_lock;
} h = {.adc_gpio = -1, .test_db = DEFAULT_TEST_DB, .output_level = -1, .wanted_level = -1};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR on_rx_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context)
{
    portENTER_CRITICAL_ISR(&mux);
    ++h.rx_overflows;
    portEXIT_CRITICAL_ISR(&mux);
    return false;
}

// WM8978 control words: 7-bit register, 9-bit value. The chip cannot be read.
static esp_err_t reg(uint8_t address, uint16_t value)
{
    uint8_t word[2] = {(uint8_t)((address << 1) | ((value >> 8) & 1)), (uint8_t)value};
    // The bus is shared with the expander the phone task polls; a write can
    // meet a busy or recovering bus, so retry before giving up.
    static const uint8_t pause_ms[] = {0, 5, 20, 50};
    esp_err_t err = ESP_FAIL;
    for (size_t attempt = 0; attempt < sizeof(pause_ms); ++attempt) {
        if (pause_ms[attempt]) vTaskDelay(pdMS_TO_TICKS(pause_ms[attempt]));
        err = i2c_master_transmit(h.codec, word, sizeof(word), 100);
        if (err == ESP_OK) {
            if (attempt) ++h.retried_writes;
            return ESP_OK;
        }
    }
    ++h.failed_writes;
    ESP_LOGW(TAG, "WM8978 R%u write failed: %s", address, esp_err_to_name(err));
    return err;
}

// Headphone output level: code 57 is 0 dB, 1 dB steps, bit 6 mutes.
// A failed write is repeated by the reader task (sync_output).
static esp_err_t write_level(int level)
{
    if (level < 0 || level == h.output_level) return ESP_OK;
    h.output_level = -1;
    esp_err_t err = reg(52, (uint16_t)level);
    if (err == ESP_OK) err = reg(53, (uint16_t)(0x100 | level)); // Bit 8 updates both sides.
    if (err == ESP_OK) h.output_level = level;
    return err;
}

static esp_err_t output_level(bool mute, int db)
{
    int level = mute ? 0x040 : 57 + db;
    if (h.output_lock) xSemaphoreTake(h.output_lock, portMAX_DELAY);
    h.wanted_level = level;
    esp_err_t err = write_level(level);
    if (h.output_lock) xSemaphoreGive(h.output_lock);
    return err;
}

static void sync_output(void)
{
    if (xSemaphoreTake(h.output_lock, 0) != pdTRUE) return; // A write is under way.
    if (h.wanted_level >= 0 && h.wanted_level != h.output_level && write_level(h.wanted_level) == ESP_OK)
        ESP_LOGI(TAG, "WM8978 output level written again after a failed write");
    xSemaphoreGive(h.output_lock);
}

static esp_err_t apply_input(void)
{
    bool mic = h.input == HANDSET_INPUT_MIC;
    // The module's audio in jack is on L2/R2 (tip left), taken straight into
    // the boost stage: +6 dB for the microphone, 0 dB for line. The PGAs
    // stay disconnected and muted: with L2 routed through them they latched
    // at a rail on a route change (the ADC then reads pure zeros), and their
    // own pins (LIP/LIN) carry the module's microphone, which an inserted
    // plug disconnects, leaving an open input that picks up digital noise.
    uint16_t boost = mic ? 0x070 : 0x050;
    esp_err_t err = reg(44, 0x000);
    if (err == ESP_OK) err = reg(45, 0x040);
    if (err == ESP_OK) err = reg(46, 0x140);
    if (err == ESP_OK) err = reg(47, boost);
    if (err == ESP_OK) err = reg(48, boost);
    return err;
}

static esp_err_t codec_setup(void)
{
    esp_err_t err = reg(0, 0); // Software reset.
    if (err != ESP_OK) return err;
    h.output_level = -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint16_t setup[][2] = {
        {1, 0x01B},  // Mic bias, analogue bias, VMID 5 kOhm.
        {2, 0x1BF},  // Headphone L/R, boost L/R, PGA L/R, ADC L/R.
        {3, 0x00F},  // Output mixers and DACs; speaker outputs off.
        {4, 0x010},  // I2S, 16-bit.
        {6, 0x000},  // Clock slave, MCLK / 1 (256 fs).
        {7, 0x000},  // Filters for 48 kHz.
        // DAC 128x oversampling; right side inverted: the earpiece sits
        // between left and right (tip and ring), which also doubles its level.
        {10, 0x00A},
        {11, 0x0FF}, {12, 0x1FF}, // DAC 0 dB.
        // DAC limiter on, +12 dB boost, limit -6 dBFS: the earpiece has no
        // amplifier, and tones and speech sit well below full scale. The
        // limit leaves room for the output's +6 dB without clipping.
        {24, 0x132}, {25, 0x05C},
        {14, 0x108}, // ADC high-pass filter, 128x oversampling.
        {15, 0x0FF}, {16, 0x1FF}, // ADC 0 dB.
        {54, 0x040}, {55, 0x140}, // Speaker outputs muted.
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]) && err == ESP_OK; ++i)
        err = reg((uint8_t)setup[i][0], setup[i][1]);
    if (err == ESP_OK) err = apply_input();
    if (err == ESP_OK) err = output_level(true, 0);
    vTaskDelay(pdMS_TO_TICKS(250)); // VMID settles.
    return err;
}

static void close_channels(void)
{
    if (h.tx) {
        i2s_channel_disable(h.tx);
        i2s_del_channel(h.tx);
        h.tx = NULL;
    }
    if (h.rx) {
        i2s_channel_disable(h.rx);
        i2s_del_channel(h.rx);
        h.rx = NULL;
    }
    // Idle data pins as pulled-down inputs, so neither side drives the other.
    gpio_set_direction(PIN_DAT, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_SDB, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_DAT, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(PIN_SDB, GPIO_PULLDOWN_ONLY);
}

static esp_err_t open_channels(gpio_num_t din, gpio_num_t dout)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 4; // 20 ms each way, in internal RAM.
    channel.dma_frame_num = FRAMES;
    bool tx = dout != GPIO_NUM_NC, rx = din != GPIO_NUM_NC;
    esp_err_t err = i2s_new_channel(&channel, tx ? &h.tx : NULL, rx ? &h.rx : NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = PIN_MCK, .bclk = PIN_SCK, .ws = PIN_FSA, .dout = dout, .din = din},
    };
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // 32-bit slots around the 16-bit samples, as on the board codec, so the
    // buffers match the speaker's: a fully packed 16-bit frame came up
    // misaligned in some runs.
    standard.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    standard.slot_cfg.ws_width = 32;
    if (tx) err = i2s_channel_init_std_mode(h.tx, &standard);
    if (err == ESP_OK && rx) err = i2s_channel_init_std_mode(h.rx, &standard);
    const i2s_event_callbacks_t rx_events = {.on_recv_q_ovf = on_rx_overflow};
    if (err == ESP_OK && rx) err = i2s_channel_register_event_callback(h.rx, &rx_events, NULL);
    // Input first: it then starts with the first frame the output clocks.
    if (err == ESP_OK && rx) err = i2s_channel_enable(h.rx);
    if (err == ESP_OK && tx) err = i2s_channel_enable(h.tx);
    if (err != ESP_OK) close_channels();
    return err;
}

static esp_err_t read_block(int16_t *block, size_t bytes)
{
    size_t got = 0;
    esp_err_t err = i2s_channel_read(h.rx, block, bytes, &got, 100);
    return err == ESP_OK && got != bytes ? ESP_ERR_TIMEOUT : err;
}

// Counts samples that differ from the first on din, with the other data pin
// and din itself pulled down: an undriven pin reads all zeros.
static int listen_on(gpio_num_t din, gpio_num_t other)
{
    static int16_t block[FRAMES * 2];
    gpio_set_direction(other, GPIO_MODE_INPUT);
    gpio_set_pull_mode(other, GPIO_PULLDOWN_ONLY);
    if (open_channels(din, GPIO_NUM_NC) != ESP_OK) return -1;
    gpio_set_pull_mode(din, GPIO_PULLDOWN_ONLY);
    int changes = 0;
    int16_t first = 0;
    for (int n = 0; n < 30; ++n) { // 150 ms; the first 50 ms settle.
        if (read_block(block, sizeof(block)) != ESP_OK) { changes = -1; break; }
        if (n < 10) continue;
        if (n == 10) first = block[0];
        for (int i = 0; i < FRAMES * 2; ++i) changes += block[i] != first;
    }
    close_channels();
    return changes;
}

static esp_err_t find_data_direction(void)
{
    int on_sdb = listen_on(PIN_SDB, PIN_DAT), on_dat = listen_on(PIN_DAT, PIN_SDB);
    ESP_LOGI(TAG, "Codec data: %d changing samples on GPIO%d (SDB), %d on GPIO%d (DAT)",
             on_sdb, PIN_SDB, on_dat, PIN_DAT);
    if (on_sdb < 0 || on_dat < 0) return ESP_FAIL;
    const int threshold = 100; // Of 9600: the mic and ADC noise change nearly all.
    if (on_sdb < threshold && on_dat < threshold) return ESP_ERR_NOT_FOUND;
    h.adc_gpio = on_sdb >= on_dat ? PIN_SDB : PIN_DAT;
    return ESP_OK;
}

// Click filter. The microphone wire picks up the board's digital activity
// as clicks about 5 samples long, as tall as speech peaks once amplified.
// A sample far from the median of its 11 neighbours, measured against the
// recent typical distance, is replaced by that median; speech moves slowly
// at 48 kHz and passes. Output lags by DECLICK_HALF samples.
#define DECLICK_WINDOW 11
#define DECLICK_HALF (DECLICK_WINDOW / 2)
typedef struct {
    int16_t recent[DECLICK_WINDOW];
    unsigned at;
    int typical_x256; // Running mean distance from the median, times 256.
} declick_t;

static int16_t declick(declick_t *d, int16_t sample)
{
    d->recent[d->at] = sample;
    d->at = (d->at + 1) % DECLICK_WINDOW;
    int16_t sorted[DECLICK_WINDOW];
    for (int i = 0; i < DECLICK_WINDOW; ++i) { // Insertion sort.
        int16_t v = d->recent[i];
        int j = i;
        for (; j > 0 && sorted[j - 1] > v; --j) sorted[j] = sorted[j - 1];
        sorted[j] = v;
    }
    int median = sorted[DECLICK_HALF];
    int centre = d->recent[(d->at + DECLICK_HALF) % DECLICK_WINDOW];
    int distance = abs(centre - median), limit = 4 * d->typical_x256 / 256;
    if (limit < 24) limit = 24;
    if (distance > limit) {
        centre = median;
        distance = limit;
    }
    d->typical_x256 += distance - d->typical_x256 / 256;
    return (int16_t)centre;
}

static int16_t scaled(int16_t sample, int factor)
{
    int v = sample * factor;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// The TH-07 capsule (5 V through 1k + 5k) gives speech near -42 dBFS at the
// codec's +6 dB; 12 dB more brings it to the board microphones' level.
#define MIC_FACTOR 4
// Echo and record playback: plus about the 18 dB a call adds (tx_voice.h).
static int16_t test_gain(int16_t sample) { return scaled(sample, MIC_FACTOR * 8); }

// Drains the input for good: levels for the mic test, mono samples for the
// microphone task, and the echo test's loopback.
static void reader(void *arg)
{
    static int16_t block[FRAMES * 2], mono[FRAMES], stereo[FRAMES * 2];
    static declick_t clicks = {.typical_x256 = 20 * 256};
    unsigned logged = 0;
    for (unsigned n = 0;; ++n) {
        if (n % 100 == 0) sync_output(); // Every 500 ms.
        if (n % 1000 == 999) { // Every 5 s: clicks in the microphone path, if any.
            unsigned total = h.underruns + h.drops + h.rx_overflows;
            if (total != logged)
                ESP_LOGW(TAG, "Microphone path: %u underruns, %u drops, %u input gaps so far",
                         h.underruns, h.drops, h.rx_overflows);
            logged = total;
        }
        if (read_block(block, sizeof(block)) != ESP_OK) {
            vTaskDelay(1);
            continue;
        }
        levels_t add = {0};
        for (int i = 0; i < FRAMES; ++i) {
            int l = block[2 * i], r = block[2 * i + 1];
            if (abs(l) > add.peak[0]) add.peak[0] = abs(l);
            if (abs(r) > add.peak[1]) add.peak[1] = abs(r);
            add.sum[0] += (double)l * l;
            add.sum[1] += (double)r * r;
            mono[i] = declick(&clicks, (int16_t)l); // The handset microphone is on the tip: left.
        }
        portENTER_CRITICAL(&mux);
        for (int c = 0; c < 2; ++c) {
            if (add.peak[c] > h.levels.peak[c]) h.levels.peak[c] = add.peak[c];
            h.levels.sum[c] += add.sum[c];
        }
        h.levels.samples += FRAMES;
        portEXIT_CRITICAL(&mux);
        xStreamBufferSend(h.stream, mono, sizeof(mono), 0); // Full: the rest is dropped.
        size_t at = h.record_at;
        if (at < h.record_samples) {
            size_t n = h.record_samples - at < FRAMES ? h.record_samples - at : FRAMES;
            memcpy(h.record + 2 * at, block, n * 2 * sizeof(int16_t));
            h.record_at = at + n;
        }
        if (h.echo) {
            for (int i = 0; i < FRAMES; ++i) stereo[2 * i] = stereo[2 * i + 1] = test_gain(mono[i]);
            size_t written;
            i2s_channel_write(h.tx, stereo, sizeof(stereo), &written, 20);
        }
    }
}

esp_err_t digits_handset_init(void)
{
    if (h.running) return ESP_OK;
    i2c_master_bus_handle_t bus = digits_audio_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    if (!h.codec) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = WM8978_ADDRESS, .scl_speed_hz = 100000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &config, &h.codec);
        if (err != ESP_OK) return err;
        // The codec cannot be read: a first write tells whether it is there.
        const uint8_t probe[2] = {0, 0}; // R0: reset.
        if (i2c_master_transmit(h.codec, probe, sizeof(probe), 50) != ESP_OK) {
            i2c_master_bus_rm_device(h.codec);
            h.codec = NULL;
            return ESP_ERR_NOT_FOUND;
        }
    }
    esp_err_t err = codec_setup();
    if (err == ESP_OK) err = find_data_direction();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WM8978 answers, but no microphone data on GPIO%d or GPIO%d: check MCK/SCK/FSA and both data wires",
                 PIN_DAT, PIN_SDB);
        return err;
    }
    if (!h.stream) h.stream = xStreamBufferCreateWithCaps(STREAM_BYTES, 1, PSRAM);
    if (!h.scratch) h.scratch = heap_caps_malloc(DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t), PSRAM);
    if (!h.output_lock) h.output_lock = xSemaphoreCreateMutex();
    if (!h.stream || !h.scratch || !h.output_lock) return ESP_ERR_NO_MEM;
    gpio_num_t adc = (gpio_num_t)h.adc_gpio, dac = adc == PIN_SDB ? PIN_DAT : PIN_SDB;
    err = open_channels(adc, dac);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCoreWithCaps(reader, "digits_handset", 3072, NULL, 5, NULL, 1, PSRAM) != pdPASS) {
        close_channels();
        return ESP_ERR_NO_MEM;
    }
    h.running = true;
    ESP_LOGI(TAG, "WM8978 ready: microphone data on GPIO%d, earpiece data on GPIO%d; internal RAM free %u",
             adc, dac, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

bool digits_handset_ready(void) { return h.running; }
int digits_handset_adc_gpio(void) { return h.adc_gpio; }
handset_input_t digits_handset_input(void) { return h.input; }
int digits_handset_test_db(void) { return h.test_db; }

esp_err_t digits_handset_set_input(handset_input_t input)
{
    h.input = input;
    return h.codec ? apply_input() : ESP_OK;
}

esp_err_t digits_handset_set_test_db(int db)
{
    if (db < -57 || db > 6) return ESP_ERR_INVALID_ARG;
    h.test_db = db;
    return ESP_OK;
}

esp_err_t digits_handset_output(bool enabled, int volume)
{
    if (!h.running) return ESP_ERR_INVALID_STATE;
    if (volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    return output_level(!enabled || volume == 0, -34 + (volume - 1) * 40 / 99);
}

esp_err_t digits_handset_write(const int16_t *stereo, size_t bytes, size_t *written, uint32_t timeout_ms)
{
    if (!h.running) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(h.tx, stereo, bytes, written, timeout_ms);
}

void digits_handset_capture(int16_t *pcm, int16_t *reference, size_t frames, bool use)
{
    if (!h.running || frames > DIGITS_MIC_FRAME_SAMPLES) return;
    const size_t bytes = frames * sizeof(int16_t);
    size_t available = xStreamBufferBytesAvailable(h.stream);
    if (!h.primed && available >= bytes + PRIME_BYTES) h.primed = true;
    // Both I2S ports run from one clock, so the fill level only moves at
    // startup or after a stall; drop a large excess to keep the delay short.
    if (h.primed && available > bytes + EXCESS_BYTES) {
        ++h.drops;
        while (available > bytes + PRIME_BYTES) {
            size_t excess = available - bytes - PRIME_BYTES;
            if (excess > bytes) excess = bytes;
            available -= xStreamBufferReceive(h.stream, h.scratch, excess, 0);
        }
    }
    bool have = h.primed && available >= bytes;
    if (have) xStreamBufferReceive(h.stream, h.scratch, bytes, 0);
    else if (h.primed) {
        h.primed = false;
        ++h.underruns;
    }
    if (!use) return;
    if (have) for (size_t i = 0; i < frames; ++i) pcm[i] = scaled(h.scratch[i], MIC_FACTOR);
    else memset(pcm, 0, bytes);
    memset(reference, 0, bytes); // The speaker is off.
}

// Tests run only while nothing else plays (lifted handset: after the dial
// tone and reorder have ended).
static esp_err_t test_begin(bool output)
{
    if (!h.running) {
        esp_err_t err = digits_handset_init();
        if (err != ESP_OK) return err;
    }
    if (!output) return ESP_OK;
    if (!digits_audio_speaker_acquire(0)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = output_level(false, h.test_db);
    if (err != ESP_OK) digits_audio_speaker_release();
    return err;
}

static void test_end(void)
{
    // Back to what the route wants.
    digits_handset_output(digits_audio_handset_route(), digits_audio_speaker_volume());
    digits_audio_speaker_release();
}

esp_err_t digits_handset_tone(unsigned seconds)
{
    static int16_t out[FRAMES * 2];
    esp_err_t err = test_begin(true);
    if (err != ESP_OK) return err;
    printf("440 Hz at -12 dBFS for %u s, output %d dB\n", seconds, h.test_db);
    const unsigned total = seconds * RATE, fade = RATE / 100;
    float phase = 0;
    const float step = 2.0f * (float)M_PI * 440.0f / RATE;
    for (unsigned n = 0; n < total && err == ESP_OK;) {
        for (int i = 0; i < FRAMES; ++i, ++n) {
            float gain = n < fade ? (float)n / fade : n + fade > total ? (float)(total - n) / fade : 1.0f;
            if (n >= total) gain = 0;
            int16_t s = (int16_t)(8192.0f * gain * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            out[2 * i] = out[2 * i + 1] = s;
        }
        size_t written = 0;
        err = i2s_channel_write(h.tx, out, sizeof(out), &written, 100);
    }
    vTaskDelay(pdMS_TO_TICKS(30)); // Let the last block play.
    test_end();
    return err;
}

static float dbfs(float value) { return value < 1.0f ? -99.0f : 20.0f * log10f(value / 32768.0f); }

static levels_t take_levels(void)
{
    portENTER_CRITICAL(&mux);
    levels_t levels = h.levels;
    memset(&h.levels, 0, sizeof(h.levels));
    portEXIT_CRITICAL(&mux);
    return levels;
}

esp_err_t digits_handset_mic(unsigned seconds)
{
    esp_err_t err = test_begin(false);
    if (err != ESP_OK) return err;
    printf("%s input, levels every 250 ms (peak / RMS dBFS):\n", h.input == HANDSET_INPUT_MIC ? "Mic" : "Line");
    take_levels();
    for (unsigned line = 0; line < seconds * 4; ++line) {
        vTaskDelay(pdMS_TO_TICKS(250));
        levels_t l = take_levels();
        double n = l.samples ? l.samples : 1;
        printf("  L %6.1f / %6.1f   R %6.1f / %6.1f\n", dbfs((float)l.peak[0]), dbfs((float)sqrt(l.sum[0] / n)),
               dbfs((float)l.peak[1]), dbfs((float)sqrt(l.sum[1] / n)));
    }
    return ESP_OK;
}

esp_err_t digits_handset_echo(unsigned seconds)
{
    esp_err_t err = test_begin(true);
    if (err != ESP_OK) return err;
    printf("Echo for %u s: left input to the output\n", seconds);
    h.echo = true;
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    h.echo = false;
    vTaskDelay(pdMS_TO_TICKS(30));
    test_end();
    return ESP_OK;
}

esp_err_t digits_handset_test_input(int r44, int r45, int r47)
{
    if (!h.codec) return ESP_ERR_INVALID_STATE;
    if (r44 < 0) return apply_input(); // Back to the selected input.
    esp_err_t err = reg(44, (uint16_t)(r44 & 0x1FF));
    if (err == ESP_OK) err = reg(45, (uint16_t)(r45 & 0x0FF));
    if (err == ESP_OK) err = reg(46, (uint16_t)(0x100 | (r45 & 0x0FF)));
    if (err == ESP_OK) err = reg(47, (uint16_t)(r47 & 0x1FF));
    if (err == ESP_OK) err = reg(48, (uint16_t)(r47 & 0x1FF));
    return err;
}

esp_err_t digits_handset_capture_raw(unsigned seconds, bool gain, int16_t **out, size_t *out_samples)
{
    if (!h.running) return ESP_ERR_INVALID_STATE;
    if (h.record) return ESP_ERR_INVALID_STATE; // One recording at a time.
    const size_t samples = (size_t)seconds * RATE;
    int16_t *record = heap_caps_malloc(samples * 2 * sizeof(int16_t), PSRAM);
    if (!record) return ESP_ERR_NO_MEM;
    h.record = record;
    h.record_at = 0;
    h.record_samples = samples;
    while (h.record_at < samples) vTaskDelay(pdMS_TO_TICKS(50));
    h.record_samples = 0;
    vTaskDelay(pdMS_TO_TICKS(20)); // The reader is past its copy.
    h.record = NULL;
    if (gain) { // As a caller hears it: click filter and gain, on both sides.
        declick_t clicks = {.typical_x256 = 20 * 256};
        for (size_t i = 0; i < samples; ++i)
            record[2 * i] = record[2 * i + 1] = test_gain(declick(&clicks, record[2 * i]));
    }
    *out = record;
    *out_samples = samples;
    return ESP_OK;
}

esp_err_t digits_handset_record(unsigned seconds)
{
    static int16_t out[FRAMES * 2];
    esp_err_t err = test_begin(false);
    if (err != ESP_OK) return err;
    printf("Recording %u s from the %s input...\n", seconds, h.input == HANDSET_INPUT_MIC ? "mic" : "line");
    int16_t *record;
    size_t samples;
    err = digits_handset_capture_raw(seconds, true, &record, &samples);
    if (err != ESP_OK) return err;
    // Through the speaker functions: the board speaker with the handset
    // down, the earpiece while it is lifted.
    if (!digits_audio_speaker_acquire(0)) err = ESP_ERR_INVALID_STATE;
    else {
        printf("Playing it back on the %s...\n", digits_audio_handset_route() ? "earpiece" : "speaker");
        err = digits_audio_speaker_start();
        for (size_t at = 0; at < samples && err == ESP_OK; at += FRAMES) {
            for (size_t i = 0; i < FRAMES; ++i)
                out[2 * i] = out[2 * i + 1] = at + i < samples ? record[2 * (at + i)] : 0;
            size_t written = 0;
            err = digits_audio_speaker_write(out, sizeof(out), &written, 100);
        }
        digits_audio_speaker_silence();
        digits_audio_speaker_release();
    }
    heap_caps_free(record);
    return err;
}

void digits_handset_status(void)
{
    if (!h.running) {
        printf("Handset codec not running (no WM8978, or its data lines failed; see the boot log)\n");
        return;
    }
    portENTER_CRITICAL(&mux);
    unsigned overflows = h.rx_overflows;
    portEXIT_CRITICAL(&mux);
    printf("WM8978 ready: microphone data on GPIO%d, input %s, test volume %d dB\n", h.adc_gpio,
           h.input == HANDSET_INPUT_MIC ? "mic" : "line", h.test_db);
    printf("Route: %s; input gaps %u, microphone task underruns %u, drops %u; I2C writes retried %u, failed %u\n",
           digits_audio_handset_route() ? "handset" : "speaker", overflows, h.underruns, h.drops,
           h.retried_writes, h.failed_writes);
}
