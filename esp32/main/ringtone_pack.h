#pragma once

#include "esp_err.h"

// Registers the user clips packed into the "ringtones" partition
// (tools/pack_ringtones.py) with ringtones.c. Boot-time, before any player
// renders. A missing partition or empty pack is not an error: only the
// built-in ringtones are then available.
esp_err_t digits_ringtone_pack_load(void);
