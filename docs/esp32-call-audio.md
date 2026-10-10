# ESP32 call audio implementation and validation

The ESP32 call path now connects the existing ES7210 microphone and ES8311
speaker to the established Digits/`esp_peer` connection. Implementation and
host validation are available. **Physical tests confirmed clear received speech
during full-duplex capture.** ESP32 speech recorded and decoded by Pion was
also confirmed intelligible when replayed through the speaker after offline
gain/filter processing. A clean acoustic-loop probe estimated about 170 ms
round trip. Native microphone transmit gain remains unchanged; raw recording
level, calibrated one-way latency and longer-term timing still need assessment.
The [nine-call transport baseline](esp32-webrtc-interoperability.md#live-hardware-verification)
used encoded silence and packet counters. Its 15 audible Ring Tests verify the
local speaker diagnostic, rather than speech over WebRTC.

The Digits server, original Pi client and Pico firmware are unchanged. This
implementation covers one peer on the existing LAN connection. Handset wiring,
hook handling, group calls, TURN and signaling-loss media recovery remain
outside this work.

## Media path

[`call_audio.c`](../esp32/main/call_audio.c) uses the direct Opus APIs from
`espressif/esp_audio_codec` 2.6.2. No capture/render framework is added, and the
code reuses the existing I²S channels and codec control interfaces.

| Direction | Processing |
| --- | --- |
| ESP32 to Pion | ES7210 stereo capture selects the configured mono microphone; each frame contains 960 signed 16-bit samples at 48 kHz. A dedicated task encodes one 20 ms Opus packet. |
| Pion to ESP32 | The peer callback copies encoded audio into a bounded queue. A playback task orders packets by their media timestamp, decodes 20 ms mono PCM, applies half amplitude and duplicates it into the ES8311's two I²S slots. |
| Full duplex | Capture/encoding and decode/playback run simultaneously, while the existing native capture task continuously drains RX. |

The ESP32 encoder uses VOIP mode, 32 kbit/s CBR, complexity 0, with FEC and DTX
disabled. These are local ESP32 settings. The unchanged upstream Pi/Pion
encoder uses 24 kbit/s with FEC and DTX. SDP still advertises `opus/48000/2`;
local PCM and decoder output are mono. This call path expects 20 ms packets.
Invalid or unexpected decoded formats increment errors and render silence.

The encoder holds the microphone's single-consumer reservation for the call.
Source PTS advances from the capture sequence in 20 ms steps, including frames
skipped locally. Read-completion timestamps identify stale PCM; they are not
calibrated ADC sampling timestamps. Encoding does not change the microphone
gain, add AGC or use the saved-recording diagnostic's normalization.

## Ownership and bounded work

[`webrtc.c`](../esp32/main/webrtc.c) remains the only owner of `esp_peer`.
WebSocket callbacks still copy signaling into its bounded queue. The peer
worker starts audio only after the connection reports `CONNECTED`; no I²S
read/write or Opus operation runs in the WebSocket or peer audio callbacks.
The encoder uses a 48 KiB internal task stack and the decoder uses 24 KiB,
both at priority 5.
Bulk PCM, packet storage and jitter slots prefer PSRAM, with internal fallback.

The four-frame microphone queue drops its oldest frame when full. The encoder
keeps current PCM and discards capture or encoded media at least 60 ms old.
Its outgoing queue holds three encoded packets. The peer polls the latest
available packet, advances a deadline anchored to capture time and retains a
10 ms minimum send spacing after a stall. It sends no catch-up burst and does
not retry a blocked packet indefinitely.

The peer's incoming queue holds eight copied Opus packets of at most 1,275
bytes each. [`call_audio_jitter.c`](../esp32/main/call_audio_jitter.c) has eight
slots spanning a 160 ms source-timestamp window and a 60 ms initial playout
target. It permits early reordering, rejects duplicate/late or misaligned
packets and rebases large timestamp discontinuities. The pinned peer converts
the raw RTP timestamp into milliseconds; the application also handles its
large backwards jump when the underlying RTP timestamp wraps.

Missing frames use the Opus decoder's PLC path for one 20 ms frame. Ten
consecutive missing frames, or 200 ms, end the current playout window; output
is muted until fresh packets prime it again. Decode errors produce zero PCM.
Local playback delays skip expired slots rather than expanding latency or
playing a backlog. Queue capacity and the initial target are design limits,
not measured end-to-end latency. The peer's own 8 KiB/100 ms receive cache
can also affect delivery under loss or reordering.

The existing I²S output has two 10 ms DMA descriptors. After the initial
60 ms prime, the playback worker feeds subsequent PCM one descriptor early
so decoding/writing can finish while queued output remains. A producer gap
longer than the 20 ms descriptor capacity is counted separately. Partial or
failed writes mute/flush output and start a new priming window.

The playback task holds the existing speaker mutex. Ring Test skips its
request while that speaker is reserved. Recording diagnostics reserve the
microphone and speaker; a busy resource prevents a new diagnostic or causes
call audio startup to fail cleanly. TX-only calls reserve the microphone;
RX-only calls reserve the speaker. No call worker pauses the ADC for playback.
There is no acoustic echo cancellation; full-duplex board tests therefore
need conservative speaker and microphone levels.

Hangup closes the peer and requests asynchronous codec-worker shutdown.
Workers mute/release output and release capture ownership before acknowledging
completion. The peer owner frees the audio context only after both workers
have stopped and peer callbacks can no longer reference it. Incoming calls,
outgoing calls and audio-mode changes are rejected while cleanup is pending.
Pairing and NVS credentials are retained.

## Scheduling and diagnostics

New configurations select `CONFIG_FREERTOS_HZ=1000`. Existing `sdkconfig`
values override defaults, so an older configuration must also select this
tick rate. The ICE receive timeout is 5 ms and the peer's active-call wait is
one tick. Previously, the 100 Hz tick rate rounded socket waits upward and
the worker added another 10 ms wait. This could delay local 20 ms send slots;
the earlier approximately 21.5% skipped-slot sample was not a network-loss
measurement.

After the initial full-duplex continuity failure, the peer owner's priority
is raised from 3 to 6, above the priority-5 capture/codec workers. Its socket
wait and one-tick yield still permit audio work. This correction keeps the
same core assignment and Wi-Fi power-save configuration for the
first comparison. The corrected image completed full-duplex calls with each
endpoint as caller, as recorded below.

`webrtc status` and connected-call logs expose separate stages:

| Counters | Meaning |
| --- | --- |
| Capture frames, gaps, discontinuities, stale frames and timeouts | PCM publication/consumer continuity and capture freshness; native microphone logs also report DMA overruns and I²S read errors. The timeout field is added for the priority-correction build. |
| Encoded frames, encode errors, encode duration | Work completed by the local Opus encoder. |
| TX queue/pop drops, blocked sends, send errors, source PTS skips | Local discard or application submission outcomes. The pinned RTP layer can hide socket errors, so zero application send errors does not prove successful UDP delivery. A source timestamp gap preserves the missing source time even when RTP sequence numbers remain contiguous. |
| Peer loop maximum and loops over 20 ms | Connected-state `esp_peer_main_loop` duration; DTLS connection-transition time is excluded. It does not time the subsequent audio submission. |
| RX queue drops/stale frames, jitter overruns, late/duplicate/missing timestamps, local skips | Separate arrival-queue, playout-deadline and local scheduling outcomes. Missing playout timestamps alone do not identify network loss. The current `rebases` log field combines timestamp discontinuities and misaligned-timestamp rejection; it is not a count of actual reprimes. |
| Decoded, PLC, silence and played frames; write errors and producer gaps | Decoder/output work and inferred local feed starvation. Producer gaps are not calibrated hardware DMA-underrun measurements. |
| Capture/send/receive age, decode/write maxima, task stack minima and heap reports | Stage timing and resource observations. Send age is measured before submitting the packet, RX age starts at the peer callback, and a completed write means PCM was queued to DMA. |
| RSSI and Wi-Fi power-save mode | Radio environment/configuration observations added for the priority-correction build; neither directly counts lost packets. |

Pion reports `rtp_sequence_missing`, gap events, reordered and duplicate packets
separately from timestamp gaps. Its 256-packet history can repair inferred holes
when late packets arrive. Missing RTP sequences suggest delivery/receiver
drops; they do not prove radio loss. Timestamp gaps on contiguous forward
sequences identify source-timeline gaps. Receive-WAV queue drops and disk
errors are separate local recording outcomes. Compare synchronized call
intervals before drawing conclusions from cumulative counts.

The pinned default peer creates no internal sender or receiver task; its API
documents that all work is driven by `esp_peer_main_loop`. For this UDP path,
RTP packetization, SRTP and socket submission run synchronously in the caller.
Inspection of the exact ESP32-S3 library shows the generic RTP encoder discards
the write callback's result and returns success. The visible UDP wrapper can
retry buffer exhaustion twice, with a 20 ms sleep and up to 5 ms `select` wait
per retry, then return an error that the encoder hides. Consequently a counted
successful application send is an RTP submission, rather than confirmed wire
delivery. Inferred sequence holes can reflect local transport failures,
receiver drops or path loss; radio loss is not isolated by these counters.

Neither packet counts nor decoded WAV duration measure end-to-end latency.
The clean marker measurement below estimates one acoustic round trip at about
170 ms; it does not establish calibrated one-way delay or a jitter bound.
This board cannot currently be moved to improve Wi-Fi
signal. The codec-stack-corrected validation boots reported RSSI -86 and
-85 dBm, compared with approximately -90 dBm during the earlier transport run.
The priority-correction boot reported -67 dBm; its completed ESP32-originated
full call sampled -83 to -66 dBm; the Pion-originated full call sampled
-79 to -65 dBm, with power-save mode 1 unchanged. These are stronger observed
samples than the earlier approximately -90 dBm run. Signal varied
between samples, and a controlled improved-signal comparison remains unavailable.

## Physical validation procedure

Audio mode is selected for the next call and can change only when the peer is
idle and cleanup has completed. Full duplex is the default:

```text
webrtc audio tx
webrtc audio rx
webrtc audio full
webrtc status
```

Use the already paired Pion line in the same Digits family. The
[Pion helper guide](../tools/interop/README.md#record-receive-audio-and-send-a-speech-wav)
describes private recording directories, container mounts and source-file
requirements. Store recordings outside the FUSE repository mount. A normal
flash preserves NVS; do not erase flash to install this change.

1. **Microphone to Pion:** select `webrtc audio tx` while idle. Start the helper
   with `--audio silence --record-dir PRIVATE_DIRECTORY`, establish a call,
   speak a recognizable phrase near the onboard microphone and hang up.
   Confirm the finalized 48 kHz mono PCM16 receive WAV contains intelligible
   speech. Compare capture/encode/send counts, source PTS gaps, RTP sequence
   gaps and recorder drops. A nonempty file or decoded packet alone is
   insufficient acoustic evidence.
2. **Pion to speaker:** select `webrtc audio rx`. Feed a finalized speech WAV
   with `--audio wav --audio-file PRIVATE_SPEECH_WAV`. Confirm intelligible
   speech through the integrated speaker and collect decoder, PLC, deadline,
   write and producer-gap counters. A successful I²S write alone does not
   establish intelligibility.
3. **Full duplex:** select `webrtc audio full`, repeat with speech in both
   directions, then test both endpoints as caller. Repeat call/hangup cycles;
   wait for `cleanup_pending=no` before the next call. Compare idle internal
   free/largest-block memory, PSRAM free and both codec-worker stack minima.
4. **Diagnostic exclusion and recovery:** if Ring Test can be dispatched during
   an RX/full call, it should log that the speaker is reserved. The current
   WebUI disables its Ring Test button during a call, so that dispatch path
   could not be exercised on this board. KEY1 recording should decline a
   busy microphone/speaker without pausing call capture. After hangup, repeat
   Ring Test and local recording to check resource release and resumed
   capture. Test Wi-Fi/signaling loss separately from ordinary hangup.

Record firmware identity, endpoint versions, call role, audio mode, duration,
RSSI and both logs with each result. Keep physical audibility, local stage
timing, inferred missing media and actual end-to-end latency as separate
findings.

## Validation status

The priority-corrected ESP-IDF 5.5.3 audio build passes compilation and has been
flashed. Its image is 1,369,904 bytes (`0x14e730`), leaving
67% of the 4 MiB application partition free, with ELF SHA-256 prefix
`808a295d05`. Its private
hardware configuration selects `CONFIG_FREERTOS_HZ=1000`; the user's supplied
`sdkconfig` remains unchanged. These are build identifiers and configured
limits, rather than runtime heap measurements. The 53 peer-worker lifecycle cases
pass normally and under ASan/UBSan. They exercise encoded-frame ownership,
timestamps, asynchronous startup/failure/cleanup, audio mode controls and the
short worker wait. Microphone reservation and recording cleanup regressions
also pass. The physical results below are separate from these host models.

The first physical TX-only audio call on image `559ad7bda5` failed immediately
with a stack overflow in `digits_call_tx`, before audio was transmitted.
That image gave both codec workers 16 KiB stacks. The pinned codec documentation recommends
about 40 KiB for a task supporting all encoders and 20 KiB for all decoders;
the correction allocates 48 KiB TX and 24 KiB RX. The `6e93bdd4dc` build's first
TX-only retest established Opus transport and measured stack headroom without
the immediate overflow. Host codec mocks did not exercise the
real Opus library's stack usage, so their earlier passes do not invalidate
this physical failure.

### Microphone-to-Pion transport and recording

The `6e93bdd4dc` firmware's Pion-originated TX-only call was manually answered on
ESP32 and ran from 23:12:42 to 23:17:09 UTC, about 267.7 seconds. ESP32 encoded
13,382 frames and successfully submitted 13,372 packets; Pion received all
13,372. There were nine encoded-pop drops and nine source PTS skips, about
0.0673% locally skipped source slots. Capture gaps, stale frames, encode errors,
blocked sends and send errors were zero, with one initial capture discontinuity.
Maximum encode time was 8,540 microseconds, capture age 12,611 microseconds and
send age 27,632 microseconds. The peer loop maximum was 22,066 microseconds,
with nine loops above 20 ms. TX minimum free stack was 27,136 bytes of 48 KiB.

Pion finalized a 48 kHz mono PCM16 WAV containing 12,837,120 samples, or
267.44 seconds of decoded media, with peak 1,805 and RMS 21.05. The user spoke
for 10–15 seconds, but the raw recording is quiet and its speech
intelligibility has not been confirmed. Decode errors, inferred missing RTP
sequences, sequence gap events, reordered/duplicate packets and recording
drops/errors were zero. Nine contiguous-sequence timestamp gaps totaled
8,640 samples: these match the nine locally unsent 20 ms source frames, rather
than missing transmitted RTP packets. After cleanup, internal free memory was
158,943 bytes and PSRAM free was 8,362,080 bytes.

This weak-signal sample shows a substantially lower local skipped-slot rate
than the historical approximately 21.5% sample, and no inferred missing RTP
sequence numbers. It does not isolate which scheduling change caused the
improvement, prove absence of all network loss or establish microphone speech
audibility, end-to-end latency or long-duration resource stability.

### Pion-to-speaker speech

An ESP32-originated RX-only call on `6e93bdd4dc` ran from 23:18:50 to
23:19:49 UTC. Pion looped
a known 7.842-second offline synthesized speech WAV with RMS 821.65 and peak
6,564. The user confirmed: **“Speech is clear.”** This verifies intelligible
Pion-to-ESP32 playback through the integrated speaker.

ESP32 reported 2,931 received frames, 2,556 decoded frames, 154 PLC frames and
2,709 played frames. Decode/write errors, arrival queue drops/stale frames,
jitter overruns and local playback skips were zero. There were 60 late frames,
154 missing playout timestamps, 39 combined discontinuity/misaligned-timestamp
events and 11 producer feed gaps. The combined timestamp counter does not
establish 39 actual reprimes; missing playout timestamps and inferred producer
gaps do not measure radio loss or calibrated DMA underruns.
Maximum decode time was 10,788 microseconds, receive age 141,796 microseconds
and write time 17,098 microseconds. RX minimum free stack was 16,000 bytes of
24 KiB. After cleanup, internal free memory was 158,835 bytes and PSRAM free
was 8,362,080 bytes. These results confirm a short audible RX call; they leave
playout continuity, end-to-end latency and repeated-cycle stability to further
validation. Subsequent full-duplex speech results follow below.

### First full-duplex continuity failure

The first full-duplex call on `6e93bdd4dc`, with peer-owner priority 3, ran from
23:20:32 to 23:24:14 UTC, about 222 seconds. The user's clarity response was
**“not so clear.”** ESP32 captured and encoded 11,116 frames with no capture
gaps or encode errors, but sent 10,397 packets. There were 64 TX queue drops
and 654 pop drops, about 6.46% of encoded source frames. ESP32 received 10,505
frames, dropped 121 in its arrival queue and counted 300 late frames, 799
missing playout timestamps and 194 combined discontinuity/misaligned-timestamp
events. It decoded 8,795 frames, used 799 PLC frames and played 9,594, with
90 inferred producer gaps. Local playback skips and decode/write errors were
zero.

Pion received 9,984 packets and reported 413 inferred missing RTP sequences
across 240 gap events. Contiguous-sequence timestamp gaps totaled 406,080
samples across 289 events. The receive WAV contains 9,584,640 samples, with
zero recording drops/errors. TX queue/pop drops, RTP missing sequences and
timestamp gaps observe different stages and cannot simply be added as one
loss total. The pinned library's 64-bit RTP-to-millisecond conversion preserves
20 ms frame spacing; library-cache abandonment or burst delivery remain
possible contributors to playout discontinuities. These counters do not
identify radio loss as the cause.

Internal minimum free memory was 20,731 bytes. After cleanup, internal free
memory was 158,883 bytes and PSRAM free was 8,362,080 bytes. The priority-6
peer-owner correction described above is flashed for a separate retest.
This failed clarity result does not establish stable full-duplex audio.

### Priority-6 full duplex and recorded microphone speech

On `808a295d05`, ESP32 called Pion from 23:26:49 to 23:28:50 UTC, about
121 seconds. Pion played the known synthesized speech source while ESP32
captured live microphone PCM. Asked to speak for 10–15 seconds and listen,
the user confirmed **“speech is fine.”** ESP32 captured/encoded 6,057 frames
and submitted 6,031 packets, with zero TX queue drops and 25 pop drops, about
0.413% locally skipped source slots; one closing frame accounts for the
remaining encoded/submitted difference. Capture gaps/stale frames/timeouts
and codec errors were zero. RX received 6,054, decoded 6,051 and played
6,050 frames, with zero PLC, late/missing timestamps, discontinuities, local
skips, queue drops/stale media, jitter overruns, producer gaps and decode/write
errors.

Maximum encode/capture-age/send-age were 18,347/17,127/31,185 microseconds;
decode/receive-age/write maxima were 16,307/95,873/17,235 microseconds. TX/RX
minimum free stacks were 27,136/16,012 bytes. The last periodic peer-loop
report before hangup showed a maximum of 20,002 microseconds and one loop
above 20 ms. Minimum internal free memory was 39,915 bytes, and stable idle
PSRAM free returned to 8,362,080 bytes. Connected RSSI samples ranged from
-83 to -66 dBm, with power-save mode 1 unchanged.

Pion received all 6,031 submitted packets with no inferred missing sequences.
Twenty-five contiguous-sequence timestamp gaps totaled 24,000 samples,
matching the locally skipped 20 ms source frames. The finalized WAV contains
5,789,760 samples, or 120.62 seconds, with peak 9,092 and RMS 100.81; recording
drops/errors were zero. This completed sample shows clear speaker speech and
simultaneous live capture without the first full-call continuity failure.
Priority and radio conditions both differed between those runs, so it is not
an isolated causal measurement of the priority change.

For microphone-speech confirmation, the original receive WAV was preserved.
A separate private copy of its last 35 seconds used 16× offline gain, an
80 Hz high-pass filter and a peak limiter of 8,192, yielding RMS 438. Pion then
called ESP32 at 23:29:59 UTC and looped this source during full duplex. The
user explicitly confirmed **“My words are clear.”** This verifies the physical
ESP32 microphone → Pion decoded WAV → Pion Opus → ESP32 speaker path with
that offline preparation. Firmware transmit gain and raw recordings were not
normalized. The following completed-call counters and diagnostic checks scope
that result; these bounded audible calls do not establish long-duration
stability. A separate marker probe below estimates one acoustic round trip.

### Pion-originated full duplex and diagnostic exclusion

The same `808a295d05` firmware completed the Pion-originated call from
23:29:59 to 23:33:12 UTC, about 193 seconds. ESP32 captured/encoded 9,639
frames, submitted 9,587 and counted zero TX queue drops, 51 pop drops
(including closing discard) and 50 source PTS skips. Capture gaps/timeouts
were zero, with one initial capture discontinuity. Pion received all 9,587
packets, with zero inferred RTP gaps, reordered/duplicate packets or recording
drops/errors. Fifty timestamp gaps totaled 48,000 samples, matching the
source PTS skips. The finalized WAV contains 9,203,520 samples, or 191.74
seconds, with peak 1,865 and RMS 30.9976.

RX received 9,638 frames, decoded 9,630, used three PLC frames and played
9,633. Late frames and missing playout timestamps were three each. Timestamp
discontinuities, arrival queue drops/stale frames, jitter overruns, local
skips and decode/write errors were zero; one inferred producer feed gap was
reported. Maximum encode/capture-age/send-age were 19,493/19,500/34,371
microseconds; decode/receive-age/write maxima were 18,961/101,726/18,546
microseconds. TX/RX minimum free stacks were 27,136/16,012 bytes. Maximum peer
main-loop time was 28,879 microseconds, with one loop above 20 ms. Internal
minimum free memory was 37,955 bytes. After cleanup, internal free/largest
block were 158,735/63,488 bytes and PSRAM free was 8,362,080 bytes. Connected
RSSI samples ranged from -79 to -65 dBm, with power-save mode 1 unchanged.

KEY1 was pressed twice during this call; both recording requests logged
`Speaker busy` and declined. The user reported that streamed speech continued
playing. This exercises diagnostic exclusion while call playback owns the
speaker. Concurrent Ring Test was not dispatched: its WebUI button was
disabled during the call.

After hangup, the user confirmed Ring Test and KEY1 recording/playback worked
as usual. Serial logs show a Ring Test queuing 48,000 frames and a 2,060 ms
KEY1 recording containing 98,880 samples, followed by resumed capture. This
verifies speaker/capture release and diagnostic recovery after the full call.

### Additional call/hangup cycles

Four additional own-voice full calls completed on the same firmware, alternating
callers. All four reached idle cleanup with the internal
minimum free unchanged at 37,955 bytes, largest block 63,488 bytes and PSRAM
free 8,362,080 bytes.

| Cycle | Caller; UTC interval | ESP32 submitted / Pion received | Local TX pop drops; source timestamp gaps | RX PLC; local skips; producer gaps | Idle internal free |
| --- | --- | --- | --- | --- | --- |
| 1 | ESP32; 23:35:04–23:36:01 | 2,852 / 2,852 | 12; 11,520 samples across 12 events | 0; 16; 14 | 158,467 bytes |
| 2 | Pion; 23:37:04–23:37:20 | 806 / 806 | 3; 2,880 samples across 3 events | 0; 4; 4 | 158,487 bytes |
| 3 | ESP32; 23:38:19–23:38:37 | 860 / 859 | 3; 2,880 samples across 3 events | 0; 0; 0 | 158,499 bytes |
| 4 | Pion; 23:41:45–23:42:01 | 781 / 781 | 6; 5,760 samples across 6 events | 0; 0; 0 | 158,467 bytes |

Pion inferred no missing RTP sequences in these cycles; cycle 3's one-packet
endpoint-count difference occurred at the hangup boundary. RX decoded/played
counts were 2,842/2,841, 800/799, 858/858 and 782/782 respectively. Cycles 1 and 2 had
maximum decode durations of 35,560 and 32,856 microseconds, beyond one 20 ms
frame, alongside local deadline skips and inferred producer gaps. These local
playback delays are separate from transport sequence gaps. Signal samples
during the extra cycles ranged from -87 to -63 dBm, with the board unmoved.
Cycle 4 had zero capture gaps/stale frames/timeouts, codec/write errors,
arrival drops/stale media, jitter overruns and timestamp discontinuities;
its maximum encode/decode/write times were 10,036/10,040/15,752 microseconds.
Peer-worker minimum free stack in its final status was 11,272 bytes of 20 KiB.
The short runs demonstrate repeated cleanup and continued pairing, while
showing that occasional local playback starvation remains. They do not
establish long-duration stability or calibrated DMA-underrun rates. The
arrival-order full-call WAVs contain source timestamp gaps, so they cannot
support an absolute acoustic lag estimate without restoring the timeline.

### Single acoustic round-trip estimate

Five short full-duplex probes using both call roles were attempted. Four were
rejected from lag analysis because outgoing source PTS gaps compressed the
arrival-order receive WAV. The clean probe ran from 23:43:26.82 to
23:43:28.93 UTC, about 2.1 seconds. Its source WAV used a 100 ms chirp at
0.5 seconds of a four-second period, with PCM peak 4,096. Pion recorded
100,800 samples from 105 packets; inferred RTP sequence gaps, timestamp gaps,
reordering, duplicates, decode errors and WAV drops/errors were zero. ESP32
captured/encoded 106 frames and submitted 105 with no pop drops. RX received
104, decoded 100 and played 99 before stopping during a frame; PLC, local
skips, producer gaps and codec/write errors were zero. Source and receive
totals at closure are not a loss-rate measurement.

The helper's first successful source submission was observed at
`2026-10-10T23:43:26.820004531Z`; its first successful receive decode at
`2026-10-10T23:43:26.825504128Z`, about 5.5 ms later. The known source marker
starts at sample 24,000, with a correlated receive position near 31,856.
Combining those sample positions and software origins gives the reported
**169.167 ms estimate, approximately 170 ms**. The correlation was 0.598722;
alternative peaks at least 10 ms away had absolute correlation at most 0.201.
An independent analysis reproduced the estimate.

This observation includes Pion encoding/transmission → ESP32 jitter/decode/
DMA/speaker → acoustic coupling → onboard microphone/capture/Opus → Pion
receive/decode. It is one acoustic round trip, not one-way transport latency.
Software origins are recorded after submission/decode and are not calibrated
speaker/microphone or wire times. Missing per-frame source timestamps prevent
fully excluding ticker stalls. Timing uncertainty can exceed one 20 ms frame;
no ±20 ms bound is established. One marker supplies no repeatability or jitter
distribution. The full-call files with source gaps remain excluded from this
absolute estimate.

The last probe ended at 23:45:02 UTC; one source PTS gap made it unsuitable
for lag analysis despite zero codec/recording errors. Final idle internal
free/largest block were 158,475/63,488 bytes, PSRAM free 8,362,080 bytes and
internal minimum free 37,955 bytes. RSSI was -66 dBm with power-save mode 1.

### Final state and remaining limits

At the final 23:45:35 UTC console check, the board was online, paired and idle
in full mode with `cleanup_pending=no`. Internal free/minimum/largest block
were 158,475/37,955/63,488 bytes, PSRAM free 8,362,080 bytes and peer-worker
minimum free stack 11,272 bytes. RSSI was -65 dBm with power-save mode 1.
Pion and serial tools stopped normally; the board was not reset. The current
boot's microphone logs contain no I²S read faults or RX DMA overruns; the
capture driver reports nonzero faults in its one-second windows.

The verified speech result includes offline gain/filter preparation of the
microphone recording, with firmware transmit gain unchanged. Occasional local
RX deadline skips/producer gaps remain, and actual playback DMA underruns
were not calibrated. The single acoustic round-trip estimate supplies no
one-way delay, jitter distribution or proven timing bound. Radio conditions
varied, concurrent Ring Test dispatch was unavailable in the WebUI and these
short calls do not establish long-duration stability.

### Host regressions

The production audio-worker regressions also pass normally and under
ASan/UBSan. With mocked capture, Opus APIs and two DMA descriptors they cover
1,000 50 Hz capture frames polled every 6 ms without cadence drift, 100
playback cycles without modeled starvation, an injected 100 ms scheduling
stall, queue bounds, partial-write reprime, both-worker shutdown acknowledgements
and allocation/codec/task/resource-ownership failures. These are host model
results, not measured ESP32 deadlines or actual decoder quality. The pure
jitter/render checks cover reorder, duplicates, late/missing media, timestamp
wrap/rebase, bounded outage resynchronization and signed PCM gain limits.

After a build installs the pinned component headers, run from the repository
root:

```sh
python3 esp32/tests/call_audio_worker_test.py
python3 esp32/tests/call_audio_worker_test.py --sanitize
cc -std=c11 -Wall -Wextra -Werror -I esp32/main \
  esp32/main/call_audio_jitter.c esp32/tests/call_audio_jitter_test.c \
  -o /tmp/digits-call-audio-jitter-test
/tmp/digits-call-audio-jitter-test
```

## DTLS identity limitation

This audio change does not resolve certificate-to-SDP fingerprint validation.
The visible pinned DTLS implementation uses optional client certificate
verification, and a remote fingerprint comparison has not been established
from the available source/library inspection. The historical calls verify
DTLS/SRTP transport interoperability; they do not establish authenticated
remote peer identity or production security readiness.
