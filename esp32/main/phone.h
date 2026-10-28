#pragma once

#include "esp_err.h"

// Hook switch and call buttons. Start after audio-board and incoming-ring
// initialization; WebRTC may start later (commands fail until it is online).
// A no-op returning ESP_OK when CONFIG_DIGITS_PHONE is disabled.
esp_err_t digits_phone_start(void);

#include <stdbool.h>
#include <stdint.h>
#include "ring_select.h"

// Settings shared by the console and the web page. Setters save to NVS and
// must run on a task with an internal-RAM stack.
#define DIGITS_PHONE_MAX_BUTTONS 3
typedef struct {
    char number[32];         // "" when unset.
    uint8_t ringtone;        // How the callee's phone rings (ringtones.h).
    uint8_t selector;        // This phone's knob position for their calls, 0-based.
    bool selector_hardware;  // A physical knob sets it.
    unsigned rings_here;     // Ringtone their calls ring with here now.
} digits_phone_button_t;
typedef struct {
    unsigned buttons;        // Call buttons this build has.
    digits_phone_button_t button[DIGITS_PHONE_MAX_BUTTONS];
    ring_select_map_t map;   // Knob position -> built-in ringtone.
    char file[RING_SELECT_MAX_POSITIONS][52]; // Knob position -> SD card song name, "" none.
    uint8_t effective[RING_SELECT_MAX_POSITIONS]; // What each position plays now.
    bool off_hook;
} digits_phone_config_t;

void digits_phone_config(digits_phone_config_t *out);
esp_err_t digits_phone_set_number(unsigned button, const char *number); // "" restores the build default.
esp_err_t digits_phone_set_ringtone(unsigned button, unsigned ringtone);
esp_err_t digits_phone_set_selector(unsigned button, unsigned position); // Without a physical knob.
// A built-in ringtone, or a library song (RINGTONE_FIRST_LOCAL and up),
// which is saved by file name and replaces the built-in one while it exists.
esp_err_t digits_phone_set_position(unsigned position, unsigned ringtone);
// The song was deleted: positions using it go back to their built-in ringtone.
esp_err_t digits_phone_forget_song(const char *name);
// Plays a ringtone on the speaker; only with the handset down and no call.
esp_err_t digits_phone_preview(unsigned ringtone);

// Console handler for: phone status | set a|b NUMBER | clear a|b |
// hook up|down | press a|b. Returns 0 on success, 1 on usage error.
int digits_phone_console(int argc, char **argv);
