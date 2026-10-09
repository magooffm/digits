#pragma once

// Shared clock/sample format; deliberately usable by host PCM tests too.
#define DIGITS_AUDIO_SAMPLE_RATE 48000
#define DIGITS_AUDIO_DMA_FRAMES 480 // 10 ms, two signed 16-bit slots.
