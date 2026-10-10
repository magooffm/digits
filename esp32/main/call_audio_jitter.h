#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CALL_AUDIO_FRAME_MS 20U
#define CALL_AUDIO_OPUS_MAX_BYTES 1275U
#define CALL_AUDIO_JITTER_SLOTS 8U
#define CALL_AUDIO_JITTER_DELAY_US 60000LL

typedef struct {
    uint32_t pts_ms;
    uint16_t size;
    int64_t captured_at_us; // Local capture time for TX; arrival time for RX.
    uint8_t data[CALL_AUDIO_OPUS_MAX_BYTES];
} call_audio_packet_t;

typedef struct {
    bool occupied;
    call_audio_packet_t packet;
} call_audio_jitter_slot_t;

typedef struct {
    call_audio_jitter_slot_t slots[CALL_AUDIO_JITTER_SLOTS];
    bool primed;
    bool playing;
    uint32_t expected_ms;
    int64_t due_us;
    unsigned consecutive_missing;
    uint32_t late, duplicate, overflow, missing, discontinuities, local_skips;
} call_audio_jitter_t;

typedef enum {
    CALL_AUDIO_JITTER_WAIT,
    CALL_AUDIO_JITTER_PACKET,
    CALL_AUDIO_JITTER_MISSING,
} call_audio_jitter_result_t;

// Single playback-worker owner. Timestamp subtraction handles uint32 rollover.
void call_audio_jitter_init(call_audio_jitter_t *jitter);
bool call_audio_jitter_push(call_audio_jitter_t *jitter, const call_audio_packet_t *packet,
                            int64_t now_us);
call_audio_jitter_result_t call_audio_jitter_pop(call_audio_jitter_t *jitter,
                                               int64_t now_us, call_audio_packet_t *packet);

// Conservative half-amplitude mono -> stereo, including INT16_MIN.
void call_audio_pcm_stereo(const int16_t *mono, int16_t *stereo, size_t samples);
