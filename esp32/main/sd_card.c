#include "sd_card.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// Waveshare ESP32-S3-AUDIO-Board TF slot: 1-bit SDMMC (Waveshare BSP demos).
#define PIN_CLK GPIO_NUM_40
#define PIN_CMD GPIO_NUM_42
#define PIN_D0 GPIO_NUM_41

static bool tried;
static esp_err_t result = ESP_ERR_INVALID_STATE;

esp_err_t digits_sd_card_mount(void)
{
    if (tried) return result;
    tried = true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_CLK;
    slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0;
    // The defaults name pins this board uses for I2C and I2S.
    slot.d1 = slot.d2 = slot.d3 = slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, // Never touch the user's card.
        .max_files = 4, // Ringtone or greeting stream, recording, message playback, spare.
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    result = esp_vfs_fat_sdmmc_mount(DIGITS_SD_MOUNT_POINT, &host, &slot, &config, &card);
    return result;
}

bool digits_sd_card_mounted(void)
{
    return tried && result == ESP_OK;
}
