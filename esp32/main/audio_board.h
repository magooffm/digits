#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "audio_format.h"

// Boot-time initialization; microphone initialization is independent of DAC.
esp_err_t digits_audio_board_init(void);
i2c_master_bus_handle_t digits_audio_i2c_bus(void);
i2s_chan_handle_t digits_audio_rx_channel(void);

// Waveshare KEY1: TCA9555 P11, active low with an onboard 10 kOhm pull-up.
// Configuration preserves the adjacent P10 amplifier output. Reads are bounded.
esp_err_t digits_audio_record_button_init(void);
esp_err_t digits_audio_record_button_read(bool *pressed);

// Reserve output across recording/playback; never acquire from the WS callback.
bool digits_audio_speaker_acquire(uint32_t timeout_ms);
void digits_audio_speaker_release(void);
// Only the holder of the speaker reservation may call these functions.
esp_err_t digits_audio_speaker_start(void);
esp_err_t digits_audio_speaker_write(const int16_t *stereo, size_t bytes,
                                    size_t *written, uint32_t timeout_ms);
bool digits_audio_speaker_silence(void);
