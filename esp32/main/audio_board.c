#include "audio_board.h"
#include <string.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Waveshare v1.1: PA_CTRL is Extend_IO8 / TCA9555 P10, active high.
#define PA_BIT (1U << 0)
#define TCA_OUTPUT_1 0x03
#define TCA_CONFIG_1 0x07
#define TCA_POLARITY_1 0x05
#define TCA_INPUT_1 0x01
#define RECORD_KEY_BIT (1U << 1)
#define SPEAKER_VOLUME 50

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t expander;
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    const audio_codec_ctrl_if_t *ctrl;
    const audio_codec_data_if_t *data;
    const audio_codec_if_t *codec;
    esp_codec_dev_handle_t device;
    SemaphoreHandle_t speaker_lock;
    SemaphoreHandle_t expander_lock;
} board;

static esp_err_t update_expander(uint8_t reg, uint8_t mask, bool high)
{
    if (xSemaphoreTake(board.expander_lock, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t value;
    esp_err_t err = i2c_master_transmit_receive(board.expander, &reg, 1, &value, 1, 100);
    if (err == ESP_OK) {
        uint8_t command[] = {reg, high ? (uint8_t)(value | mask) : (uint8_t)(value & ~mask)};
        err = i2c_master_transmit(board.expander, command, sizeof(command), 100);
    }
    xSemaphoreGive(board.expander_lock);
    return err;
}

static esp_err_t amplifier_set(bool enabled)
{
    return update_expander(TCA_OUTPUT_1, PA_BIT, enabled);
}

static void release_board(void)
{
    if (board.expander && board.expander_lock) amplifier_set(false);
    if (board.device) esp_codec_dev_delete(board.device);
    if (board.codec) audio_codec_delete_codec_if(board.codec);
    if (board.data) audio_codec_delete_data_if(board.data);
    if (board.ctrl) audio_codec_delete_ctrl_if(board.ctrl);
    if (board.rx) {
        i2s_channel_disable(board.rx);
        i2s_del_channel(board.rx);
    }
    if (board.tx) {
        i2s_channel_disable(board.tx);
        i2s_del_channel(board.tx);
    }
    if (board.expander) i2c_master_bus_rm_device(board.expander);
    if (board.bus) i2c_del_master_bus(board.bus);
    if (board.speaker_lock) vSemaphoreDelete(board.speaker_lock);
    if (board.expander_lock) vSemaphoreDelete(board.expander_lock);
    memset(&board, 0, sizeof(board));
}

// esp_codec_dev_open(1.6.2) ignores data-interface format/enable failures.
// Validate the actual native configuration rather than trusting that result.
static esp_err_t verify_format(i2s_chan_handle_t channel, bool enabled)
{
    i2s_chan_info_t info;
    esp_err_t err = i2s_channel_get_info(channel, &info);
    if (err != ESP_OK) return err;
    // IDF makes RX the internal clock slave of TX in a full-duplex pair.
    i2s_role_t role = info.dir == I2S_DIR_TX ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE;
    if (info.mode != I2S_COMM_MODE_STD || info.role != role ||
        info.pair_chan != (info.dir == I2S_DIR_TX ? board.rx : board.tx) ||
        info.is_enabled != enabled || !info.mode_cfg) return ESP_ERR_INVALID_STATE;
    const i2s_std_config_t *cfg = info.mode_cfg;
    if (cfg->clk_cfg.sample_rate_hz != DIGITS_AUDIO_SAMPLE_RATE ||
        cfg->clk_cfg.mclk_multiple != I2S_MCLK_MULTIPLE_256 ||
        cfg->slot_cfg.data_bit_width != I2S_DATA_BIT_WIDTH_16BIT ||
        cfg->slot_cfg.slot_bit_width != I2S_SLOT_BIT_WIDTH_16BIT ||
        cfg->slot_cfg.ws_width != 16 ||
        cfg->slot_cfg.slot_mode != I2S_SLOT_MODE_STEREO ||
        cfg->slot_cfg.slot_mask != I2S_STD_SLOT_BOTH || !cfg->slot_cfg.bit_shift ||
        cfg->slot_cfg.ws_pol || cfg->slot_cfg.big_endian || cfg->slot_cfg.bit_order_lsb ||
        !cfg->slot_cfg.left_align)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t digits_audio_board_init(void)
{
    if (board.bus) return ESP_ERR_INVALID_STATE;
    esp_err_t err;
#define TRY(call) do { err = (call); if (err != ESP_OK) goto fail; } while (0)
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_11, .scl_io_num = GPIO_NUM_10,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    TRY(i2c_new_master_bus(&bus, &board.bus));
    i2c_device_config_t expander = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x20, .scl_speed_hz = 100000,
    };
    TRY(i2c_master_bus_add_device(board.bus, &expander, &board.expander));
    board.expander_lock = xSemaphoreCreateMutex();
    if (!board.expander_lock) { err = ESP_ERR_NO_MEM; goto fail; }
    // Latch OFF before changing P10 from input to output; preserve other bits.
    TRY(amplifier_set(false));
    TRY(update_expander(TCA_CONFIG_1, PA_BIT, false));

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 2;
    channel.dma_frame_num = DIGITS_AUDIO_DMA_FRAMES;
    TRY(i2s_new_channel(&channel, &board.tx, &board.rx));
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DIGITS_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_12, .bclk = GPIO_NUM_13, .ws = GPIO_NUM_14,
            .dout = GPIO_NUM_16, .din = GPIO_NUM_15,
        },
    };
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // Explicit 16-bit slots, not 16-bit values packed into wider words.
    standard.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    TRY(i2s_channel_init_std_mode(board.tx, &standard));
    TRY(i2s_channel_init_std_mode(board.rx, &standard));
    TRY(i2s_channel_enable(board.tx));
    // The codec adapter owns TX only. Capture owns native RX and never asks
    // this adapter to close/reconfigure either side during operation.
    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = board.bus, .clock_speed_hz = 100000,
    };
    board.ctrl = audio_codec_new_i2c_ctrl(&control);
    audio_codec_i2s_cfg_t data = {.port = I2S_NUM_0, .tx_handle = board.tx};
    board.data = audio_codec_new_i2s_data(&data);
    if (!board.ctrl || !board.data) { err = ESP_ERR_NO_MEM; goto fail; }
    es8311_codec_cfg_t codec = {
        .ctrl_if = board.ctrl, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false, .use_mclk = true, .mclk_div = 256, .pa_pin = -1,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    board.codec = es8311_codec_new(&codec);
    if (!board.codec) { err = ESP_FAIL; goto fail; }
    esp_codec_dev_cfg_t device = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = board.codec, .data_if = board.data,
    };
    board.device = esp_codec_dev_new(&device);
    if (!board.device) { err = ESP_ERR_NO_MEM; goto fail; }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = DIGITS_AUDIO_SAMPLE_RATE, .channel = 2, .channel_mask = 3,
        .bits_per_sample = 16, .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(board.device, &sample) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(board.device, SPEAKER_VOLUME) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(board.device, true) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto fail;
    }
    TRY(verify_format(board.tx, true));
    TRY(verify_format(board.rx, false));
    board.speaker_lock = xSemaphoreCreateMutex();
    if (!board.speaker_lock) { err = ESP_ERR_NO_MEM; goto fail; }
    ESP_LOGI("audio_board", "Speaker ready: ES8311 DAC, 48000 Hz, MCLK=12288000 Hz; PA=TCA9555 P10; volume=%d",
             SPEAKER_VOLUME);
    return ESP_OK;
fail:
    release_board();
    return err;
#undef TRY
}

i2c_master_bus_handle_t digits_audio_i2c_bus(void) { return board.bus; }
i2s_chan_handle_t digits_audio_rx_channel(void) { return board.rx; }

esp_err_t digits_audio_record_button_init(void)
{
    if (!board.expander || !board.expander_lock) return ESP_ERR_INVALID_STATE;
    esp_err_t err = update_expander(TCA_CONFIG_1, RECORD_KEY_BIT, true);
    if (err == ESP_OK) err = update_expander(TCA_POLARITY_1, RECORD_KEY_BIT, false);
    return err;
}

esp_err_t digits_audio_record_button_read(bool *pressed)
{
    if (!pressed) return ESP_ERR_INVALID_ARG;
    if (!board.expander) return ESP_ERR_INVALID_STATE;
    uint8_t reg = TCA_INPUT_1, value;
    esp_err_t err = i2c_master_transmit_receive(board.expander, &reg, 1, &value, 1, 20);
    if (err == ESP_OK) *pressed = (value & RECORD_KEY_BIT) == 0;
    return err;
}

bool digits_audio_speaker_acquire(uint32_t timeout_ms)
{
    return board.speaker_lock &&
        xSemaphoreTake(board.speaker_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void digits_audio_speaker_release(void)
{
    xSemaphoreGive(board.speaker_lock);
}

esp_err_t digits_audio_speaker_start(void)
{
    if (!board.device) return ESP_ERR_INVALID_STATE;
    if (esp_codec_dev_set_out_mute(board.device, false) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    return amplifier_set(true);
}

esp_err_t digits_audio_speaker_write(const int16_t *stereo, size_t bytes,
                                    size_t *written, uint32_t timeout_ms)
{
    if (!board.tx) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(board.tx, stereo, bytes, written, timeout_ms);
}

bool digits_audio_speaker_silence(void)
{
    if (!board.device) return false;
    // Hardware gate first; codec mute API can hide an underlying I2C failure.
    esp_err_t err = amplifier_set(false);
    bool disabled = err == ESP_OK;
    if (err != ESP_OK) ESP_LOGE("ring_test", "Amplifier disable failed: %s", esp_err_to_name(err));
    if (esp_codec_dev_set_out_mute(board.device, true) != ESP_CODEC_DEV_OK)
        ESP_LOGE("ring_test", "DAC mute failed");
    static const int16_t zeros[DIGITS_AUDIO_DMA_FRAMES * 2] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        size_t written;
        err = digits_audio_speaker_write(zeros, sizeof(zeros), &written, 50);
        if (err != ESP_OK || written != sizeof(zeros)) {
            ESP_LOGW("ring_test", "I2S silence flush failed: %s", esp_err_to_name(err));
            break;
        }
    }
    return disabled;
}
