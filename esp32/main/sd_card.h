#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define DIGITS_SD_MOUNT_POINT "/sdcard"

// Mounts the TF card once at boot and keeps it mounted; later calls return
// the first result. Never formats the card.
esp_err_t digits_sd_card_mount(void);
bool digits_sd_card_mounted(void);
