// Real buffering/rendering logic; no simulated DTLS, hardware or audibility.
#include "call_audio_jitter.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static call_audio_packet_t frame(uint32_t pts, uint8_t value)
{
    call_audio_packet_t packet = {.pts_ms = pts, .size = 1};
    packet.data[0] = value;
    return packet;
}

int main(void)
{
    call_audio_jitter_t jitter;
    call_audio_packet_t output, packet;
    call_audio_jitter_init(&jitter);
    packet = frame(1000, 1);
    assert(call_audio_jitter_push(&jitter, &packet, 0));
    packet = frame(1040, 3); // One reordered packet then missing middle.
    assert(call_audio_jitter_push(&jitter, &packet, 30000));
    packet = frame(1020, 2);
    assert(call_audio_jitter_push(&jitter, &packet, 40000));
    assert(!call_audio_jitter_push(&jitter, &packet, 41000));
    assert(jitter.duplicate == 1);
    assert(call_audio_jitter_pop(&jitter, 59999, &output) == CALL_AUDIO_JITTER_WAIT);
    for (unsigned i = 0; i < 3; ++i) {
        assert(call_audio_jitter_pop(&jitter, 60000 + i * 20000, &output) == CALL_AUDIO_JITTER_PACKET);
        assert(output.pts_ms == 1000 + i * 20 && output.data[0] == i + 1);
    }
    assert(call_audio_jitter_pop(&jitter, 120000, &output) == CALL_AUDIO_JITTER_MISSING);
    packet = frame(1060, 4);
    assert(!call_audio_jitter_push(&jitter, &packet, 120001));
    assert(jitter.missing == 1 && jitter.late == 1);
    packet = frame(1080, 5);
    assert(call_audio_jitter_push(&jitter, &packet, 120010));
    assert(call_audio_jitter_pop(&jitter, 140000, &output) == CALL_AUDIO_JITTER_PACKET);
    assert(output.data[0] == 5);

    // A blocked playback worker skips old local slots, independent of remote
    // missing-packet counts. It selects one due packet without a burst.
    for (unsigned i = 0; i < 6; ++i) {
        packet = frame(1100 + i * 20, 10 + i);
        assert(call_audio_jitter_push(&jitter, &packet, 150000));
    }
    assert(call_audio_jitter_pop(&jitter, 240000, &output) == CALL_AUDIO_JITTER_PACKET);
    assert(output.pts_ms == 1180 && jitter.local_skips == 4 && jitter.missing == 1);
    assert(call_audio_jitter_pop(&jitter, 240001, &output) == CALL_AUDIO_JITTER_WAIT);

    // Timestamp discontinuity is bounded to a fresh 60 ms priming window.
    packet = frame(9000, 99);
    assert(call_audio_jitter_push(&jitter, &packet, 250000));
    assert(jitter.discontinuities == 1);
    assert(call_audio_jitter_pop(&jitter, 309999, &output) == CALL_AUDIO_JITTER_WAIT);
    assert(call_audio_jitter_pop(&jitter, 310000, &output) == CALL_AUDIO_JITTER_PACKET);
    assert(output.data[0] == 99);
    for (unsigned i = 0; i < 10; ++i)
        assert(call_audio_jitter_pop(&jitter, 330000 + i * 20000, &output) == CALL_AUDIO_JITTER_MISSING);
    assert(!jitter.primed);
    assert(call_audio_jitter_pop(&jitter, 1000000, &output) == CALL_AUDIO_JITTER_WAIT);

    // esp_peer's raw-RTP-to-ms conversion rolls backwards near RTP wrap.
    call_audio_jitter_init(&jitter);
    packet = frame(89478480, 1);
    assert(call_audio_jitter_push(&jitter, &packet, 0));
    assert(call_audio_jitter_pop(&jitter, 60000, &output) == CALL_AUDIO_JITTER_PACKET);
    packet = frame(14, 2);
    assert(call_audio_jitter_push(&jitter, &packet, 80000));
    assert(jitter.discontinuities == 1);
    assert(call_audio_jitter_pop(&jitter, 140000, &output) == CALL_AUDIO_JITTER_PACKET);
    assert(output.pts_ms == 14);

    // Priming accepts an earlier reordered frame and remains fixed-size.
    call_audio_jitter_init(&jitter);
    packet = frame(40, 3);
    assert(call_audio_jitter_push(&jitter, &packet, 0));
    packet = frame(20, 2);
    assert(call_audio_jitter_push(&jitter, &packet, 1000));
    packet = frame(0, 1);
    assert(call_audio_jitter_push(&jitter, &packet, 2000));
    assert(call_audio_jitter_pop(&jitter, 60000, &output) == CALL_AUDIO_JITTER_PACKET);
    assert(output.pts_ms == 0);
    packet = frame(10000, 4);
    packet.size = CALL_AUDIO_OPUS_MAX_BYTES + 1;
    assert(!call_audio_jitter_push(&jitter, &packet, 80000));

    int16_t input[] = {INT16_MIN, -3, 0, 3, INT16_MAX};
    int16_t stereo[10];
    call_audio_pcm_stereo(input, stereo, 5);
    for (unsigned i = 0; i < 5; ++i)
        assert(stereo[2 * i] == input[i] / 2 && stereo[2 * i + 1] == input[i] / 2);
    puts("Call jitter: reorder, duplicate, late/missing, scheduling stall, bounded resync, RTP wrap and gain passed");
    return 0;
}
