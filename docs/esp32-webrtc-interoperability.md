# ESP32 WebRTC interoperability: Phase 1 design

This note records the compatibility review before implementation. The server stays unchanged. Upstream `server/v1.99.0` is the lightweight tag pointing to `a4edaaa557c06b3b687be9e92a869fb0cbcf68fc`; `git diff a4edaaa HEAD -- server pi/digitsd` is empty, so the locally inspected server and Pi sources are the exact release sources.

## Digits wire contract

[`signaling.Message`](../server/internal/signaling/protocol.go) carries raw SDP in the JSON string `sdp` and a **bare ICE candidate attribute string** in `candidate`. ICE is not an embedded JSON object; neither `sdpMid` nor `sdpMLineIndex` crosses the wire. [`PeerManager`](../pi/digitsd/internal/webrtc/peer.go) emits `c.ToJSON().Candidate` and wraps the received string in `ICECandidateInit{Candidate: candidate}`.

Outgoing order is `{"type":"call","to":"NUMBER"}`, then `{"type":"sdp","to":"NUMBER","sdp":"SDP_OFFER"}`, then any `ice` messages. Incoming delivery is `ring` with `from`, followed by `sdp` with that caller's offer. Acceptance sends `{"type":"answer","to":"CALLER","sdp":"SDP_ANSWER"}` before local ICE. [`InitiateCall`, `AnswerCall`, `prepareAnswer`, `sendPreparedAnswer`](../pi/digitsd/cmd/digitsd/webrtc.go) enforce these gates; [`handleSignal`](../pi/digitsd/cmd/digitsd/dispatch.go) banks remote candidates while ringing. `hangup` optionally carries `reason`; `busy` and exact `error` strings terminate unsuccessful setup. The server replaces `from` with the registered identity and only routes SDP/ICE inside a tracked call: [`Relay.handleCall`, `handleAnswer`, `handleSignalingForward`, `handleHangup`](../server/internal/signaling/relay.go). Call-setup errors include `phone not connected` and `not_authorized`. A late ordinary SDP/ICE or answer can be silently dropped after teardown. `no active call` is sent for an invalid ICE restart, which is outside this milestone.

`request-ice-servers` yields `ice-servers`, with `servers[].urls`, optional `username`/`credential`. The initial milestone supports IPv4 UDP host/STUN candidates and reports unsupported TURN configurations. Pi uses Pion **4.2.20**, Opus at **48 kHz mono**, 960 samples/20 ms, 24 kbit/s, FEC and DTX: [`newPeerManager`](../pi/digitsd/internal/webrtc/peer.go), [`codec.NewEncoder`](../pi/digitsd/internal/codec/opus.go). Pi's post-answer connection deadline is **10 seconds**: [`connectTimeout`](../pi/digitsd/cmd/digitsd/voicemail.go). There is no call-success or registration-success acknowledgement to wait for.

## Selected peer API and adapter

Pin Espressif [`esp_peer` 1.5.7](https://components.espressif.com/components/espressif/esp_peer/versions/1.5.7/readme), reviewed at official ESP-WebRTC commit `33a863f1a6c57bdcb7821f6f7a8e159035a0acf6`, with `esp_libsrtp` 1.0.0 (SRTP 3.0.0) and existing ESP-IDF **5.5.3** (mbedTLS 3.6.5). Use the encoded-audio peer API directly, without the ESP-WebRTC capture/render framework. It does not own I²S; existing ES7210/ES8311 drivers, microphone diagnostics and Ring Test remain independent.

Both roles start `esp_peer_new_connection`; the controlled/callee role receives the offer before `esp_peer_main_loop`. `on_msg` exposes local SDP containing bundled `a=candidate:` lines; it does not emit separate local trickle candidates. Feed remote SDP with `ESP_PEER_MSG_TYPE_SDP` and remote bare candidates with `ESP_PEER_MSG_TYPE_CANDIDATE`. Preserve CRLF via normal JSON escaping, without prefixing bare candidates with `a=`. Role determines whether local SDP becomes Digits `sdp` or `answer`.

One worker owns peer operations. WebSocket callbacks copy bounded messages; the WebSocket owner drains generation-tagged transmit requests only after sending `register`. Bank up to 32 remote candidates until remote SDP is applied; discard stale peer/generation messages. Development serial commands are `webrtc call NUMBER`, `webrtc answer`, `webrtc hangup`, and `webrtc status`, with optional auto-answer disabled by default. A received answer changes negotiation state only; media-connected requires the peer's `CONNECTED` event after DTLS success. Send a validated 20 ms encoded Opus silence packet if needed; count received Opus packets rather than claiming microphone playback integration.

## Compatibility risks and resource plan

- The peer core is a prebuilt binary. Reviewed SDP generation advertises `opus/48000/2` even with mono channel configuration, matching Opus RTP negotiation conventions; packet-level mono reception still needs a physical Pion test.
- ICE, DTLS and SRTP APIs exist, but real interoperability is unverified. DTLS/SRTP must be enabled. Upstream DTLS uses optional certificate verification; remote SDP fingerprint comparison could not be established from available source/disassembly. Do not claim authenticated peer identity or production readiness, and do not introduce an unsupported workaround.
- Bound raw SDP to 12 KiB and the serialized WebSocket message to 16 KiB; reserve 32 candidates, explicit 8 KiB send and 8 KiB jitter buffers, and an initial 20 KiB worker stack (official examples use 10 KiB). Enable 8 MB octal PSRAM at 40 MHz with approximately 64 KiB internal malloc reserve; log actual heap and stack use on hardware. Linker image size is not runtime peer memory usage.
- Use a 4 MiB application partition while preserving NVS at `0x9000`/`0x6000` and PHY at `0xf000`. Credentials survive flashing without erasing NVS.
- This milestone has one peer, no video/data channels/TWCC, group calls, or ICE restart. Signaling loss releases the peer; upstream's 20-second call grace is not yet used for media recovery. Ring timeout is an explicit local development bound, distinct from the upstream 10-second post-answer deadline.

No fundamental API incompatibility was found in this review. Compilation and lifecycle tests can justify implementation correctness; only two physical endpoints demonstrating ICE, DTLS/SRTP and received media establish interoperability. No existing physical Pion endpoint is currently available for that final check.

## Adapter regression verification

[`esp32/tests/webrtc_interop_test.py`](../esp32/tests/webrtc_interop_test.py) compiles the complete production peer worker with the installed public `esp_peer` headers and ESP-IDF's real cJSON parser/serializer. Only the peer operations, transport callback, heap reporting and FreeRTOS scheduling/queues are finite host mocks. These mocks check ownership and sequencing; they do not implement cryptographic handshakes or simulate network interoperability.

After sourcing ESP-IDF and building once to install the pinned components:

```sh
python3 esp32/tests/webrtc_interop_test.py
python3 esp32/tests/webrtc_interop_test.py --sanitize
```

The 41 lifecycle cases verify offer/answer role mapping, `call` before SDP, controlled-peer initialization and remote offer before the main loop, JSON CRLF preservation, bare candidates and early-ICE buffering, manual answer before the offer arrives, callback buffer ownership, retained peer configuration across ICE-server refresh, and teardown on timeouts/errors/resource exhaustion. They check cleanup and successful retry after queue/task initialization failures, and that oversized/repeated unsupported group messages cannot exhaust the ordinary call's queue. They also check that SDP acceptance does not mark the call connected, no media is sent before the peer reports connection, late audio slots are skipped rather than burst, duplicate connection callbacks do not reset pacing, and signaling generation changes invalidate an apparent connection completed during disconnect. Foreign-ring rejection does not send `hangup`, because the upstream handler ends authenticated origin's active calls rather than using `to` to select one.

The initial runtime limits are eight queued signaling events, 32 buffered remote candidates of at most 512 bytes, eight cached STUN URLs of at most 255 bytes, 12 KiB raw SDP and 16 KiB serialized signaling. Active peer configuration and remote SDP remain owned until `esp_peer_close` finishes. Runtime heap high-water and actual worker stack headroom must be measured on the board; host mock heap values are not measurements.
