"""Run production call-audio workers with finite capture, codec and 2-DMA mocks.

Checks lifecycle/resource cleanup, PCM routing, bounded queues, fixed-clock TX
pacing and playback producer scheduling. This does not test real Opus, I2S,
concurrent ESP32 scheduling, transport or audible hardware interoperability.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
codec = repo / "esp32/managed_components/espressif__esp_audio_codec/include"
if not (codec / "encoder/impl/esp_opus_enc.h").is_file():
    parser.error("Install the pinned esp_audio_codec dependency with idf.py build first")

headers = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
""",
    "driver/i2c_master.h": "#pragma once\ntypedef void *i2c_master_bus_handle_t;\n",
    "driver/i2s_std.h": "#pragma once\ntypedef void *i2s_chan_handle_t;\n",
    "esp_heap_caps.h": """
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
""",
    "esp_log.h": """
#pragma once
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
""",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "freertos/FreeRTOS.h": """
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdTRUE 1
#define pdPASS 1
""",
    "freertos/queue.h": """
#pragma once
#include <stddef.h>
typedef struct { unsigned length,size,head,count; unsigned char *storage; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(unsigned length,unsigned size,unsigned char *storage,StaticQueue_t *control);
int xQueueSend(QueueHandle_t queue,const void *item,unsigned wait);
int xQueueReceive(QueueHandle_t queue,void *item,unsigned wait);
void vQueueDelete(QueueHandle_t queue);
""",
    "freertos/task.h": """
#pragma once
typedef void *TaskHandle_t;
int xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out);
void vTaskDelay(unsigned ticks);
void vTaskDelete(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
""",
}

harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "call_audio.c"

static jmp_buf done;
static digits_call_audio_t *active;
static void (*task_entry[2])(void *);
static void *task_args[2];
static unsigned tasks, queues, codecs, mic_reads, tx_sent, speaker_writes;
static unsigned creates, stops, peak_tasks;
static bool mic_owned, speaker_owned, amplifier, mic_busy, speaker_busy;
static bool fail_codec, fail_task, fail_alloc, inject_loss, inject_stall, partial_write;
static bool stall_injected;
static bool stop_during_decode, stop_after_first_write;
static int64_t now_us, next_arrival_us, dma_until_us, initial_playback_us;
static unsigned receive_sequence, simulated_underflows, last_pcm;

int64_t esp_timer_get_time(void) { return now_us; }
void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) || caps == MALLOC_CAP_8BIT);
    return fail_alloc ? NULL : calloc(count,size);
}
QueueHandle_t xQueueCreateStatic(unsigned length,unsigned size,unsigned char *storage,StaticQueue_t *control)
{
    assert(storage && control && length <= 8 && size == sizeof(call_audio_packet_t));
    *control = (StaticQueue_t){.length=length,.size=size,.storage=storage};
    ++queues;
    return control;
}
int xQueueSend(QueueHandle_t queue,const void *item,unsigned wait)
{
    assert(wait == 0 && queue->count <= queue->length);
    if (queue->count == queue->length) return 0;
    memcpy(queue->storage + ((queue->head + queue->count) % queue->length) * queue->size,item,queue->size);
    ++queue->count;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue,void *item,unsigned wait)
{
    assert(wait == 0);
    if (!queue->count) return 0;
    memcpy(item,queue->storage + queue->head * queue->size,queue->size);
    queue->head = (queue->head + 1) % queue->length;
    --queue->count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue && queues); --queues; }
int xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out)
{
    (void)out;
    assert(stack == (strcmp(name,"digits_call_tx") == 0 ?
        ENCODER_WORKER_STACK_BYTES : DECODER_WORKER_STACK_BYTES));
    assert(priority == 5 && tasks < 2);
    if (fail_task) return 0;
    task_entry[tasks] = entry;
    task_args[tasks++] = arg;
    if (tasks > peak_tasks) peak_tasks = tasks;
    return pdPASS;
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 9000; }
void vTaskDelete(TaskHandle_t task) { assert(!task); longjmp(done,1); }

// Answering machine hooks: a recognizable greeting frame and a packet count.
#define GREETING_SAMPLE 7000
static unsigned fills, recorded_packets, filled_encodes, live_encodes, attend_at_read;
static const digits_call_audio_unattended_t *start_hooks;
static void test_fill(int16_t *mono, size_t frames)
{
    assert(frames == DIGITS_MIC_FRAME_SAMPLES);
    for (size_t i=0;i<frames;++i) mono[i] = (int16_t)(GREETING_SAMPLE + i);
    ++fills;
}
static void test_record(const uint8_t *packet, size_t size) { assert(packet && size == 1); ++recorded_packets; }
static const digits_call_audio_unattended_t test_hooks = {.fill = test_fill, .record = test_record};

static void inject_packets(void)
{
    if (!active || !active->rx_packets) return;
    while (next_arrival_us <= now_us) {
        uint8_t encoded = (uint8_t)(receive_sequence + 1);
        if (!inject_loss || receive_sequence % 5 != 2)
            digits_call_audio_receive(active,receive_sequence * 20,&encoded,1);
        ++receive_sequence;
        next_arrival_us += 20000;
    }
    if (now_us >= 600000) digits_call_audio_request_stop(active);
}
void vTaskDelay(unsigned ticks)
{
    assert(ticks == 1);
    now_us += 1000;
    if (inject_stall && !stall_injected && now_us >= 150000) {
        stall_injected=true;
        int64_t resume_at=now_us+100000;
        // Peer callbacks continue copying packets while playback is blocked.
        while (next_arrival_us < resume_at) {
            now_us=next_arrival_us;
            inject_packets();
        }
        now_us=resume_at;
    }
    inject_packets();
}
bool digits_microphone_acquire(uint32_t wait)
{
    assert(wait == 0 && !mic_owned);
    if (mic_busy) return false;
    mic_owned = true;
    return true;
}
void digits_microphone_release(void) { assert(mic_owned); mic_owned = false; }
esp_err_t digits_microphone_read(digits_microphone_frame_t *frame,uint32_t wait)
{
    assert(mic_owned && (wait == 0 || wait == 40));
    if (!wait) return ESP_ERR_TIMEOUT;
    call_audio_packet_t packet;
    if (digits_call_audio_pop_encoded(active,&packet)) {
        assert(packet.size == 3 && packet.data[0] != 0);
        assert(packet.pts_ms == (packet.data[0] - 1U) * 20);
        digits_call_audio_record_send(active,&packet,true,false);
        ++tx_sent;
    }
    if (mic_reads == 30) {
        digits_call_audio_request_stop(active);
        return ESP_ERR_TIMEOUT;
    }
    now_us += 20000;
    ++mic_reads;
    *frame = (digits_microphone_frame_t){.sequence=mic_reads,.captured_at_us=now_us};
    for (unsigned i=0;i<DIGITS_MIC_FRAME_SAMPLES;++i) frame->pcm[i] = mic_reads + i;
    if (attend_at_read && mic_reads == attend_at_read) {
        uint8_t one=1;
        assert(digits_call_audio_receive(active,40,&one,1) && recorded_packets == 2);
        digits_call_audio_attend(active); // Handset lifted mid-call.
        assert(digits_call_audio_receive(active,60,&one,1) && recorded_packets == 2);
        assert(active->rx_packets->count == 1);
    }
    return ESP_OK;
}
bool digits_audio_speaker_acquire(uint32_t wait)
{
    assert(wait == 0 && !speaker_owned);
    if (speaker_busy) return false;
    speaker_owned = true;
    return true;
}
void digits_audio_speaker_release(void) { assert(speaker_owned && !amplifier); speaker_owned = false; }
esp_err_t digits_audio_speaker_start(void)
{
    assert(speaker_owned);
    amplifier = true;
    if (!initial_playback_us) initial_playback_us = now_us;
    dma_until_us = 0;
    return ESP_OK;
}
bool digits_audio_speaker_silence(void) { assert(speaker_owned); amplifier = false; dma_until_us = 0; return true; }
esp_err_t digits_audio_speaker_write(const int16_t *stereo,size_t bytes,size_t *written,uint32_t timeout)
{
    assert(speaker_owned && amplifier && timeout == 30 && bytes == 1920);
    ++speaker_writes;
    for (unsigned i=0;i<480;++i) assert(stereo[2*i] == stereo[2*i+1]);
    last_pcm = (unsigned)(stereo[0] >= 0 ? stereo[0] : -stereo[0]);
    assert(last_pcm > 0 && last_pcm <= 16384);
    if (partial_write && speaker_writes == 5) { *written = 20; return ESP_ERR_TIMEOUT; }
    // Hardware holds exactly two 10 ms descriptors. Wait until one is free;
    // I2S continuously drains while this independent worker is blocked.
    if (dma_until_us > now_us + 10000) {
        now_us = dma_until_us - 10000;
        inject_packets();
    }
    if (dma_until_us && now_us > dma_until_us) ++simulated_underflows;
    if (dma_until_us < now_us) dma_until_us = now_us;
    dma_until_us += 10000;
    *written = bytes;
    if (stop_after_first_write && speaker_writes == 1)
        digits_call_audio_request_stop(active);
    return ESP_OK;
}
esp_audio_err_t esp_opus_enc_open(void *cfg,uint32_t size,void **handle)
{
    esp_opus_enc_config_t *settings = cfg;
    assert(size == sizeof(*settings) && settings->sample_rate == 48000 && settings->channel == 1);
    assert(settings->frame_duration == ESP_OPUS_ENC_FRAME_DURATION_20_MS && settings->complexity == 0);
    if (fail_codec) return ESP_AUDIO_ERR_MEM_LACK;
    *handle = malloc(1); assert(*handle); ++codecs;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_get_frame_size(void *handle,int *input,int *output)
{ assert(handle); *input=1920; *output=80; return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_opus_enc_process(void *handle,esp_audio_enc_in_frame_t *in,esp_audio_enc_out_frame_t *out)
{
    assert(handle && in->len == 1920 && out->len == 1275);
    int16_t *pcm = (int16_t *)in->buffer;
    assert(pcm[0] > 0 && pcm[959] == pcm[0] + 959); // Live captured PCM, not silence.
    if (pcm[0] == GREETING_SAMPLE) ++filled_encodes; // The room microphone was replaced.
    else { assert(pcm[0] == (int16_t)mic_reads); ++live_encodes; }
    out->buffer[0] = (uint8_t)mic_reads; out->buffer[1] = 1; out->buffer[2] = 2;
    out->encoded_bytes = 3;
    now_us += 1000;
    return ESP_AUDIO_ERR_OK;
}
void esp_opus_enc_close(void *handle) { assert(handle && codecs); free(handle); --codecs; }
esp_audio_err_t esp_opus_dec_open(void *cfg,uint32_t size,void **handle)
{
    esp_opus_dec_cfg_t *settings=cfg;
    assert(size == sizeof(*settings) && settings->sample_rate == 48000 && settings->channel == 1);
    if (fail_codec) return ESP_AUDIO_ERR_MEM_LACK;
    *handle=malloc(1); assert(*handle); ++codecs;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_decode(void *handle,esp_audio_dec_in_raw_t *in,esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info)
{
    assert(handle && in->buffer && out->len == 1920);
    assert((in->len == 0) == (in->frame_recover == ESP_AUDIO_DEC_RECOVERY_PLC));
    for (unsigned i=0;i<960;++i) ((int16_t *)out->buffer)[i] = in->len ? 2000 : 100;
    out->decoded_size=1920;
    *info=(esp_audio_dec_info_t){.sample_rate=48000,.channel=1,.bits_per_sample=16};
    now_us += 1000;
    if (stop_during_decode) digits_call_audio_request_stop(active);
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_reset(void *handle) { assert(handle); return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_opus_dec_close(void *handle) { assert(handle && codecs); free(handle); --codecs; return ESP_AUDIO_ERR_OK; }

static void begin(digits_call_audio_mode_t mode)
{
    now_us=1000000;
    next_arrival_us=now_us;
    receive_sequence=0;
    dma_until_us=0;
    initial_playback_us=0;
    stall_injected=false;
    mic_reads=tx_sent=speaker_writes=simulated_underflows=0;
    tasks=0;
    assert(digits_call_audio_start_unattended(mode,start_hooks,&active) == ESP_OK && active);
    ++creates;
}
static void run(unsigned index)
{
    assert(index < tasks);
    if (!setjmp(done)) task_entry[index](task_args[index]);
}
static digits_call_audio_stats_t finish(void)
{
    assert(digits_call_audio_is_stopped(active));
    digits_call_audio_stats_t stats;
    digits_call_audio_get_stats(active,&stats);
    assert(digits_call_audio_destroy(active) == ESP_OK);
    active=NULL;
    assert(!queues && !codecs && !mic_owned && !speaker_owned && !amplifier);
    ++stops;
    return stats;
}

int main(void)
{
    begin(DIGITS_CALL_AUDIO_TX);
    assert(digits_call_audio_destroy(active) == ESP_ERR_INVALID_STATE);
    run(0);
    digits_call_audio_stats_t stats=finish();
    assert(stats.capture_frames == 30 && stats.encoded_frames == 30 && stats.sent_frames == 30);
    assert(!stats.capture_gaps && !stats.encode_errors && stats.tx_stack_min_bytes == 9000);

    // With captures at50Hz and peer polls at6ms, relative now+20ms deadlines
    // would send every24ms. Fixed capture-clock slots must retain every frame.
    begin(DIGITS_CALL_AUDIO_TX);
    int64_t next_capture=1000000;
    unsigned captured=0,sent=0;
    for (now_us=1000000;now_us<21010000;now_us+=6000) {
        while (captured < 1000 && next_capture + 1000 <= now_us) {
            call_audio_packet_t packet={.pts_ms=captured*20,.size=1,.captured_at_us=next_capture};
            assert(xQueueSend(active->tx_packets,&packet,0));
            ++captured; next_capture+=20000;
        }
        call_audio_packet_t packet;
        if (digits_call_audio_pop_encoded(active,&packet)) {
            assert(packet.pts_ms == sent*20);
            ++sent;
        }
    }
    assert(captured == 1000 && sent == 1000 && active->stats.tx_pop_drops == 0);
    // Stall/backlog: latest only, no immediate follow-up catchup burst.
    for (unsigned i=0;i<3;++i) {
        call_audio_packet_t packet={.pts_ms=20000+i*20,.size=1,.captured_at_us=now_us-40000+i*20000};
        assert(xQueueSend(active->tx_packets,&packet,0));
    }
    call_audio_packet_t latest;
    assert(digits_call_audio_pop_encoded(active,&latest) && latest.pts_ms == 20040);
    assert(active->stats.tx_pop_drops == 2);
    assert(!digits_call_audio_pop_encoded(active,&latest));
    digits_call_audio_request_stop(active); run(0); finish();

    for (unsigned repeat=0;repeat<100;++repeat) {
        begin(DIGITS_CALL_AUDIO_RX);
        // Make timer relative to test receive stream while keeping TX cadence
        // test above independent from playback's 600ms stop fixture.
        now_us=0; next_arrival_us=0;
        inject_loss=repeat%2;
        run(0); stats=finish();
        assert(stats.decoded_frames > 15 && stats.played_frames > 20 && !stats.decode_errors);
        assert(initial_playback_us >= 60000 && initial_playback_us <= 64000);
        assert(!simulated_underflows && !stats.playback_feed_gaps && !stats.playback_errors);
        if (inject_loss) assert(stats.plc_frames > 0 && stats.plc_frames == stats.rx_missing);
    }
    inject_loss=false;

    inject_stall=true;
    begin(DIGITS_CALL_AUDIO_RX); now_us=0; next_arrival_us=0;
    run(0); stats=finish();
    assert(stats.rx_local_skips > 0 && stats.rx_queue_stale > 0);
    assert(stats.playback_feed_gaps > 0 && simulated_underflows > 0);
    assert(stats.decoded_frames > 10 && !stats.decode_errors);
    inject_stall=false;

    partial_write=true;
    begin(DIGITS_CALL_AUDIO_RX); now_us=0; next_arrival_us=0;
    run(0); stats=finish();
    assert(stats.playback_errors == 1 && stats.rx_discontinuities > 0);
    assert(stats.decoded_frames > 10 && !stats.decode_errors);
    partial_write=false;

    stop_after_first_write=true;
    begin(DIGITS_CALL_AUDIO_RX); now_us=0; next_arrival_us=0;
    run(0); stats=finish();
    assert(speaker_writes == 1 && stats.decoded_frames == 1);
    assert(stats.played_frames == 0 && stats.playback_errors == 0);
    stop_after_first_write=false;

    stop_during_decode=true;
    begin(DIGITS_CALL_AUDIO_RX); now_us=0; next_arrival_us=0;
    run(0); stats=finish();
    assert(stats.decoded_frames == 1 && !initial_playback_us && !speaker_writes);
    assert(stats.played_frames == 0 && stats.playback_errors == 0);
    stop_during_decode=false;

    begin(DIGITS_CALL_AUDIO_RX);
    uint8_t bounded_packet=1;
    assert(!digits_call_audio_receive(active,0,NULL,1));
    assert(!digits_call_audio_receive(active,0,&bounded_packet,1276));
    for (unsigned i=0;i<8;++i)
        assert(digits_call_audio_receive(active,i*20,&bounded_packet,1));
    assert(!digits_call_audio_receive(active,160,&bounded_packet,1));
    digits_call_audio_request_stop(active); run(0); stats=finish();
    assert(stats.rx_invalid == 2 && stats.rx_queue_drops == 1);

    begin(DIGITS_CALL_AUDIO_FULL);
    assert(!digits_call_audio_is_stopped(active));
    digits_call_audio_request_stop(active);
    run(0);
    assert(!digits_call_audio_is_stopped(active)); // RX acknowledgement still required.
    assert(digits_call_audio_destroy(active) == ESP_ERR_INVALID_STATE);
    run(1); finish();

    mic_busy=true; begin(DIGITS_CALL_AUDIO_TX); run(0); stats=finish();
    assert(stats.state == DIGITS_CALL_AUDIO_FAILED && stats.error == ESP_ERR_INVALID_STATE);
    mic_busy=false;
    speaker_busy=true; begin(DIGITS_CALL_AUDIO_RX); run(0); stats=finish();
    assert(stats.state == DIGITS_CALL_AUDIO_FAILED && stats.error == ESP_ERR_INVALID_STATE);
    speaker_busy=false;
    fail_codec=true; begin(DIGITS_CALL_AUDIO_FULL); run(0); run(1); stats=finish();
    assert(stats.state == DIGITS_CALL_AUDIO_FAILED && stats.error == ESP_ERR_NO_MEM);
    fail_codec=false;
    fail_task=true; begin(DIGITS_CALL_AUDIO_FULL); assert(!tasks); stats=finish();
    assert(stats.state == DIGITS_CALL_AUDIO_FAILED && stats.error == ESP_ERR_NO_MEM);
    fail_task=false;
    fail_alloc=true;
    assert(digits_call_audio_start(DIGITS_CALL_AUDIO_FULL,&active) == ESP_ERR_NO_MEM && !active && !queues);
    fail_alloc=false;

    // Answering machine: from the first frame the greeting replaces the room
    // microphone and received packets are recorded, not played. Attending
    // (handset lifted) restores both from the next frame.
    digits_call_audio_unattended_t half = {.fill = test_fill};
    assert(digits_call_audio_start_unattended(DIGITS_CALL_AUDIO_FULL,&half,&active) == ESP_ERR_INVALID_ARG && !active);
    filled_encodes=live_encodes=0;
    start_hooks=&test_hooks;
    attend_at_read=10;
    begin(DIGITS_CALL_AUDIO_FULL);
    start_hooks=NULL;
    uint8_t one=1;
    assert(digits_call_audio_receive(active,0,&one,1) && recorded_packets == 1 && !active->rx_packets->count);
    run(0);
    assert(filled_encodes == 9 && live_encodes == 21 && fills == 9);
    digits_call_audio_attend(active); // Repeated attend is harmless.
    run(1); stats=finish();
    assert(stats.received_frames == 3 && stats.encoded_frames == 30 && !stats.rx_queue_drops);
    attend_at_read=0;
    assert(creates == stops && peak_tasks == 2);
    printf("Call audio workers: PCM/PLC routing, 1000 fixed-clock TX frames, 100 playback cycles, two-DMA cadence, unattended answering and async cleanup/failure paths passed\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-call-audio-") as temporary:
    build = Path(temporary)
    for name, content in headers.items():
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    (build / "test.c").write_text(harness)
    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if sys.platform.startswith("linux"):
            command += ["-no-pie"]
    for include in [build, repo / "esp32/main", codec, codec / "encoder", codec / "encoder/impl",
                    codec / "decoder", codec / "decoder/impl"]:
        command += ["-I", str(include)]
    command += [str(build / "test.c"), str(repo / "esp32/main/call_audio_jitter.c"), "-o", str(build / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(build / "test")], check=True)
