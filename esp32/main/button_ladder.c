#include "button_ladder.h"
#include "sdkconfig.h"

#include "audio_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "button_ladder"
#define ADS1115_ADDRESS 0x48
#define REG_CONVERSION 0x00
#define REG_CONFIG 0x01

static i2c_master_dev_handle_t ads;

esp_err_t digits_button_ladder_init(void)
{
    if (CONFIG_DIGITS_BUTTON_LADDER_CHANNEL < 0) return ESP_OK;
    i2c_master_bus_handle_t bus = digits_audio_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS1115_ADDRESS, .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &ads);
    if (err == ESP_OK) {
        // Continuous conversion against GND, ±4.096 V, 475 samples/s (a new
        // value every 2 ms, ahead of the 10 ms poll), comparator off.
        uint16_t value = (uint16_t)(((4 + CONFIG_DIGITS_BUTTON_LADDER_CHANNEL) << 12) | (1 << 9) | (6 << 5) | 3);
        uint8_t command[] = {REG_CONFIG, (uint8_t)(value >> 8), (uint8_t)value};
        err = i2c_master_transmit(ads, command, sizeof(command), 50);
    }
    unsigned buttons = 0;
    int mv = 0;
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = digits_button_ladder_read(&buttons, &mv);
    }
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No ADS1115 at 0x%02X (%s): button ladder off", ADS1115_ADDRESS, esp_err_to_name(err));
        if (ads) i2c_master_bus_rm_device(ads);
        ads = NULL;
        return err;
    }
    ESP_LOGI(TAG, "Buttons on ADS1115 A%d: %d mV", CONFIG_DIGITS_BUTTON_LADDER_CHANNEL, mv);
    return ESP_OK;
}

bool digits_button_ladder_active(void) { return ads != NULL; }

esp_err_t digits_button_ladder_read(unsigned *buttons, int *mv)
{
    if (!ads) return ESP_ERR_INVALID_STATE;
    uint8_t reg = REG_CONVERSION, value[2];
    esp_err_t err = i2c_master_transmit_receive(ads, &reg, 1, value, 2, 20);
    if (err != ESP_OK) return err;
    int16_t raw = (int16_t)((value[0] << 8) | value[1]);
    *mv = raw < 0 ? 0 : raw / 8; // 4.096 V full scale: 1/8 mV per count.
    *buttons = button_ladder_decode(*mv);
    return ESP_OK;
}
