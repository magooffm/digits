# ESP32 WebRTC interoperability design and transport verification

This note records the compatibility review before implementation. The server stays unchanged. Upstream `server/v1.99.0` is the lightweight tag pointing to `a4edaaa557c06b3b687be9e92a869fb0cbcf68fc`; `git diff a4edaaa HEAD -- server pi/digitsd` is empty, so the locally inspected server and Pi sources are the exact release sources.

The nine-call hardware record below belongs to the encoded-silence transport
milestone. Live microphone encoding and speaker decoding are now implemented.
Pion-to-ESP32 speech has been confirmed clear on the integrated speaker during
full duplex. Recorded ESP32 microphone words were also confirmed clear after
offline gain/filter preparation and replay through Pion. Full calls completed
with each endpoint as caller; repeat-cycle and diagnostic recovery checks also
passed. See the
[call audio implementation and validation guide](esp32-call-audio.md).

## Digits wire contract

[`signaling.Message`](../server/internal/signaling/protocol.go) carries raw SDP in the JSON string `sdp` and a **bare ICE candidate attribute string** in `candidate`. ICE is not an embedded JSON object; neither `sdpMid` nor `sdpMLineIndex` crosses the wire. [`PeerManager`](../pi/digitsd/internal/webrtc/peer.go) emits `c.ToJSON().Candidate` and wraps the received string in `ICECandidateInit{Candidate: candidate}`.

Outgoing order is `{"type":"call","to":"NUMBER"}`, then `{"type":"sdp","to":"NUMBER","sdp":"SDP_OFFER"}`, then any `ice` messages. Incoming delivery is `ring` with `from`, followed by `sdp` with that caller's offer. Acceptance sends `{"type":"answer","to":"CALLER","sdp":"SDP_ANSWER"}` before local ICE. [`InitiateCall`, `AnswerCall`, `prepareAnswer`, `sendPreparedAnswer`](../pi/digitsd/cmd/digitsd/webrtc.go) enforce these gates; [`handleSignal`](../pi/digitsd/cmd/digitsd/dispatch.go) banks remote candidates while ringing. `hangup` optionally carries `reason`; `busy` and exact `error` strings terminate unsuccessful setup. The server replaces `from` with the registered identity and only routes SDP/ICE inside a tracked call: [`Relay.handleCall`, `handleAnswer`, `handleSignalingForward`, `handleHangup`](../server/internal/signaling/relay.go). Call-setup errors include `phone not connected` and `not_authorized`. A late ordinary SDP/ICE or answer can be silently dropped after teardown. `no active call` is sent for an invalid ICE restart, which is outside this milestone.

`request-ice-servers` yields `ice-servers`, with `servers[].urls`, optional `username`/`credential`. The initial milestone supports IPv4 UDP host/STUN candidates and reports unsupported TURN configurations. Pi uses Pion **4.2.20**, Opus at **48 kHz mono**, 960 samples/20 ms, 24 kbit/s, FEC and DTX: [`newPeerManager`](../pi/digitsd/internal/webrtc/peer.go), [`codec.NewEncoder`](../pi/digitsd/internal/codec/opus.go). Pi's post-answer connection deadline is **10 seconds**: [`connectTimeout`](../pi/digitsd/cmd/digitsd/voicemail.go). There is no call-success or registration-success acknowledgement to wait for.

## Selected peer API and adapter

Pin Espressif [`esp_peer` 1.5.7](https://components.espressif.com/components/espressif/esp_peer/versions/1.5.7/readme), reviewed at official ESP-WebRTC commit `33a863f1a6c57bdcb7821f6f7a8e159035a0acf6`, with `esp_libsrtp` 1.0.0 (SRTP 3.0.0) and existing ESP-IDF **5.5.3** (mbedTLS 3.6.5). Use the encoded-audio peer API directly, without the ESP-WebRTC capture/render framework. It does not own I²S; call audio reuses the existing ES7210/ES8311 drivers and coordinates with diagnostics through microphone and speaker reservations.

Both roles start `esp_peer_new_connection`; the controlled/callee role receives the offer before `esp_peer_main_loop`. `on_msg` exposes local SDP containing bundled `a=candidate:` lines; it does not emit separate local trickle candidates. Feed remote SDP with `ESP_PEER_MSG_TYPE_SDP` and remote bare candidates with `ESP_PEER_MSG_TYPE_CANDIDATE`. Preserve CRLF via normal JSON escaping, without prefixing bare candidates with `a=`. Role determines whether local SDP becomes Digits `sdp` or `answer`.

One worker owns peer operations. WebSocket callbacks copy bounded messages; the WebSocket owner drains generation-tagged transmit requests only after sending `register`. Bank up to 32 remote candidates until remote SDP is applied; discard stale peer/generation messages. Development serial commands are `webrtc call NUMBER`, `webrtc answer`, `webrtc hangup`, and `webrtc status`, with optional auto-answer disabled by default. A received answer changes negotiation state only; media-connected requires the peer's `CONNECTED` event after DTLS success. The initial milestone sent validated 20 ms Opus silence and counted received packets. The new call-audio path instead encodes live microphone PCM and decodes speaker PCM in dedicated workers; `webrtc audio tx|rx|full` selects the next call's mode while idle, with full duplex as the default.

## Compatibility risks and resource plan

- The peer core is a prebuilt binary. Reviewed SDP generation advertises `opus/48000/2` even with mono channel configuration, matching Opus RTP negotiation conventions. The first LAN hardware results below confirm reception and decoding of the ESP32's mono silence packets by Pion.
- DTLS/SRTP must be enabled. The first LAN hardware results below establish transport and media interoperability, but upstream DTLS uses optional certificate verification; remote SDP fingerprint comparison could not be established from available source/disassembly. Do not claim authenticated peer identity or production readiness, and do not introduce an unsupported workaround.
- Bound raw SDP to 12 KiB and the serialized WebSocket message to 16 KiB; reserve 32 candidates, explicit 8 KiB send and 8 KiB jitter buffers, and an initial 20 KiB worker stack (official examples use 10 KiB). Enable 8 MB octal PSRAM at 40 MHz with approximately 64 KiB internal malloc reserve; log actual heap and stack use on hardware. Linker image size is not runtime peer memory usage.
- Use a 4 MiB application partition while preserving NVS at `0x9000`/`0x6000` and PHY at `0xf000`. Credentials survive flashing without erasing NVS.
- This milestone has one peer, no video/data channels/TWCC, group calls, or ICE restart. Signaling loss releases the peer; upstream's 20-second call grace is not yet used for media recovery. Ring timeout is an explicit local development bound, distinct from the upstream 10-second post-answer deadline.

No fundamental API incompatibility was found in this review. Compilation and lifecycle tests justify adapter behavior; the LAN hardware results below provide the separate ICE, DTLS/SRTP and received-media evidence.

## Adapter regression verification

[`esp32/tests/webrtc_interop_test.py`](../esp32/tests/webrtc_interop_test.py) compiles the complete production peer worker with the installed public `esp_peer` headers and ESP-IDF's real cJSON parser/serializer. Peer operations, call-audio publication, the transport callback, heap reporting and FreeRTOS scheduling/queues are finite host mocks. These mocks check ownership and sequencing; they do not implement cryptographic handshakes, codec hardware or network interoperability.

After sourcing ESP-IDF and building once to install the pinned components:

```sh
python3 esp32/tests/webrtc_interop_test.py
python3 esp32/tests/webrtc_interop_test.py --sanitize
```

The 53 lifecycle cases pass normally and under AddressSanitizer/UndefinedBehaviorSanitizer. They verify offer/answer role mapping, `call` before SDP, controlled-peer initialization and remote offer before the main loop, JSON CRLF preservation, bare candidates and early-ICE buffering, manual answer before the offer arrives, callback buffer ownership, retained peer configuration across ICE-server refresh, and teardown on timeouts/errors/resource exhaustion. They check cleanup and successful retry after queue/task initialization failures, and that oversized/repeated unsupported group messages cannot exhaust the ordinary call's queue. SDP acceptance does not mark the call connected, media starts only after connection, producer timestamps and source gaps survive submission, and stale/empty publication does not become synthetic media or a catch-up burst. Audio startup and worker failures cleanly end the call; mode changes require idle and completed cleanup. A delayed audio shutdown prevents new incoming/outgoing calls until the peer is closed and workers acknowledge completion. Tests also check the one-tick active worker wait and connected-loop timing diagnostics, excluding DTLS transition time. Signaling generation changes invalidate an apparent connection completed during disconnect. Foreign-ring rejection does not send `hangup`, because the upstream handler ends authenticated origin's active calls rather than using `to` to select one. The teardown regressions verify ordinary `hangup` after connection and retain `connect_timeout` for setup failure.

The initial runtime limits are eight queued signaling events, 32 buffered remote candidates of at most 512 bytes, eight cached STUN URLs of at most 255 bytes, 12 KiB raw SDP and 16 KiB serialized signaling. Active peer configuration and remote SDP remain owned until `esp_peer_close` finishes. Host mock heap values are not measurements; the first board observations follow.

## Live hardware verification

### Initial firmware: seven calls and Ring Test regression

Testing on 2026-10-11 in the Europe/Berlin timezone (logs from 2026-10-10, approximately 22:01–22:09 UTC) used the Waveshare board's existing firmware (`app` version `7b94a10-dirty`, ESP-IDF 5.5.3, `esp_peer` 1.5.7) and [`tools/interop`](../tools/interop/README.md) on Fedora 44 amd64. The helper used Pion 4.2.20, Go 1.26.9 and libopus 1.6 in rootless Podman 5.8.7 with `--network host`. Both endpoints used the same LAN and the user's existing Digits deployment, reported as signald v1.99.0; its running version was not independently queried. No server or firmware change was made for this run.

Four Pion-to-ESP32 and three ESP32-to-Pion calls reached connected on both endpoints and received Opus media after SRTP processing in both directions. The fourth Pion-originated call followed a manual Pion reconnect that retained its identity and pairing. Every completed call returned both endpoints to idle after hangup, with no observed board reboot or crash during those seven calls. Existing onboard microphone capture and level diagnostics continued throughout the calls. Pion reported `audio/opus`, 48 kHz, two negotiated channels, and zero encode/decode/write errors. For example, the first Pion status reported 849 transmitted frames, 678 received packets and 650,880 decoded samples; an ESP32 status reported 437 transmitted and 551 received packets. At the first call's end, Pion had transmitted 1,668 frames and received 1,319 packets, matching the ESP32's 1,319 transmitted and 1,668 received packets. The final call after reconnect likewise ended with Pion transmitting 1,793 and receiving 1,356 packets, matching the ESP32's 1,356 transmitted and 1,793 received packets.

An additional automated attempt issued `answer` before the incoming ring was processed and was canceled before acceptance or media. It is not counted as a successful call or an ICE/DTLS handshake failure.

Before the first call, the ESP32 reported 167,091 bytes internal free, 161,691 bytes internal minimum and 8,362,080 bytes PSRAM free. After warmup, released peers left approximately 166,200–166,400 bytes internal free and the same PSRAM free, with a largest internal block of 63,488 bytes. Final idle internal free memory was 166,359 bytes. The observed global internal minimum after seven calls was 123,699 bytes; minimum peer-worker stack headroom was 11,408 bytes. That stack measurement covers only the WebRTC peer worker, not the separate `digits_ring` worker. These are bounded observations from this run, not a leak proof or a complete handshake memory budget.

The subsequent Ring Test smoke check failed. At ESP32 uptime 1,033,603 ms, `ring_test: Playing 1s speaker test` was followed by a stack-overflow panic in task `digits_ring`, a corrupted backtrace and a software reset. Repeated Ring Test requests produced five `digits_ring` stack-overflow panics in the capture, with one intervening test completing normally. The user reported hearing the tone only on some presses. After reset, the board retained saved number `3140001` and reconnected with its existing pairing. The initial firmware smoke test therefore failed despite the seven successful calls. The correction and its remaining hardware check are recorded below.

Follow-ups exposed by this run:

- A remote transport close after `CONNECTED` can arrive before the signaling hangup. The original [`on_peer_state` and `peer_task`](../esp32/main/webrtc.c) classified it as `connect_timeout`, despite successful media. Cleanup worked, but this could send a false failed-call reason to an upstream Pi. No firmware correction was applied during these initial seven calls; the later correction is recorded below.
- One status sample from the first call reported 1,222 submitted packets, 334 skipped 20 ms send slots and zero blocked sends: approximately 21.5% of elapsed slots were skipped (`334 / (1,222 + 334)`). These are local pacing skips, not measured network packet loss. The worker intentionally avoids catch-up bursts; receive-loop timing and scheduling need measurement. Wi-Fi RSSI was approximately -90 dBm, so repeat under better signal before attributing a cause.

### Corrected firmware: two further calls and repeated Ring Tests

The ESP-IDF 5.5.3 build used a separate private copy of the supplied `sdkconfig`, with KEY1 hold-to-record enabled to match the already flashed board. The supplied file selected recording disabled and remained unchanged. The application image was 1,185,200 bytes (`0x1215b0`) in the 4 MiB (`0x400000`) app partition, with 72% free. Bootloader, application and partition table were flashed without erasing NVS; the board retained its original hardware ID and number `3140001`. The flashed firmware reported SHA-256 prefix `6970a82cf`.

The Ring Test correction moves its 1,920-byte PCM block into static storage owned by its single worker and increases that worker's stack from 4,096 to 8,192 bytes. Host stack-usage measurement reduced the worker's own frame from 2,096 to 160 bytes; this is a host compiler measurement, not the board's stack high-water mark. The deadline regression passes with `--stack-usage`, covering 70 DMA timing phases, 40 producer-preemption cases, genuine stalls, overlapping requests and subsequent playback.

One further call in each direction established host-candidate ICE, DTLS/SRTP and bidirectional Opus reception through the same unchanged Digits deployment. Pion negotiated Opus at 48 kHz with two SDP channels and decoded the ESP32's silence into 960 mono samples per 20 ms frame, with zero encode/decode/write errors. Both calls returned both endpoints to idle, bringing the total to nine successful calls: seven on the initial firmware and two on the corrected build.

At the Pion-originated call's end, Pion transmitted 939 frames and received 710 packets, matching the ESP32's 710 transmitted and 939 received packets. In the reverse call, ESP32 transmitted 617 and received 674 packets; Pion received 617 and had submitted 677 frames by closure. The three-packet difference at teardown is not a measured loss rate. The Pion hangup reproduced DTLS closure before signaling hangup: ESP32 logged local reason `peer transport failed` and the corrected `end_failed_call` path sends ordinary wire `hangup` without `connect_timeout`. The reverse call ended with ESP32 local hangup and Pion receiving an empty reason.

After these two calls, idle internal free memory was 160,315 bytes, the internal minimum was 116,123 bytes and the largest internal block was 63,488 bytes. PSRAM free was 8,362,080 bytes; peer-worker stack headroom was 11,432 bytes. These measurements belong to the corrected build and are separate from the seven-call values above; they do not measure the Ring Test worker's stack or establish absence of leaks.

After both corrected-build calls, the Ring Test hardware retest passed: the serial capture recorded 15 playbacks between boot uptimes 148,103 and 185,893 ms, and the user confirmed a consistently audible tone on every WebUI button press. All captured tests reached the normal stop message, with no stack overflow, panic or further reset after the initial USB reset. Minimum measured Ring Test worker stack headroom was 5,932 bytes of its 8,192-byte stack. Each playback queued 47,520 or 48,000 frames within the existing one-second deadline; maximum reported render time was 218 microseconds and maximum producer gap was 219 microseconds, against a 10,000-microsecond DMA block. This is a bounded short retest, not a long-duration firmware stability result.

At uptime 196,223 ms and the final USB status at 262,183 ms, the board remained online, paired and idle with internal free memory of 160,339 bytes, internal minimum of 116,123 bytes, largest internal block of 63,488 bytes, PSRAM free of 8,362,080 bytes and peer-worker stack headroom of 11,432 bytes.

Together, these runs establish one-to-one LAN ICE, DTLS/SRTP and bidirectional encoded Opus interoperability through Digits signaling, plus consistently audible Ring Tests and 15 completed playbacks without the earlier stack panic on the corrected build. They do not establish audible call audio, microphone encoding/playback, Internet/TURN coverage, certificate fingerprint verification or long-duration resource stability. Concurrent Ring Test/recording behavior, the skipped-send-slot follow-up and ESP32 Wi-Fi-loss recovery require further hardware checks.

## Call audio implementation and physical validation

The ESP-IDF 5.5.3 build now includes `esp_audio_codec` 2.6.2 and passes with
live ES7210 microphone encoding and ES8311 Opus playback. The configured
current priority-corrected image is 1,369,904 bytes (`0x14e730`), leaving 67%
of the 4 MiB application partition free, with ELF SHA-256 prefix `808a295d05`,
and has been flashed. The private
hardware configuration uses a 1,000 Hz tick; the supplied `sdkconfig` remains
unchanged. Codec tasks reuse
the existing 48 kHz mono PCM path; encoded packets and playback timestamps
remain 20 ms. The encoder now has a 48 KiB internal stack and the decoder
has 24 KiB, while bounded
PCM/packet/jitter storage prefers PSRAM. A microphone consumer mutex and the
speaker reservation exclude recording diagnostics and overlapping playback.
Shutdown is asynchronous; a later call waits until old workers have stopped
and their context has been released.

Production audio-worker checks pass normally and under ASan/UBSan with finite
capture/codec/two-DMA mocks. They cover 1,000 fixed-clock TX frames with 6 ms
polling, 100 playback cycles without modeled starvation, injected scheduling
stalls, bounded queues and cleanup/failure paths. Pure jitter/render checks
also pass. These results support local ordering and scheduling logic, without
claiming ESP32 execution deadlines, Opus quality or physical audibility.

New configurations use a 1,000 Hz FreeRTOS tick, a 5 ms peer receive timeout
and a one-tick active wait. The former 100 Hz timeout rounding and extra
10 ms wait could delay local send slots. The encoder/peer now preserve
capture-clock PTS, count source skips and expose stage age and loop-duration
diagnostics. These changes address local scheduling; they do not prove that
the earlier weak Wi-Fi signal caused the skipped-slot observation. Pion's RTP
sequence-gap and contiguous-sequence timestamp-gap counters provide separate
delivery and source-timeline evidence. Record/WAV drops remain local outcomes.

The [audio guide](esp32-call-audio.md) records microphone-to-WAV,
speech-WAV-to-speaker and full-duplex procedures. Pion-to-speaker speech was
confirmed clear in RX-only and full-duplex calls below. Recorded microphone
speech was confirmed intelligible with offline processing. End-to-end latency,
long-duration resource stability still needs validation. Diagnostic recovery
and a qualified single acoustic round-trip estimate are recorded below.
The board cannot currently be moved
for an improved-signal comparison. Stack-correction boots reported RSSI -86
and -85 dBm. The priority-correction boot reported -67 dBm, while the completed
ESP32-originated full call varied from -83 to -66 dBm and the Pion-originated
full call from -79 to -65 dBm, with power-save mode 1 unchanged. Radio
conditions vary between samples; no controlled improved-signal comparison exists.

DTLS certificate-to-SDP fingerprint verification is a separate unresolved
limitation. Adding codecs and speaker playback does not establish remote peer
identity authentication or production security readiness.

The first full-duplex call on the priority-3 peer-owner image lasted about
222 seconds, and the user reported **“not so clear.”** ESP32 captured/encoded
11,116 frames, sent 10,397 and counted 64 TX queue drops plus 654 pop drops,
about 6.46% locally skipped source frames. Pion received 9,984 packets and
inferred 413 missing RTP sequences across 240 events; contiguous-sequence
timestamp gaps totaled 406,080 samples across 289 events. Its WAV recorder
dropped no media and reported no disk errors. ESP32 used 799 PLC frames and
counted 90 inferred producer feed gaps; decode/write errors and local playback
skips were zero. Minimum internal free memory was 20,731 bytes; after cleanup,
internal free was 158,883 bytes and PSRAM free 8,362,080 bytes.

The [full result](esp32-call-audio.md#first-full-duplex-continuity-failure)
keeps source skips, arrival-queue drops and inferred RTP gaps separate. These
counters do not establish radio loss. The pinned library's 64-bit RTP timestamp
conversion retains 20 ms alignment; cache abandonment or burst delivery may
also contribute to discontinuities. The peer owner's priority is being raised
from 3 to 6, above priority-5 audio workers, retaining the same affinity and
power-save settings for the first comparison. RSSI/power-save mode and capture
timeouts are added to its logs. The new image is flashed; bounded full-duplex
audibility results follow, while repeated-cycle stability remains to be tested.

The priority-6 ESP32-originated full call ran from 23:26:49 to 23:28:50 UTC,
about 121 seconds. The user confirmed **“speech is fine”** while live capture
continued. ESP32 captured/encoded 6,057 frames, submitted 6,031 and counted
25 pop drops, about 0.413% local source skips, with no TX queue drops or capture
gaps/stale frames/timeouts. Pion received all 6,031 packets with no inferred
missing sequences; 25 timestamp gaps totaled 24,000 samples. Its receive WAV
contains 5,789,760 samples (120.62 seconds), peak 9,092 and RMS 100.81, with
zero recording drops/errors. ESP32 received 6,054, decoded 6,051 and played
6,050 frames, with zero PLC, late/missing timestamps, discontinuities, local
skips, queue drops/stale media, jitter overruns, producer gaps or decode/write
errors. The last periodic peer-loop report showed 20,002 microseconds maximum
and one loop above 20 ms. TX/RX minimum free stacks were 27,136/16,012 bytes,
minimum internal free was 39,915 bytes and idle PSRAM returned to 8,362,080
bytes. The detailed stage maxima are in the [audio results](esp32-call-audio.md#priority-6-full-duplex-and-recorded-microphone-speech).

The original microphone receive WAV was retained. A separate last-35-second
copy used 16× offline gain, an 80 Hz high-pass filter and a peak limiter of
8,192 (RMS 438). During a Pion-originated full call starting at 23:29:59 UTC,
Pion looped that copy, and the user confirmed **“My words are clear.”** This
establishes intelligibility for the ESP32 microphone → Pion decoded WAV →
Pion source → ESP32 speaker path with offline preparation. Native transmit
gain remains unchanged. That call completed at 23:33:12 UTC, about 193 seconds.
ESP32 captured/encoded 9,639 frames and submitted 9,587; Pion received all of
them with zero inferred RTP gaps, reordering or duplicates. Fifty source PTS
skips matched 50 timestamp gaps totaling 48,000 samples; 51 pop drops include
closing discard. RX received 9,638, decoded 9,630, used three PLC frames and
played 9,633, with three late/missing timestamps and one inferred producer gap.
Queue drops/stale media, jitter overruns, local skips, discontinuities and
decode/write errors were zero. Maximum peer loop was 28,879 microseconds with
one loop over 20 ms; TX/RX minimum free stacks were 27,136/16,012 bytes and
minimum internal free was 37,955 bytes. Stable idle internal free/largest block
were 158,735/63,488 bytes, with PSRAM free 8,362,080 bytes.

Two concurrent KEY1 requests declined with `Speaker busy`; the user confirmed
streamed speech continued. Concurrent Ring Test was not dispatched because
the WebUI disabled its button. After hangup, Ring Test queued 48,000 frames,
and KEY1 recorded/played 98,880 samples over 2,060 ms with capture resumed;
the user confirmed both diagnostics worked as usual. Four subsequent calls
completed cleanup, alternating callers. Their idle internal free values were
158,467, 158,487, 158,499 and 158,467 bytes, largest block 63,488 bytes and PSRAM
free 8,362,080 bytes; internal minimum stayed 37,955 bytes. All had zero
inferred missing RTP sequences at Pion. The first two had local playback
skips/producer gaps of 16/14 and 4/4, alongside maximum decode durations of
35,560 and 32,856 microseconds; the third and fourth had neither. These are local
scheduling observations, not transport-loss evidence. Extra-cycle RSSI
samples varied from -87 to -63 dBm without moving the board. Detailed
[cycle results](esp32-call-audio.md#additional-callhangup-cycles) retain source
timestamp gaps and boundary packet-count differences separately. A separate
clean marker probe estimated about 170 ms acoustic round trip; calibrated
one-way latency, jitter and long-duration stability remain unmeasured. Priority and
RSSI both changed between runs; the continuity improvement cannot be assigned
solely to the priority change.

The exact pinned default peer creates no internal sender/receiver tasks; the
public header states all work runs through the main loop. Its UDP RTP path
packetizes, encrypts and submits synchronously, but the generic RTP encoder
discards the write callback result and returns success. The visible UDP wrapper
can retry buffer exhaustion twice with a 20 ms sleep plus up to 5 ms `select`
wait per retry, then return a hidden error. Accordingly an application
successful-send count is not confirmation of socket or wire delivery. RTP
holes may include local transport failures, receiver drops or path loss,
in addition to the separately measured source-frame skips.

Five short acoustic-loop probes followed; four were rejected because source
PTS gaps compressed the receive file's timeline. The clean Pion-originated
probe from 23:43:26.82 to 23:43:28.93 UTC recorded 105 packets/100,800 samples,
with zero RTP sequence/timestamp gaps, reordering, duplicates, decode errors or
recording drops/errors. A 100 ms source chirp at sample 24,000 matched near
receive sample 31,856, with correlation 0.598722 and weaker distant alternatives.
Using the helper's first-send/first-decode software origins gave approximately
170 ms for Pion → ESP32 speaker → onboard microphone → Pion, independently
reproduced. This is a single acoustic round-trip estimate with uncalibrated
origins; uncertainty can exceed 20 ms, and it supplies neither a proven timing
bound nor one-way delay or jitter. The [measurement details](esp32-call-audio.md#single-acoustic-round-trip-estimate)
record the origins, packet eligibility and limitations. After the final probe,
idle internal free/largest block were 158,475/63,488 bytes, PSRAM free 8,362,080
bytes and minimum internal free 37,955 bytes, with RSSI -66 dBm/power-save mode 1.
At the final 23:45:35 UTC console check, the board was online, paired and idle
in full mode with `cleanup_pending=no`; RSSI was -65 dBm, power-save mode 1,
and peer-worker minimum free stack was 11,272 bytes. Pion and serial tools
stopped normally, without resetting the board. No capture I²S read faults or
RX DMA overruns were logged in the current boot; playback producer gaps remain
inferred scheduling measurements, rather than calibrated DMA underruns.

### First physical audio failure and corrected TX transport retest

The initial 1,368,928-byte audio image (`0x14e360`, ELF prefix `559ad7bda5`)
passed compilation. Its first TX-only call failed immediately with
a stack overflow in `digits_call_tx`, before audio transmission. The initial
16 KiB codec-worker allocations were insufficient for the real library.
The pinned audio-codec documentation recommends about 40 KiB for tasks
supporting all encoders and 20 KiB for all decoders. The correction increases
TX to 48 KiB and RX to 24 KiB. The revised `6e93bdd4dc` build was flashed and
completed a Pion-originated TX-only call, manually answered on ESP32, from
23:12:42 to 23:17:09 UTC, about 267.7 seconds. ESP32 encoded 13,382 frames and
sent 13,372 packets; Pion received all 13,372. Nine pop drops/source PTS skips
represent about 0.0673% locally skipped source slots. Capture gaps, stale frames,
encode errors, blocked sends and send errors were zero, with one initial capture
discontinuity. Maximum encode/capture-age/send-age were 8,540/12,611/27,632
microseconds; the peer loop maximum was 22,066 microseconds with nine loops
above 20 ms. TX minimum free stack was 27,136 bytes of 48 KiB.

Pion finalized 12,837,120 decoded samples, or 267.44 seconds of 48 kHz mono
PCM16, with peak 1,805 and RMS 21.05. The user spoke for 10–15 seconds; the
raw recording is quiet and its intelligibility remains unverified. RTP missing
sequences, sequence gap events, reordering, duplicates, decode errors and
recording drops/errors were zero. Nine contiguous-sequence timestamp gaps
totaled 8,640 samples, matching nine locally unsent 20 ms frames. The new
local skip sample is far below the historical approximately 21.5% sample;
neither sample is a radio-loss measurement. After cleanup, internal free memory
was 158,943 bytes and PSRAM free was 8,362,080 bytes.

Host codec mocks verified ordering and cleanup without reproducing the actual
library's stack use. This physical failure and correction do not change the
earlier nine-call encoded-silence transport baseline.

### Audible Pion-to-ESP32 speech

An ESP32-originated RX-only call from 23:18:50 to 23:19:49 UTC looped a known
7.842-second offline synthesized speech WAV from Pion (RMS 821.65, peak 6,564).
The user confirmed **“Speech is clear”** through the integrated speaker.
ESP32 reported 2,931 received, 2,556 decoded, 154 PLC and 2,709 played frames.
Decode/write errors, queue drops/stale frames, jitter overruns and local skips
were zero; late frames were 60, missing playout timestamps 154, the combined
discontinuity/misaligned-timestamp counter 39 and producer feed gaps 11. The
current `rebases` log label includes misaligned-timestamp rejection and does
not establish 39 actual reprimes. Missing timestamps and producer gaps do not
measure radio loss or calibrated DMA underruns.

Maximum decode/receive-age/write times were 10,788/141,796/17,098 microseconds.
RX minimum free stack was 16,000 bytes of 24 KiB. After cleanup, internal free
memory was 158,835 bytes and PSRAM free was 8,362,080 bytes. This confirms
intelligible RX speech in one short call. Later full-duplex and processed
microphone-speech results above extend this baseline; latency, continuity and
repeated-cycle stability remain bounded validation work.
