# ESP32 Phase 1: settings, incoming ringing and effective silence

Implementation and acceptance guide, 2026-10-14. The compatibility contract is
[esp32-server-features.md](esp32-server-features.md), based on server
`server/v1.99.0` (`a4edaaa`) and ESP32 `b891ab7`. The analysis was committed as
`cbb414d`; this change adds firmware implementation on top of it.

**Hardware acceptance for this change is pending.** Host tests and a firmware
build cannot establish ringtone audibility, physical stop latency or continued
full-duplex acoustic performance. Existing physical evidence remains in
[esp32-call-audio.md](esp32-call-audio.md). No device was flashed for this phase.

## Implemented behavior

| Area | Phase 1 behavior |
| --- | --- |
| Shared settings | Normalize the existing nested `line_settings` snapshot, publish it atomically in RAM, and cache it asynchronously in a separate NVS namespace. |
| Incoming calls | Logged incoming-call indication plus a repeating 440+480 Hz speaker ringtone, approximately two seconds on / four seconds off, until answer, hangup, timeout or disconnect. |
| Silent Mode | The effective server flag suppresses only incoming ringing. Enabling it during ringing latches the current call silent; disabling it does not restart that call's ringtone. The next call uses the current flag. Since 2026-10-27 a local choice can override it until the server value changes ([README](../esp32/README.md#silence)). |
| Quiet Hours | Same effective flag as Silent Mode. All schedule, timezone and DST decisions remain on the server. |
| Ring Test | Existing explicit one-second diagnostic remains independent of Silent Mode and shares the existing speaker mutex. |
| Other preferences | Voice Style and Auto Update are cached and reported only. No voice DSP, updater or remote reboot is activated. Voicemail drives the answering machine since 2026-10-27 ([README](../esp32/README.md#answering-machine)). |

Incoming silent calls remain answerable with `webrtc answer`. No local UI,
hook-switch or TH-07 wiring was added; serial logs provide the required incoming
indication. Established calls and their capture/playback settings do not change
when any snapshot arrives. Call audio remains the existing full-duplex Opus path.

## Exact settings and cache behavior

The incoming envelope is unchanged, with no added request or acknowledgement:

```json
{
  "type": "line_settings",
  "to": "1001",
  "line_settings": {
    "voice_style": "modern",
    "silent_mode": true,
    "auto_update": true,
    "voicemail": {"enabled": true, "ring_timeout_seconds": 20}
  },
  "voicemail_unheard_count": 0
}
```

- Missing/null/non-object `line_settings` is rejected without changing the cache.
  Unknown envelope/body fields are ignored, within a 32-member-per-object bound.
  Duplicate recognized keys are rejected to avoid ambiguous snapshots.
- `voice_style` is `modern` only for that exact string; missing, unknown or
  malformed values normalize to `copper`.
- `silent_mode` and `auto_update` are true only for JSON `true`. Omission means
  false, including a snapshot after a previously true value. Malformed scalar
  values also normalize to false; they do not enable a feature.
- Absent/null `voicemail` preserves the previous enabled/timeout pair. A present
  object replaces both: enabled defaults false, timeout defaults 20 seconds.
  Integer timeouts from 5 through 60 are retained; missing, fractional,
  out-of-range or malformed values normalize to 20. A non-object non-null block
  rejects the whole snapshot. Since 2026-10-27 they arm the answering machine.
- Before any valid cache or server settings, defaults are copper, silent off,
  auto-update off, voicemail off, timeout 20 seconds. The server's fresh-line
  defaults may differ; its next snapshot takes precedence.

[line_settings.c](../esp32/main/line_settings.c) stores one 12-byte blob under
namespace `digits_settings`, key `snapshot`, in the existing NVS partition:
`DSET` magic, schema byte `1`, voice enum byte, silent/auto-update/voicemail boolean
bytes, timeout byte, and two zero reserved bytes. It never opens the `digits`
pairing namespace, erases NVS, or changes the partition table. NVS supplies blob
integrity handling; no C struct padding or native enum representation is stored.

A missing, corrupt or unknown-version record uses defaults and reports the load
result. Boot does not overwrite it. The first valid server snapshot repairs it,
even if it equals the defaults. Equal subsequent normalized snapshots do not
cause flash writes. If values change several times before persistence, the
worker saves the latest snapshot; a successful older commit cannot clear a
newer dirty revision. Failed open/set/commit operations retain RAM behavior and
retry approximately every second without requiring another server message.

The WebSocket callback performs bounded validation and RAM publication, then
notifies the ringtone worker. It performs no settings flash I/O, audio writes or
waits. New settings commits are deferred from call/ring acceptance until call
audio and ringtone cleanup complete. An already-started flash operation cannot
be cancelled, and standalone Ring Test/recording diagnostics are not included
in this deferral predicate. Power loss before a pending commit restores the
previous durable snapshot; `dirty=yes` makes this visible. Do not describe a RAM
update as durable until `dirty=no` and the write result is successful.

Settings are accepted only on the current paired registration session. There
is no upstream registration ACK: readiness follows the successful registration
send. Disconnect, registration-send failure and credential replacement/storage
failure invalidate readiness. The existing owner joins old socket callbacks
before reconnecting; the settings module also rejects older generations and
changes invalidated during parsing. Reconnect retains the cache until the
server pushes its current snapshot. An offline Quiet Hours boundary cannot
change the cached bit; the device receives no schedule.

## Ringtone ownership and cleanup

[incoming_ring.c](../esp32/main/incoming_ring.c) has one boot-lifetime worker
at priority 4, an 8 KiB stack, and one static 480-frame stereo PCM block
(1,920 bytes). It reuses the initialized Ring Test tone table at half amplitude,
without changing codec volume, microphone gain, I²S setup, DMA configuration or
the media workers' scheduling. The settings worker has a 4 KiB stack at priority
2. These allocations add to baseline RAM requirements; runtime free heap and
stack minima still require hardware observation.

The incoming worker acquires the existing speaker mutex without waiting,
checks cancellation between blocks and uses at most a 20 ms I²S write timeout.
If the speaker belongs to Ring Test, saved recording playback or call audio, it
yields and retries rather than muting or releasing that owner. It releases the
speaker during cadence pauses and after cancellation or playback failure.
Explicit Ring Test can therefore run while incoming ringing is silent, or
during a cadence pause; if another owner holds the speaker, its existing skip
behavior remains. A playback start/write failure stops audible attempts for
that ringtone generation while leaving the call answerable and logged.

**Handset suppression (added 2026-10-23).** The phone controls call
`digits_incoming_ring_set_suppressed` with the handset state. While the
handset is lifted the worker releases the speaker and plays nothing. Unlike
silent mode this is not latched: replacing the handset while the same call
still rings restarts its cadence at once. See
[Phone controls](../esp32/README.md#phone-controls).

Answering stops the ringtone immediately at the command level and waits
asynchronously for release acknowledgement before creating the answering peer.
Call media cannot start while a ringtone operation remains outstanding. Hangup,
timeout and transport loss cancel ringing; a new call is excluded until cleanup
finishes. Stop/silence notifications interrupt cadence/retry waits. No additional
PCM block is deliberately queued after cancellation is observed. A write already
in flight can run to its bound; the existing board cleanup gates the amplifier
first, then mutes/flushes using three bounded 50 ms silence writes. Consequently,
physical mute latency is not claimed to be zero, and amplifier/I²C failure is
logged. Silent suppression never calls cleanup on another speaker owner's audio.

`webrtc status` reports the effective flag, cached-only preferences, cache load
result, dirty state, revision, writes/errors, accepted/duplicate/invalid/stale
counts, persistence deferral and ringtone activity. `loaded` describes the boot
load, not whether a later write succeeded. Existing call/heap/audio diagnostics
remain available. Status does not expose pairing tokens or send a server ACK.

## Automated validation

Run from the repository root, with ESP-IDF 5.5.3 available for the real cJSON
source and the pinned managed peer headers:

```sh
python3 esp32/tests/line_settings_test.py --idf-path "$IDF_PATH" --sanitize
python3 esp32/tests/incoming_ring_test.py --sanitize
python3 esp32/tests/signaling_webrtc_test.py --idf-path "$IDF_PATH" --sanitize
python3 esp32/tests/webrtc_interop_test.py --idf-path "$IDF_PATH" --sanitize
python3 esp32/tests/call_audio_worker_test.py --sanitize
```

The same suites also run without `--sanitize`. The settings suite compiles the
actual parser/persistence source against real cJSON and NVS/RTOS mocks; it covers
normalization, omission, bad envelopes, duplicates, reboot, corrupt records,
pairing isolation, stale sessions, failures, retries and concurrent revisions.
It also links the real ringer and tone renderer to verify snapshot-to-silence
behavior, reconnects and preserving another speaker owner at the 1,000 Hz tick.
The incoming suite executes the actual worker with finite scheduler/audio mocks
to check cadence, initial/live silence, one-call latching, ownership contention,
start/write/cleanup failures and command races during each I/O stage, at both
the production 1,000 Hz tick and a 100 Hz compatibility regression rate.

The WebSocket suite covers actual dispatch, paired/unpaired session gating,
the unchanged Ring Test path and absence of a new settings ACK. The WebRTC suite
covers actual call ownership, answer while ringtone cleanup is pending,
hangup/timeout/disconnect and subsequent calls, established audio and deferred
cache writes. Existing capture, playback, KEY1, jitter and Ring Test suites are
regressions for the shared audio infrastructure. Mocks do not measure real radio,
flash, codec stack usage, I²S latency or acoustics.

Validation performed for this implementation:

| Check | Result |
| --- | --- |
| Settings/parser-to-ringer suite | Pass, normal and ASan/UBSan, including RAM-only fallback when the persistence task cannot start. |
| Incoming-ring worker | Pass, normal and ASan/UBSan at 1,000 Hz and 100 Hz. |
| WebSocket transport/dispatch | 14 scenarios pass, normal and ASan/UBSan. |
| WebRTC owner/call lifecycle | 57 scenarios pass, normal and ASan/UBSan. |
| Call-audio TX/RX worker suite | Pass, normal and ASan/UBSan against the pinned audio-codec headers. |
| Existing capture, record playback, KEY1 and Ring Test Python suites | All five pass; Ring Test stack-report check also passes (208-byte host frame, not ESP32 high-water evidence). |
| Pure tone, microphone PCM, saved-playback PCM and call-jitter suites | All four pass with compiler warnings treated as errors. |
| Document checks | Relative links, JSON examples and diff whitespace pass. |

The Ring Test stack-report runner now compiles source files separately so Apple
Clang cannot overwrite the first source's `.su` report while linking multiple
sources. This only corrects test tooling; Ring Test audio behavior is unchanged.
The call-audio sanitizer runner retains Linux's `-no-pie` flag only on Linux,
avoiding Apple Clang's unused-argument error without disabling sanitizers.

The ESP-IDF 5.5.3 firmware build with the existing recording-disabled options and
a private 1,000 Hz configuration copy passes. Its application is 1,376,816 bytes
(`0x150230`), fitting the existing 4 MiB partition with about 67% free. ELF SHA-256:
`3dd7f69454628c30d0710abad1c0279ea56b9f179f5d1403fa97b7391f399128`.
The separate KEY1 hold-to-record variant also passes at 1,000 Hz with
`CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK=y`: 1,381,952 bytes (`0x151640`),
also about 67% partition space free. Its ELF SHA-256 is
`1d1c4fddcaaffc8a8eac923bba0ff8fedb108bb432d15a9d85a72a45ba6ba78e`.
The original `esp32/sdkconfig` and `dependencies.lock` hashes are unchanged;
tracked tick defaults, audio scheduling configuration and partition table are
unchanged. ESP-IDF/tool dependencies were restored in a private temporary
directory because the previously available installation was incomplete.

## Practical WebUI and hardware acceptance

Use the existing LAN server, paired board and a Pi/Pion peer. Keep a serial log
and record firmware hash, configuration, server version, Wi-Fi RSSI and peer.
Use a copy of the known-good hardware configuration with
`CONFIG_FREERTOS_HZ=1000`; retain its microphone/recording options. Do not erase
NVS. Use manual answering (development auto-answer disabled) for the ringing
scenarios. Confirm the image fits the existing application partition before the
separately authorized flash. Read `webrtc status` before each scenario.

1. **Registration/cache:** with Silent Mode off, connect and confirm the server
   snapshot appears in status. Set Silent Mode on in the phone's WebUI settings;
   confirm RAM changes promptly, then wait for `dirty=no`. Reboot normally and
   verify effective silence and unchanged hardware ID/number/pairing. Repeat
   with false booleans: their omission in the wire snapshot must clear prior
   true values. Toggle Voice Style/Auto Update/voicemail and confirm only cached
   status changes; no DSP switch, reboot, updater or auto-answer may occur.
2. **Audible incoming ring:** disable Silent Mode, place an incoming call and
   confirm the logged caller and audible two-on/four-off cadence at conservative
   level. Answer during the tone, then during its pause; the ringtone must stop
   and intelligible call audio must work in both directions. Repeat with local
   hangup, remote hangup, unanswered timeout and Wi-Fi loss. Confirm idle and no
   residual tone; a following call must work.
3. **Silent call/latching:** enable Silent Mode before an incoming call: logs and
   answer must work without audible ringing. For a different call, enable it
   during the audible portion and measure the delay until physical silence.
   Disable it while the same call remains ringing: it must stay silent. Hang up
   and place a new call: it must ring. Repeat toggles during the cadence pause.
4. **Established calls:** test both outgoing and incoming roles, talking both
   ways. Toggle Silent Mode repeatedly, then other cached preferences. No new
   ringtone, call interruption, change of audio mode or unexpected disconnect
   is allowed. Status should show dirty settings deferred until hangup/cleanup,
   then a successful commit. Compare audio skips, peer-loop timing, underruns,
   overruns and heap with the existing baseline at good Wi-Fi signal strength.
5. **Diagnostics/ownership:** with Silent Mode enabled and no call, use the WebUI
   Ring Test and verify the existing one-second tone. Repeat while a silent call
   is waiting. Test Ring Test/KEY1 playback around incoming-ring start, answer
   and hangup; owners must never overlap, a busy diagnostic may retain its
   existing skip behavior, and cleanup must allow the next diagnostic/call.
   A diagnostic already holding the speaker can still cause existing call-audio
   startup failure; this phase does not add diagnostic preemption.
6. **Reconnect/offline:** save settings while the ESP32 is offline, reconnect and
   verify convergence without re-pairing. Reconnect again with an unchanged
   snapshot: the duplicate count may increase, but cache writes must not. Test
   transport loss during ringing, answer and cleanup. Repeat at least ten
   call/hangup cycles per role, comparing idle heap and recording any failures.
7. **Quiet Hours:** set the household timezone explicitly (for example
   Europe/Berlin), select today's weekday and a short upcoming interval. With
   manual silence off, observe server-driven silence at its start and normal
   ringing for a new call after its end. With manual silence on, the end must
   remain silent. Test an overnight interval, reboot inside the window, and
   crossing a boundary offline followed by reconnect. DST, weekday and
   start-inclusive/end-exclusive assertions belong to server tests or an
   isolated server fixture; do not alter the production clock or add a device
   schedule. Exercise the documented Silence All-off/unrelated-save cases and
   record upstream push gaps as server findings.

Physical NVS fault injection is optional and must use a disposable test setup,
not erase the paired board. Automated tests inject open/set/commit failures and
verify retries, latest-RAM behavior and untouched pairing. If a real storage
failure occurs, retain logs/status and recover storage without resetting pairing.
Record each hardware case as passed/failed/not run, separately from host results.

## Remaining limits

No server, Pi or Pico source/deployment change is required or made for this
phase. The known server effective-silence/push gaps in the compatibility analysis
remain; ESP32 cannot reconstruct schedules or distinguish manual from scheduled
silence from one boolean. Settings-worker allocation failure is logged and
reported as `ESP_ERR_NO_MEM`; authenticated settings still apply in RAM, but
pending changes cannot become durable until a successful subsequent boot.
Pairing storage remains untouched. Ringer allocation failure leaves logged, answerable
incoming calls. These degraded states are not successful audio acceptance.

Transport security is unchanged: the development signaling configuration uses
`ws://`, so device-token authentication does not provide confidentiality or
on-path integrity. This phase adds no privileged maintenance actions. **DTLS
fingerprint verification remains a separate unresolved limitation** of the
existing peer integration; settings/ringing tests do not verify remote media
identity. Neither issue is repaired or waived by these tests. Voice Style DSP,
voicemail, remote reboot and OTA/rollback remain future phases.
