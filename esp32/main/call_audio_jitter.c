#include "call_audio_jitter.h"
#include <string.h>

void call_audio_jitter_init(call_audio_jitter_t *jitter)
{
    memset(jitter, 0, sizeof(*jitter));
}

static void discard_slots(call_audio_jitter_t *jitter)
{
    for (unsigned i = 0; i < CALL_AUDIO_JITTER_SLOTS; ++i)
        jitter->slots[i].occupied = false;
}

static void prime(call_audio_jitter_t *jitter, uint32_t pts_ms, int64_t now_us)
{
    discard_slots(jitter);
    jitter->expected_ms = pts_ms;
    jitter->due_us = now_us + CALL_AUDIO_JITTER_DELAY_US;
    jitter->consecutive_missing = 0;
    jitter->playing = false;
    jitter->primed = true;
}

bool call_audio_jitter_push(call_audio_jitter_t *jitter, const call_audio_packet_t *packet,
                            int64_t now_us)
{
    if (!packet->size || packet->size > CALL_AUDIO_OPUS_MAX_BYTES) return false;
    if (!jitter->primed) prime(jitter, packet->pts_ms, now_us);
    int32_t delta = (int32_t)(packet->pts_ms - jitter->expected_ms);
    // esp_peer converts the raw 32-bit RTP timestamp to milliseconds before
    // this callback, so RTP wrap is a large backwards jump, not uint32 ms wrap.
    if (delta < -10000) {
        ++jitter->discontinuities;
        prime(jitter, packet->pts_ms, now_us);
        delta = 0;
    }
    if (delta < 0) {
        // Permit early packet reordering during the initial 60 ms window.
        if (!jitter->playing && delta >= -(int32_t)(CALL_AUDIO_JITTER_SLOTS * CALL_AUDIO_FRAME_MS))
            jitter->expected_ms = packet->pts_ms;
        else {
            ++jitter->late;
            return false;
        }
    } else if (delta >= (int32_t)(CALL_AUDIO_JITTER_SLOTS * CALL_AUDIO_FRAME_MS)) {
        // Never accumulate unbounded latency after a timestamp jump/stall.
        ++jitter->discontinuities;
        prime(jitter, packet->pts_ms, now_us);
    }
    // Peer audio is negotiated as one 20 ms Opus packet. Reject misaligned
    // timestamps instead of retaining a packet that can never be selected.
    if ((packet->pts_ms - jitter->expected_ms) % CALL_AUDIO_FRAME_MS) {
        ++jitter->discontinuities;
        return false;
    }
    int empty = -1;
    for (unsigned i = 0; i < CALL_AUDIO_JITTER_SLOTS; ++i) {
        if (!jitter->slots[i].occupied) empty = (int)i;
        else if (jitter->slots[i].packet.pts_ms == packet->pts_ms) {
            ++jitter->duplicate;
            return false;
        }
    }
    if (empty < 0) {
        ++jitter->overflow;
        return false;
    }
    jitter->slots[empty].packet = *packet;
    jitter->slots[empty].occupied = true;
    return true;
}

call_audio_jitter_result_t call_audio_jitter_pop(call_audio_jitter_t *jitter,
                                               int64_t now_us, call_audio_packet_t *packet)
{
    if (!jitter->primed || now_us < jitter->due_us) return CALL_AUDIO_JITTER_WAIT;
    jitter->playing = true;
    // Local playback scheduling delays are counted separately from packets
    // absent at their deadline. Skip expired slots; never burst old playback.
    uint32_t skipped = (uint32_t)((now_us - jitter->due_us) / (CALL_AUDIO_FRAME_MS * 1000LL));
    if (skipped) {
        jitter->local_skips += skipped;
        jitter->expected_ms += skipped * CALL_AUDIO_FRAME_MS;
        jitter->due_us += skipped * CALL_AUDIO_FRAME_MS * 1000LL;
        for (unsigned i = 0; i < CALL_AUDIO_JITTER_SLOTS; ++i)
            if (jitter->slots[i].occupied &&
                (int32_t)(jitter->slots[i].packet.pts_ms - jitter->expected_ms) < 0)
                jitter->slots[i].occupied = false;
    }
    bool found = false;
    for (unsigned i = 0; i < CALL_AUDIO_JITTER_SLOTS; ++i) {
        if (jitter->slots[i].occupied && jitter->slots[i].packet.pts_ms == jitter->expected_ms) {
            *packet = jitter->slots[i].packet;
            jitter->slots[i].occupied = false;
            found = true;
            break;
        }
    }
    jitter->expected_ms += CALL_AUDIO_FRAME_MS;
    jitter->due_us += CALL_AUDIO_FRAME_MS * 1000LL;
    if (found) {
        jitter->consecutive_missing = 0;
        return CALL_AUDIO_JITTER_PACKET;
    }
    ++jitter->missing;
    // After 200 ms without packets, wait for a fresh priming window instead
    // of synthesizing audio forever or expanding the queue latency.
    if (++jitter->consecutive_missing >= 10) {
        jitter->primed = false;
        discard_slots(jitter);
    }
    return CALL_AUDIO_JITTER_MISSING;
}

void call_audio_pcm_stereo(const int16_t *mono, int16_t *stereo, size_t samples)
{
    for (size_t i = 0; i < samples; ++i) {
        int16_t pcm = mono[i] / 2;
        stereo[i * 2] = pcm;
        stereo[i * 2 + 1] = pcm;
    }
}
