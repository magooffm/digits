#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

typedef enum {
    DIGITS_VOICE_COPPER = 0,
    DIGITS_VOICE_MODERN = 1,
} digits_voice_style_t;

typedef struct {
    digits_voice_style_t voice_style;
    bool silent_mode; // Effective server value: manual silence OR Quiet Hours.
    bool auto_update; // Cached only; this module never starts an updater.
    bool voicemail_enabled; // Cached only; no answering machine is activated.
    uint8_t voicemail_ring_timeout_seconds;
} digits_line_settings_t;

typedef struct {
    uint32_t revision, accepted, duplicates, invalid, stale;
    uint32_t writes, write_errors;
    bool loaded, dirty, media_busy;
    // last_write_error also reports ESP_ERR_NO_MEM if the persistence worker
    // could not start; authenticated RAM updates still work in that mode.
    esp_err_t load_error, last_write_error;
} digits_settings_stats_t;

// Call once after nvs_flash_init, before signaling. A missing/invalid settings
// record uses copper, silent/auto-update/voicemail off and a 20 s voicemail
// timeout, without modifying or erasing pairing. Worker creation failure returns
// ESP_ERR_NO_MEM for the boot warning, but the cached/default RAM snapshot and
// authenticated updates remain available. Changed settings stay dirty without
// persistence until reboot; initialization does not retry worker creation.
esp_err_t digits_settings_init(void);

// Defer new cache writes while calls/ringing and their cleanup are active. This
// is nonblocking; a flash operation already in progress cannot be cancelled.
// Clear only after media workers release their resources.
void digits_settings_set_media_busy(bool busy);

// ready means the caller has sent registration for its paired credentials.
// There is no server registration ACK. Invalidate on disconnect, credential
// replacement/error or unpaired registration. Older session updates are ignored.
void digits_settings_transport(uint32_t session, bool ready);

// Full server envelope. Bounded parsing and atomic RAM publication only: no
// allocation, flash, logging or blocking I/O. False means rejected; true includes
// duplicate snapshots. Missing scalar values restore defaults; missing/null
// voicemail preserves that optional block. Caller retains ownership of JSON.
bool digits_settings_receive(const cJSON *message, uint32_t session);

// Either output may be NULL. Short critical sections only, safe for audio tasks.
void digits_settings_get(digits_line_settings_t *snapshot, digits_settings_stats_t *stats);
bool digits_settings_silent(void);
