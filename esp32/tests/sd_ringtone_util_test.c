// Host test for SD card ringtone helpers: the file naming pattern, ID3v2 tag
// sizes, MP3 length without decoding, downmix, silence and gain.
// Run by esp32/tests/run_host_tests.sh.
#include "sd_ringtone_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_names(void)
{
    assert(sd_ringtone_position("ringtone-03.mp3", 12) == 2);
    assert(sd_ringtone_position("ringtone-7.mp3", 12) == 6);
    assert(sd_ringtone_position("RINGTONE-12.MP3", 12) == 11);
    assert(sd_ringtone_position("Ringtone-01.Mp3", 12) == 0);
    assert(sd_ringtone_position("ringtone-12.mp3", 6) == -1);  // Past the switch's end stop.
    assert(sd_ringtone_position("ringtone-00.mp3", 12) == -1);
    assert(sd_ringtone_position("ringtone-003.mp3", 12) == -1);
    assert(sd_ringtone_position("ringtone-.mp3", 12) == -1);
    assert(sd_ringtone_position("ringtone-03.mp3.bak", 12) == -1);
    assert(sd_ringtone_position("ringtone-03.wav", 12) == -1);
    assert(sd_ringtone_position("ringtone03.mp3", 12) == -1);
    assert(sd_ringtone_position("._ringtone-03.mp3", 12) == -1); // macOS metadata file.
    assert(sd_ringtone_position("ring", 12) == -1);
    assert(sd_ringtone_position(NULL, 12) == -1);
}

static void test_id3(void)
{
    // Size 0x0101 = 129 bytes in syncsafe form: 0x00 0x00 0x01 0x01.
    const uint8_t v3[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 1, 1};
    assert(sd_ringtone_id3_size(v3, 10) == 10 + 129);
    const uint8_t v4_footer[10] = {'I', 'D', '3', 4, 0, 0x10, 0, 0, 1, 1};
    assert(sd_ringtone_id3_size(v4_footer, 10) == 10 + 129 + 10);
    const uint8_t frame[10] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0};
    assert(sd_ringtone_id3_size(frame, 10) == 0);
    const uint8_t broken[10] = {'I', 'D', '3', 3, 0, 0, 0x80, 0, 0, 0};
    assert(sd_ringtone_id3_size(broken, 10) == 0);
    assert(sd_ringtone_id3_size(v3, 9) == 0);
}

// MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo, no padding: 417-byte frames.
static void put_frame(uint8_t *at) { at[0] = 0xFF; at[1] = 0xFB; at[2] = 0x90; at[3] = 0x00; }
static void put_u32(uint8_t *at, uint32_t v)
{
    at[0] = (uint8_t)(v >> 24); at[1] = (uint8_t)(v >> 16); at[2] = (uint8_t)(v >> 8); at[3] = (uint8_t)v;
}

static void test_mp3_length(void)
{
    static uint8_t head[4096];
    uint32_t rate = 0;
    uint64_t samples = 0;
    // Constant bitrate: 10 s of 128 kbps after 7 bytes of junk.
    memset(head, 0, sizeof(head));
    for (size_t at = 7; at + 4 <= sizeof(head); at += 417) put_frame(head + at);
    assert(sd_ringtone_mp3_length(head, sizeof(head), 7 + 160000, &rate, &samples));
    assert(rate == 44100 && samples == 441000);
    // Xing (LAME) frame count wins: 1000 frames of 1152 samples.
    memcpy(head + 7 + 4 + 32, "Xing", 4);
    put_u32(head + 7 + 4 + 32 + 4, 0x0F);
    put_u32(head + 7 + 4 + 32 + 8, 1000);
    assert(sd_ringtone_mp3_length(head, sizeof(head), 7 + 160000, &rate, &samples) && samples == 1152000);
    memcpy(head + 7 + 4 + 32, "Info", 4); // CBR files carry "Info".
    assert(sd_ringtone_mp3_length(head, sizeof(head), 1, &rate, &samples) && samples == 1152000);
    put_u32(head + 7 + 4 + 32 + 4, 0x0E); // No frame count: fall back to the bitrate.
    assert(sd_ringtone_mp3_length(head, sizeof(head), 7 + 160000, &rate, &samples) && samples == 441000);
    // VBRI (Fraunhofer) frame count.
    memset(head + 7 + 4 + 32, 0, 12);
    memcpy(head + 7 + 4 + 32, "VBRI", 4);
    put_u32(head + 7 + 4 + 32 + 14, 500);
    assert(sd_ringtone_mp3_length(head, sizeof(head), 1, &rate, &samples) && samples == 576000);
    // A lone sync pattern in junk is not a frame: the next header must follow.
    memset(head, 0, sizeof(head));
    head[3] = 0xFF; head[4] = 0xFB; head[5] = 0x90; head[6] = 0x00;
    for (size_t at = 1000; at + 4 <= sizeof(head); at += 417) put_frame(head + at);
    assert(sd_ringtone_mp3_length(head, sizeof(head), 1000 + 16000, &rate, &samples) && samples == 44100);
    // MPEG-2 Layer III, 64 kbps, 22.05 kHz: 576-sample frames.
    memset(head, 0, sizeof(head));
    for (size_t at = 0; at + 4 <= sizeof(head); at += 208) {
        head[at] = 0xFF; head[at + 1] = 0xF3; head[at + 2] = 0x80; head[at + 3] = 0xC0;
    }
    assert(sd_ringtone_mp3_length(head, sizeof(head), 8000, &rate, &samples) && rate == 22050 && samples == 22050);
    // Not MP3 at all.
    memset(head, 0x55, sizeof(head));
    assert(!sd_ringtone_mp3_length(head, sizeof(head), sizeof(head), &rate, &samples));
}

static void test_pcm(void)
{
    int16_t stereo[] = {1000, 3000, -32768, -32768, 32767, -32768};
    assert(sd_ringtone_downmix(stereo, 3, 2, stereo) == 3); // In place.
    assert(stereo[0] == 2000 && stereo[1] == -32768 && stereo[2] == 0);
    int16_t mono[] = {5, -7}, out[2];
    assert(sd_ringtone_downmix(mono, 2, 1, out) == 2 && out[0] == 5 && out[1] == -7);
    assert(sd_ringtone_downmix(mono, 2, 0, out) == 0);

    int16_t quiet[] = {0, 3, -64, 64, -65, 9000};
    assert(sd_ringtone_first_sound(quiet, 6) == 4);
    assert(sd_ringtone_first_sound(quiet, 4) == 4);

    int16_t loud[] = {32767, -32768, 100};
    sd_ringtone_scale(loud, 3, 12);
    assert(loud[0] == 3932 && loud[1] == -3932 && loud[2] == 12);
    sd_ringtone_scale(loud, 3, 100); // Unchanged.
    assert(loud[0] == 3932);
}

int main(void)
{
    test_names();
    test_id3();
    test_mp3_length();
    test_pcm();
    puts("sd_ringtone_util_test: names, ID3 tags, MP3 length, downmix, silence and gain passed");
    return 0;
}
