#pragma once

#include <stdint.h>
#include "esp_err.h"

// The board's ring of WS2812 LEDs (Waveshare: 7 on GPIO38), showing the
// call as a traffic light: yellow with dial tone, orange flashing while a
// call rings (here or at the other end), green during a call, red when a
// call ends, off otherwise. Until the server first answers after boot a
// blue dot circles; a firmware upload fills the ring blue. Disabled when
// CONFIG_DIGITS_LED_RING_GPIO is -1.

typedef enum {
    DIGITS_LED_RING_OFF,
    DIGITS_LED_RING_STARTING, // Server not reached since boot.
    DIGITS_LED_RING_LIFTED,   // Dial tone.
    DIGITS_LED_RING_RINGING,
    DIGITS_LED_RING_CALL,
    DIGITS_LED_RING_ENDED, // Busy or reorder tone.
} digits_led_ring_mode_t;

#define DIGITS_LED_RING_UPLOAD_DONE (-1)
#define DIGITS_LED_RING_UPLOAD_FAILED (-2) // Red for a moment.

// Boot-only; switches the ring off (it may light up at power-on).
esp_err_t digits_led_ring_init(void);
// Call often (the phone task's poll); writes the LEDs only on changes.
void digits_led_ring_show(digits_led_ring_mode_t mode, int64_t now_us);
// A call ended: red for a moment, unless the next call already rings.
void digits_led_ring_call_ended(int64_t now_us);
// Firmware upload progress 0-100, drawn over everything else by show(), or
// one of the DIGITS_LED_RING_UPLOAD_* values. Any task.
void digits_led_ring_upload(int percent);
