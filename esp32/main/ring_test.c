#include "ring_test.h"
#include "ring_tone.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Waveshare v1.1 schematic wiring; Espressif's official ES8311 driver example.
// PA_CTRL is Extend_IO8 / TCA9555 P10. Change only this expander bit.
#define PA_BIT (1U << 0)
#define TCA_OUTPUT_1 0x03
#define TCA_CONFIG_1 0x07
#define PCM_BLOCK_FRAMES 160
#define TEST_DURATION_US 1000000LL
#define TEST_VOLUME 50

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t expander;
    i2s_chan_handle_t tx;
    const audio_codec_ctrl_if_t *ctrl;
    const audio_codec_data_if_t *data;
    const audio_codec_if_t *codec;
    esp_codec_dev_handle_t device;
    QueueHandle_t requests;
} audio;

static esp_err_t update_expander(uint8_t reg, uint8_t mask, bool high)
{
    uint8_t value;
    esp_err_t err = i2c_master_transmit_receive(audio.expander, &reg, 1, &value, 1, 100);
    if (err != ESP_OK) return err;
    uint8_t command[] = {reg, high ? (uint8_t)(value | mask) : (uint8_t)(value & ~mask)};
    return i2c_master_transmit(audio.expander, command, sizeof(command), 100);
}

static esp_err_t amplifier_set(bool enabled)
{
    return update_expander(TCA_OUTPUT_1, PA_BIT, enabled);
}

static bool silence_output(void)
{
    // Hardware gate first, then attempt DAC mute as an additional safeguard.
    // The codec API does not expose every underlying mute-register failure.
    esp_err_t err = amplifier_set(false);
    bool amplifier_disabled = err == ESP_OK;
    if (err != ESP_OK) ESP_LOGE("ring_test", "Amplifier disable failed: %s", esp_err_to_name(err));
    if (esp_codec_dev_set_out_mute(audio.device, true) != ESP_CODEC_DEV_OK)
        ESP_LOGE("ring_test", "DAC mute failed");
    // Flush queued PCM while the amplifier is disabled. DMA auto-clear then
    // keeps idle output at zero instead of repeating the last tone buffer.
    int16_t zeros[PCM_BLOCK_FRAMES * 2] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        size_t written;
        err = i2s_channel_write(audio.tx, zeros, sizeof(zeros), &written, 50);
        if (err != ESP_OK || written != sizeof(zeros)) {
            ESP_LOGW("ring_test", "I2S silence flush failed: %s", esp_err_to_name(err));
            break;
        }
    }
    return amplifier_disabled;
}

static void ring_worker(void *arg)
{
    int16_t pcm[PCM_BLOCK_FRAMES * 2];
    // IDF converts write milliseconds to ticks, rounding down. Allow one DMA
    // block plus a whole tick of scheduling margin before admitting a write.
    const uint32_t dma_ticks = (PCM_BLOCK_FRAMES * configTICK_RATE_HZ +
        RING_TONE_SAMPLE_RATE - 1) / RING_TONE_SAMPLE_RATE;
    const uint32_t min_write_ms = ((dma_ticks + 1) * 1000 +
        configTICK_RATE_HZ - 1) / configTICK_RATE_HZ;
    const uint32_t max_write_ms = min_write_ms > 50 ? min_write_ms : 50;
    int64_t previous_deadline = 0;
    for (;;) {
        int64_t requested_at;
        xQueueReceive(audio.requests, &requested_at, portMAX_DELAY);
        // Pi schedules an independent STOP for every request. Its earliest
        // stop truncates overlapping tests; do not extend our active window.
        if (requested_at < previous_deadline) {
            ESP_LOGI("ring_test", "Overlapping test coalesced; one-second stop retained");
            continue;
        }
        int64_t deadline = esp_timer_get_time() + TEST_DURATION_US;
        previous_deadline = deadline;
        esp_err_t err = ESP_FAIL;
        if (esp_codec_dev_set_out_mute(audio.device, false) == ESP_CODEC_DEV_OK)
            err = amplifier_set(true);
        if (err == ESP_OK) {
            ESP_LOGI("ring_test", "Playing 1s speaker test (440+480 Hz, volume=%d)", TEST_VOLUME);
            for (size_t frame = 0; frame < RING_TONE_FRAMES; frame += PCM_BLOCK_FRAMES) {
                ring_tone_render(pcm, frame, PCM_BLOCK_FRAMES);
                // Recheck after rendering. Near the stop deadline, let DMA
                // drain instead of polling for another buffer with zero ticks.
                int64_t remaining = deadline - esp_timer_get_time();
                if (remaining < (int64_t)min_write_ms * 1000) break;
                size_t written = 0;
                // Direct bounded I2S writes, rather than codec's long default
                // wait. The WebSocket task never performs audio work.
                uint32_t timeout_ms = (uint32_t)(remaining / 1000);
                if (timeout_ms > max_write_ms) timeout_ms = max_write_ms;
                err = i2s_channel_write(audio.tx, pcm, sizeof(pcm), &written, timeout_ms);
                if (err != ESP_OK || written != sizeof(pcm)) {
                    ESP_LOGE("ring_test", "Playback write failed: %s (%u bytes)",
                             esp_err_to_name(err), (unsigned)written);
                    err = ESP_FAIL;
                    break;
                }
            }
            while (err == ESP_OK && esp_timer_get_time() < deadline)
                vTaskDelay(1);
        } else {
            ESP_LOGE("ring_test", "Cannot enable speaker output: %s", esp_err_to_name(err));
        }
        if (silence_output())
            ESP_LOGI("ring_test", "Speaker test stopped; amplifier disabled");
        else
            ESP_LOGE("ring_test", "Speaker test finished; amplifier gate failed, DAC mute and PCM silence requested");
    }
}

static void release_audio(void)
{
    if (audio.expander) amplifier_set(false);
    if (audio.device) esp_codec_dev_delete(audio.device);
    if (audio.codec) audio_codec_delete_codec_if(audio.codec);
    if (audio.data) audio_codec_delete_data_if(audio.data);
    if (audio.ctrl) audio_codec_delete_ctrl_if(audio.ctrl);
    if (audio.tx) {
        i2s_channel_disable(audio.tx);
        i2s_del_channel(audio.tx);
    }
    if (audio.expander) i2c_master_bus_rm_device(audio.expander);
    if (audio.bus) i2c_del_master_bus(audio.bus);
    if (audio.requests) vQueueDelete(audio.requests);
    memset(&audio, 0, sizeof(audio));
}

esp_err_t digits_ring_test_init(void)
{
    if (audio.requests) return ESP_ERR_INVALID_STATE;
    esp_err_t err;
#define TRY(call) do { err = (call); if (err != ESP_OK) goto fail; } while (0)
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_11, .scl_io_num = GPIO_NUM_10,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    TRY(i2c_new_master_bus(&bus, &audio.bus));
    i2c_device_config_t expander = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x20, .scl_speed_hz = 100000,
    };
    TRY(i2c_master_bus_add_device(audio.bus, &expander, &audio.expander));
    // Latch OFF before changing P10 from input to output, avoiding a boot pop.
    TRY(amplifier_set(false));
    TRY(update_expander(TCA_CONFIG_1, PA_BIT, false));

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 2;
    channel.dma_frame_num = PCM_BLOCK_FRAMES;
    TRY(i2s_new_channel(&channel, &audio.tx, NULL)); // Output only.
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RING_TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_12, .bclk = GPIO_NUM_13, .ws = GPIO_NUM_14,
            .dout = GPIO_NUM_16, .din = I2S_GPIO_UNUSED,
        },
    };
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    TRY(i2s_channel_init_std_mode(audio.tx, &standard));
    TRY(i2s_channel_enable(audio.tx));
    // Codec component expects 8-bit 0x30 and converts it to 7-bit 0x18.
    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = audio.bus, .clock_speed_hz = 100000,
    };
    audio.ctrl = audio_codec_new_i2c_ctrl(&control);
    audio_codec_i2s_cfg_t data = {.port = I2S_NUM_0, .tx_handle = audio.tx};
    audio.data = audio_codec_new_i2s_data(&data);
    if (!audio.ctrl || !audio.data) { err = ESP_ERR_NO_MEM; goto fail; }
    es8311_codec_cfg_t codec = {
        .ctrl_if = audio.ctrl, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false, .use_mclk = true, .mclk_div = 256,
        .pa_pin = -1, // PA is controlled via the TCA9555, not native GPIO.
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    audio.codec = es8311_codec_new(&codec);
    if (!audio.codec) { err = ESP_FAIL; goto fail; }
    esp_codec_dev_cfg_t device = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = audio.codec, .data_if = audio.data,
    };
    audio.device = esp_codec_dev_new(&device);
    if (!audio.device) { err = ESP_ERR_NO_MEM; goto fail; }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = RING_TONE_SAMPLE_RATE, .channel = 2, .channel_mask = 3, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(audio.device, &sample) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(audio.device, TEST_VOLUME) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(audio.device, true) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto fail;
    }
    audio.requests = xQueueCreate(1, sizeof(int64_t));
    if (!audio.requests || xTaskCreate(ring_worker, "digits_ring", 4096, NULL, 4, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI("ring_test", "Speaker ready: ES8311 DAC, 16000 Hz, MCLK=4096000 Hz; PA=TCA9555 P10; volume=%d",
             TEST_VOLUME);
    return ESP_OK;
fail:
    release_audio();
    return err;
#undef TRY
}

void digits_ring_test_request(void)
{
    if (!audio.requests) {
        ESP_LOGE("ring_test", "Ring Test unavailable: audio initialization failed");
        return;
    }
    int64_t requested_at = esp_timer_get_time();
    // Single pending command, no accumulating audio jobs and no callback wait.
    xQueueOverwrite(audio.requests, &requested_at);
}
