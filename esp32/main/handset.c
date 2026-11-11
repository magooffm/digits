#include "handset.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "audio_board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
#define DEFAULT_OUTPUT_DB (-9)

static struct {
    i2c_master_dev_handle_t codec;
    i2s_chan_handle_t tx, rx;
    int adc_gpio;
    handset_input_t input;
    int output_db;
} h = {.adc_gpio = -1, .output_db = DEFAULT_OUTPUT_DB};

static int16_t buffer[FRAMES * 2];
static volatile unsigned rx_overflows, tx_underruns; // From the I2S ISR.

static bool IRAM_ATTR on_rx_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context)
{
    ++rx_overflows;
    return false;
}

static bool IRAM_ATTR on_tx_underrun(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context)
{
    ++tx_underruns;
    return false;
}
static int16_t out[FRAMES * 2];

// WM8978 control words: 7-bit register, 9-bit value. The chip cannot be read.
static esp_err_t reg(uint8_t address, uint16_t value)
{
    uint8_t word[2] = {(uint8_t)((address << 1) | ((value >> 8) & 1)), (uint8_t)value};
    // The bus is shared with the expander the phone task polls; a write can
    // meet a busy or recovering bus, so retry before giving up.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(5));
        err = i2c_master_transmit(h.codec, word, sizeof(word), 100);
        if (err == ESP_OK) return ESP_OK;
    }
    ESP_LOGW(TAG, "WM8978 R%u write failed: %s", address, esp_err_to_name(err));
    return err;
}

static uint16_t output_code(void) { return (uint16_t)(57 + h.output_db); } // 57 = 0 dB.

static esp_err_t output_mute(bool mute)
{
    uint16_t level = mute ? 0x040 : output_code(); // Bit 6 mutes.
    esp_err_t err = reg(52, level);
    return err == ESP_OK ? reg(53, 0x100 | level) : err; // Bit 8 updates both sides.
}

static esp_err_t apply_input(void)
{
    bool mic = h.input == HANDSET_INPUT_MIC;
    esp_err_t err = reg(44, mic ? 0x033 : 0x000); // Mic: LIP/LIN and RIP/RIN to the PGAs.
    // PGA +18 dB for the mic; muted (bit 6) for line.
    if (err == ESP_OK) err = reg(45, mic ? 0x028 : 0x040);
    if (err == ESP_OK) err = reg(46, mic ? 0x128 : 0x140);
    // Boost: mic +20 dB after the PGA; line L2/R2 at 0 dB.
    if (err == ESP_OK) err = reg(47, mic ? 0x100 : 0x050);
    if (err == ESP_OK) err = reg(48, mic ? 0x100 : 0x050);
    return err;
}

static esp_err_t codec_setup(void)
{
    esp_err_t err = reg(0, 0); // Software reset.
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint16_t setup[][2] = {
        {1, 0x01B},  // Mic bias, analogue bias, VMID 5 kOhm.
        {2, 0x1BF},  // Headphone L/R, boost L/R, PGA L/R, ADC L/R.
        {3, 0x00F},  // Output mixers and DACs; speaker outputs off.
        {4, 0x010},  // I2S, 16-bit.
        {6, 0x000},  // Clock slave, MCLK / 1 (256 fs).
        {7, 0x000},  // Filters for 48 kHz.
        {10, 0x008}, // DAC 128x oversampling.
        {11, 0x0FF}, {12, 0x1FF}, // DAC 0 dB.
        {14, 0x108}, // ADC high-pass filter, 128x oversampling.
        {15, 0x0FF}, {16, 0x1FF}, // ADC 0 dB.
        {54, 0x040}, {55, 0x140}, // Speaker outputs muted.
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]) && err == ESP_OK; ++i)
        err = reg((uint8_t)setup[i][0], setup[i][1]);
    if (err == ESP_OK) err = apply_input();
    if (err == ESP_OK) err = output_mute(true);
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
    channel.dma_desc_num = 6; // 30 ms: the console task runs at low priority.
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
    // 32-bit slots around the 16-bit samples, as on the board codec: a fully
    // packed 16-bit frame came up misaligned in some runs.
    standard.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    standard.slot_cfg.ws_width = 32;
    if (tx) err = i2s_channel_init_std_mode(h.tx, &standard);
    if (err == ESP_OK && rx) err = i2s_channel_init_std_mode(h.rx, &standard);
    const i2s_event_callbacks_t rx_events = {.on_recv_q_ovf = on_rx_overflow};
    const i2s_event_callbacks_t tx_events = {.on_send_q_ovf = on_tx_underrun};
    if (err == ESP_OK && rx) err = i2s_channel_register_event_callback(h.rx, &rx_events, NULL);
    if (err == ESP_OK && tx) err = i2s_channel_register_event_callback(h.tx, &tx_events, NULL);
    rx_overflows = tx_underruns = 0;
    // Input first: it then starts with the first frame the output clocks.
    if (err == ESP_OK && rx) err = i2s_channel_enable(h.rx);
    if (err == ESP_OK && tx) err = i2s_channel_enable(h.tx);
    if (err != ESP_OK) close_channels();
    return err;
}

static esp_err_t read_block(void)
{
    size_t got = 0;
    esp_err_t err = i2s_channel_read(h.rx, buffer, sizeof(buffer), &got, 100);
    return err == ESP_OK && got != sizeof(buffer) ? ESP_ERR_TIMEOUT : err;
}

// Counts samples that differ from the first on din, with the other data pin
// and din itself pulled down: an undriven pin reads all zeros.
static int listen_on(gpio_num_t din, gpio_num_t other)
{
    gpio_set_direction(other, GPIO_MODE_INPUT);
    gpio_set_pull_mode(other, GPIO_PULLDOWN_ONLY);
    if (open_channels(din, GPIO_NUM_NC) != ESP_OK) return -1;
    gpio_set_pull_mode(din, GPIO_PULLDOWN_ONLY);
    int changes = 0;
    int16_t first = 0;
    for (int block = 0; block < 30; ++block) { // 150 ms; the first 50 ms settle.
        if (read_block() != ESP_OK) { changes = -1; break; }
        if (block < 10) continue;
        if (block == 10) first = buffer[0];
        for (int i = 0; i < FRAMES * 2; ++i) changes += buffer[i] != first;
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

esp_err_t digits_handset_init(void)
{
    if (digits_handset_ready()) return ESP_OK;
    i2c_master_bus_handle_t bus = digits_audio_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    if (!h.codec) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = WM8978_ADDRESS, .scl_speed_hz = 100000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &config, &h.codec);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = codec_setup();
    if (err != ESP_OK) return err;
    err = find_data_direction();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "WM8978 ready: microphone data on GPIO%d, earpiece data on GPIO%d",
                 h.adc_gpio, h.adc_gpio == PIN_SDB ? PIN_DAT : PIN_SDB);
    else
        ESP_LOGW(TAG, "WM8978 answers, but no microphone data on GPIO%d or GPIO%d: check MCK/SCK/FSA and both data wires",
                 PIN_DAT, PIN_SDB);
    return err;
}

bool digits_handset_ready(void) { return h.adc_gpio >= 0; }
int digits_handset_adc_gpio(void) { return h.adc_gpio; }
handset_input_t digits_handset_input(void) { return h.input; }
int digits_handset_output_db(void) { return h.output_db; }

esp_err_t digits_handset_set_input(handset_input_t input)
{
    h.input = input;
    return h.codec ? apply_input() : ESP_OK;
}

esp_err_t digits_handset_set_output_db(int db)
{
    if (db < -57 || db > 6) return ESP_ERR_INVALID_ARG;
    h.output_db = db;
    return ESP_OK; // Applied when the next test unmutes.
}

static esp_err_t start(bool input, bool output)
{
    esp_err_t err = digits_handset_init();
    if (err != ESP_OK) return err;
    gpio_num_t adc = (gpio_num_t)h.adc_gpio, dac = adc == PIN_SDB ? PIN_DAT : PIN_SDB;
    err = open_channels(input ? adc : GPIO_NUM_NC, output ? dac : GPIO_NUM_NC);
    if (err == ESP_OK && output) err = output_mute(false);
    if (err != ESP_OK) close_channels();
    return err;
}

static void stop(void)
{
    unsigned missed_in = rx_overflows, missed_out = tx_underruns; // Before output runs dry.
    bool input = h.rx, output = h.tx;
    if (output) output_mute(true);
    vTaskDelay(pdMS_TO_TICKS(20)); // Mute before the clocks stop.
    close_channels();
    if (input) printf("Input gaps (not read in time): %u\n", missed_in);
    if (output) printf("Output gaps (not written in time): %u\n", missed_out);
}

static esp_err_t write_block(void)
{
    size_t written = 0;
    esp_err_t err = i2s_channel_write(h.tx, out, sizeof(out), &written, 100);
    return err == ESP_OK && written != sizeof(out) ? ESP_ERR_TIMEOUT : err;
}

esp_err_t digits_handset_tone(unsigned seconds)
{
    esp_err_t err = start(false, true);
    if (err != ESP_OK) return err;
    printf("440 Hz at -12 dBFS for %u s, output %d dB\n", seconds, h.output_db);
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
        err = write_block();
        if (n == FRAMES) rx_overflows = tx_underruns = 0; // Count from the first block on.
    }
    stop();
    return err;
}

static float dbfs(float value) { return value < 1.0f ? -99.0f : 20.0f * log10f(value / 32768.0f); }

esp_err_t digits_handset_mic(unsigned seconds)
{
    esp_err_t err = start(true, false);
    if (err != ESP_OK) return err;
    printf("%s input, levels every 250 ms (peak / RMS dBFS):\n", h.input == HANDSET_INPUT_MIC ? "Mic" : "Line");
    const int per_line = RATE / 4 / FRAMES;
    for (unsigned line = 0; line < seconds * 4 && err == ESP_OK; ++line) {
        int peak[2] = {0, 0};
        double sum[2] = {0, 0};
        for (int block = 0; block < per_line && err == ESP_OK; ++block) {
            err = read_block();
            if (line == 0 && block == 0) rx_overflows = 0;
            for (int i = 0; i < FRAMES * 2; ++i) {
                int s = buffer[i], c = i & 1;
                if (abs(s) > peak[c]) peak[c] = abs(s);
                sum[c] += (double)s * s;
            }
        }
        const double n = (double)per_line * FRAMES;
        printf("  L %6.1f / %6.1f   R %6.1f / %6.1f\n", dbfs((float)peak[0]), dbfs((float)sqrt(sum[0] / n)),
               dbfs((float)peak[1]), dbfs((float)sqrt(sum[1] / n)));
    }
    stop();
    return err;
}

esp_err_t digits_handset_echo(unsigned seconds)
{
    esp_err_t err = start(true, true);
    if (err != ESP_OK) return err;
    printf("Echo for %u s: input to output, both sides mixed\n", seconds);
    for (unsigned block = 0; block < seconds * (RATE / FRAMES) && err == ESP_OK; ++block) {
        err = read_block();
        for (int i = 0; i < FRAMES && err == ESP_OK; ++i)
            out[2 * i] = out[2 * i + 1] = (int16_t)((buffer[2 * i] + buffer[2 * i + 1]) / 2);
        if (err == ESP_OK) err = write_block();
        if (block == 0) rx_overflows = tx_underruns = 0; // Count from the first block on.
    }
    stop();
    return err;
}
