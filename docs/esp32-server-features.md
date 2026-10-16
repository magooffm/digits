# ESP32 compatibility with Digits server features

Source analysis, 2026-10-14. No feature implementation, firmware flash, deployment,
or hardware acceptance test was performed for this document.

Subsequent Phase 1 implementation is recorded separately in
[esp32-settings-ringing.md](esp32-settings-ringing.md). The baseline matrix and
source-derived wire contracts below remain the compatibility specification.

## Scope and evidence

- Server baseline: `server/v1.99.0`, commit `a4edaaa`.
- ESP32 baseline: `b891ab7`, ESP-IDF 5.5.3; working pairing, registration,
  signaling, Ring Test, WebRTC and full-duplex Opus are preserved.
- `git diff a4edaaa b891ab7 -- server pi firmware` is empty. The inspected server,
  original Pi client and Pico sources therefore match the stated baseline.
- Implementation code takes precedence over comments and design documents.
  Source-derived defects below are analysis findings, not newly reproduced
  hardware failures. Proposed changes and acceptance tests are explicitly future work.
- The existing [call-audio validation](esp32-call-audio.md) remains the authority
  for physical audio evidence and its qualifications.

## Compatibility matrix

| Feature | Authority / implementation upstream | ESP32 at `b891ab7` | Compatibility work | Server change |
| --- | --- | --- | --- | --- |
| Copper Wire / Modern HD | Server stores choice; Pi applies transmit DSP live | No settings consumer or selectable voice filters | Settings cache and live, frame-boundary TX filter selection | None for existing contract |
| Silent Mode | Server stores flag; Pi suppresses bell, retaining call handling | No settings consumer; incoming calls currently log/ring in state only | Incoming-call indication plus independent audible-ring suppression | None for per-line toggle; Silence All interaction has a genuine gap |
| Quiet Hours | Server evaluates local civil time; sends effective SilentMode | Ignores effective setting | Consume/cache `silent_mode`; no device schedule engine | Existing contract sufficient; push correctness/reliability gaps identified below |
| Answering Machine | Server stores settings/count; Pi answers, plays greeting, records and retrieves locally | No voicemail state machine, durable media store, greeting or count reporting | Local answering/recording/storage/retrieval and existing signaling | No cloud mailbox required; upstream Pi limitations must not be mistaken for ESP32 obligations |
| Remote Reboot | Household admin command over authenticated device signaling; Pi executes | `restart` ignored | Validate modes; coordinate teardown/restart and preserve NVS | None for reboot; service-restart semantics need an ESP32 decision |
| Remote / Automatic Updates | Working Pi binary + Pico ELF pipeline; server release index and progress UI | Update messages ignored; no OTA slots or updater | Secure platform-specific artifacts, OTA partition migration, verification/rollback, idle policy and status | **Required for first-class ESP32 artifacts, targeting and version display**; existing `firmware` means Pico |

## Shared settings and signaling contract

### Authentication, targeting and delivery

The WebUI routes use a signed-in session. Per-line settings require membership
in the owning household. Manual restart/update require that household's
`household_members.role == "admin"`; this is **not** the global `AdminEmails`
operator allowlist. Missing/foreign lines return 404; a member lacking the
required admin role gets 403 `forbidden`.
See [ownership checks](../server/internal/web/handler_helpers.go#L23),
[route registration](../server/internal/web/handler.go#L563) and
[origin checks](../server/internal/web/middleware.go#L64).

Device registration is the first application WebSocket message, within ten
seconds of connection:

```json
{"type":"register","number":"<paired-number>","hardware_id":"<persistent-id>","device_token":"<paired-token>"}
```

Unpaired ESP32 registration uses `number: "unpaired"` and omits `device_token`.
The server verifies the token against the hardware ID and uses the DB-bound
line as authoritative. It can send `line_renumber` with `number` to repair a
stale cached number. Errors include `must send register message first`,
`hardware_id required`, `device_token required`, `invalid device_token`,
`device has no assigned line`, `internal error` and `server shutting down`.
Registration errors close the socket. There is no success `register_ack`.
The current ESP32 persists pairing/renumbering and reconnects already.
See [server registration](../server/internal/web/handler_ws.go#L17) and
[ESP32 registration](../esp32/main/signaling.c#L179).

Settings are line-wide: `Hub.SendTo(number, ...)` fans out to connected devices
on that line. Device maintenance selects a `hardware_id` belonging to the line
and uses `SendToHardware`; omitted selection defaults to the first DB device,
with a legacy line-level fallback when no device rows exist. Device-originated
`from` and `hardware_id` are overwritten from the registered connection; they
are not accepted as authority from JSON. `restart` received from a device is
ignored, not forwarded to another device.
See [WS read pump](../server/internal/web/handler_ws.go#L239),
[relay](../server/internal/signaling/relay.go#L205) and
[device command selection](../server/internal/web/handler_phones.go#L1042).

The hub has bounded queues and best-effort Redis pub/sub fan-out, not a durable
offline command queue. A successful HTTP `{"status":"triggered"}` means the
server accepted/enqueued/published the command, **not** device execution.
`SendTo` can drop a full local queue while returning success; remote-pod
publication likewise is not a delivery acknowledgement.
See [hub delivery](../server/internal/signaling/hub.go#L716) and
[HTTP command response](../server/internal/web/handler_phones.go#L1111).

### Authoritative settings snapshot

One message serves Voice Style, Silent Mode, Quiet Hours' result, Auto Update,
and voicemail configuration:

```json
{
  "type": "line_settings",
  "to": "<line-number>",
  "line_settings": {
    "voice_style": "copper",
    "silent_mode": true,
    "auto_update": true,
    "voicemail": {"enabled": true, "ring_timeout_seconds": 20}
  },
  "voicemail_unheard_count": 0
}
```

The Go envelope's `voicemail_unheard_count` has **no `omitempty`**, so it appears
as zero even on unrelated server/Pi messages. It is meaningful as a mailbox
snapshot only for `type: "voicemail_state"`. Other unused envelope fields
are omitted. Contract examples in later sections omit this incidental zero
for readability unless showing the full envelope.
See [server protocol](../server/internal/signaling/protocol.go#L81) and
[Pi mirror](../pi/digitsd/internal/signal/protocol.go#L143).

Inside a present `line_settings` object:

| Field | Actual receiver semantics |
| --- | --- |
| `voice_style` | `copper` / `modern`; missing/empty falls back to `copper`. Server normalizes unknown values to `copper`. |
| `silent_mode` | Boolean; false is omitted by Go serialization. Missing therefore means **false**, not “keep previous true”. Contains the effective result of manual silence OR Quiet Hours on the normal push paths. |
| `auto_update` | Boolean; omitted means false. Pi's local development skip flag is an additional override. |
| `voicemail` | Optional object; absent/null preserves Pi's cached voicemail settings for compatibility with older servers. Present object replaces both fields. Inner booleans and integer seconds are explicit, without `omitempty`. |

There is no `quiet_hours`, timezone, settings revision, settings-request RPC or
settings-applied acknowledgement in this wire struct. Do not invent one for
ESP32 compatibility. A missing/null `line_settings` payload is ignored by Pi
with a warning. The normal paths push after a successful settings save and
after paired registration; Quiet Hours also pushes on scheduler transitions.
See [projection](../server/internal/signaling/linestore_adapter.go#L21),
[registration push](../server/internal/signaling/relay.go#L746),
[save/push](../server/internal/web/handler_phones.go#L805) and
[Pi receiver](../pi/digitsd/cmd/digitsd/dispatch.go#L474).

Settings endpoints accept forms, not a JSON settings API: success is a 200
HTMX HTML partial or a 303 redirect. Malformed forms return 400, DB failure
returns 500 `internal server error`, and push failure only logs. Unchanged
normalized settings skip both DB write and push. The maintenance
`{"status":"triggered"}` response does not apply to these settings saves.
Concurrent form saves replace whole settings snapshots without revision/CAS,
so a stale read can overwrite another setting's concurrent change.

### Persistence and session rules for ESP32

The server DB is authoritative. Mirror Pi's cached settings in a separate,
versioned NVS record: normalized voice style, **effective** silent flag,
auto-update preference, and voicemail enabled/ring timeout. This preserves
last-known behavior before the next server push and when a push fails. It does
not enable offline Quiet Hours transitions: the schedule is never transmitted.
Do not persist restart commands or treat a missed release broadcast as a
durable update request. OTA pending/confirmed-image metadata is separate from
line settings and must survive reboot.

Implement parsing, validation and bounded task handoff outside the WebRTC
owner's media loop; persist changed values outside the WebSocket callback.
Preserve the existing generation-tagged outbound queue and discard work from
old sessions. Coordinate maintenance with both an idle call state **and**
completed codec cleanup. These are firmware recommendations, not existing
behavior. No new on-wire acknowledgement is proposed by this analysis.

## Current ESP32 implementation and OTA baseline

`signaling.c` handles pairing, renumbering, Ring Test and server errors, then
delegates recognized call messages to `webrtc.c`. The latter accepts `ring`,
`sdp`, `answer`, `ice`, `hangup`, `busy`, `error`, `ice-servers` and
`ice_restart` (with its existing unsupported-restart handling). It does not
consume `line_settings`, `restart`, `update_trigger` or `release_available`.
There is no `device_info`, `update_status` or `voicemail_state` reporting.
The zero count on ordinary `send_wire` messages is not voicemail support.
See [dispatch](../esp32/main/signaling.c#L129),
[call-message filter](../esp32/main/webrtc.c#L916) and
[send envelope](../esp32/main/webrtc.c#L293).

Incoming `ring` currently enters `CALL_RINGING`, logs a console instruction and
waits for manual answer/hangup (or the lab-only compile-time auto-answer option).
It does not start a production audible ringer/LED indication. The existing
one-second Ring Test is a separate diagnostic and must not be mistaken for
incoming-call ringing. Silent Mode therefore cannot be declared supported
merely because current incoming calls make no bell sound.
See [incoming call](../esp32/main/webrtc.c#L690) and
[Ring Test](../esp32/main/ring_test.c#L25).

NVS currently holds only persistent hardware ID and an atomic pairing blob
containing number/token in namespace `digits`. Boot deliberately does not
erase NVS on failure. Wi-Fi configuration is compiled in and set with
`WIFI_STORAGE_RAM`. Signaling disconnection tears down ESP32 media; existing
Pi grace/recovery behavior is not yet implemented on ESP32.
See [credentials](../esp32/main/credentials.c#L28),
[boot](../esp32/main/app_main.c#L12),
[Wi-Fi](../esp32/main/wifi.c#L34) and
[transport handling](../esp32/main/webrtc.c#L794).

The tracked [partition table](../esp32/partitions.csv) is:

| Partition | Type/subtype | Offset | Size |
| --- | --- | --- | --- |
| `nvs` | data/nvs | `0x9000` | `0x6000` (24 KiB) |
| `phy_init` | data/phy | `0xf000` | `0x1000` (4 KiB) |
| `factory` | app/factory | `0x10000` | 4 MiB |

[Defaults](../esp32/sdkconfig.defaults) select 16 MiB flash. There is no
`otadata`, `ota_0`, `ota_1` or durable voicemail filesystem partition, and no
application calls to the ESP-IDF OTA APIs. Free flash address space is not an
OTA slot. The available local, untracked `esp32/sdkconfig` has
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, signed-app enforcement, Secure Boot,
flash encryption and NVS encryption disabled; it is a local configuration
observation, not proof of the private image currently on the board. Tracked
defaults do not enable those protections either.

Before implementing OTA, design and validate a one-time partition/bootloader
migration preserving the NVS/PHY offsets and pairing. Reserve suitable OTA
slots and metadata, choose whether to retain a factory recovery image, enable
rollback in the actual bootloader build, and implement pending-image health
confirmation/fallback. Do not attempt an in-place overwrite of the running
factory image as an ordinary remote update. Plan mailbox storage together with
OTA capacity. No partition layout or migration is changed in this phase.

Keep the known-good 1,000 Hz audio build configuration and existing worker
priorities/stacks during later feature work. The local old `sdkconfig` still
selects 100 Hz; it must not silently replace the private hardware-tested
configuration described in [audio validation](esp32-call-audio.md#validation-status).

## 1. Voice Style: Copper Wire vs Modern HD

**Upstream:** implemented UI → DB → settings push → live Pi transmit filters.
Pico has no voice-processing role. **ESP32:** not implemented.

The [voice controls](../server/internal/web/templates/phone-detail.html#L222)
POST form field `voice_style=copper` or `voice_style=modern` to
`/phones/{number}/voice-style`. The [handler](../server/internal/web/handler_phones.go#L653)
trims whitespace, rejects empty with HTTP 400 `missing voice_style`, and
normalizes other unknown values to copper. `lines.settings.voice_style` is a
JSONB property; copper is the default. Settings reads merge defaults and
normalize; individual saves replace the normalized settings object.
See [schema](../server/internal/db/db.go#L327),
[defaults](../server/internal/line/settings.go#L50) and
[store](../server/internal/line/store.go#L334).

The [Pi receiver](../pi/digitsd/cmd/digitsd/dispatch.go#L479) applies the change
live, then saves `/data/digits/config.json`. A new call reads the cached value
in [newPipeline](../pi/digitsd/cmd/digitsd/webrtc.go#L437). The precise processing is:

| Stage | Copper | Modern |
| --- | --- | --- |
| ALSA capture | Left channel extracted to 48 kHz mono, 20 ms frames | Same |
| Optional pre-denoise bandpass/notch chain | Default off; independent of Voice Style | Same |
| RNNoise | Default on when available; initialization failure falls back without it | Same |
| Post-denoise character chain | Two second-order 300 Hz high-pass sections, followed by two second-order 3,400 Hz low-pass sections | No character chain |
| Outbound microphone mute and Opus handoff | After character processing | Same |

Each character section uses RBJ coefficients, Q = √2/2, direct-form-I recurrence
and float64 state; final output clamps to signed 16-bit PCM. Mirror the actual
four-section cascade, not an assumed single fourth-order Butterworth design.
The separate `NewPOTSChain` 200 Hz/high-pass/notch-comb implementation is not
the Voice Style chain. The setting changes neither Opus rate/bitrate nor
speaker playback, mic gain, ringing or signaling. Injected prompt frames
bypass this capture DSP. See [pipeline order](../pi/digitsd/internal/audio/pipeline.go#L260)
and [filter implementation](../pi/digitsd/internal/audio/filters.go#L165).

Live switching atomically replaces the chain pointer for the next capture
frame; selecting copper creates fresh filter history, with no crossfade.
Idle changes affect the next call; cached style survives reboot/offline;
registration refreshes it from the server. Save failure is logged after live
state changed, with no remote error/acknowledgement or state rollback. There
is an upstream malformed-input edge case: unknown nonempty styles become
copper live but disable character on a newly created pipeline. Normal server
normalization prevents it; ESP32 should consistently normalize to copper.
See [live switch](../pi/digitsd/internal/audio/pipeline.go#L81),
[cache helpers](../pi/digitsd/cmd/digitsd/main.go#L425) and
[atomic config save](../pi/digitsd/internal/config/config.go#L222).

**Required firmware:** consume/cache the snapshot; add the same character
chain before ESP32 Opus encoding and switch at frame boundaries without
per-frame allocation. Preserve 48 kHz/960-sample timing and existing gain.
Porting the character chain alone does not reproduce Pi RNNoise: current
ESP32 has no equivalent noise suppression. Treat that as an explicit acoustic
parity difference, not an effect of the Modern HD selector. No normal-path
server change is needed.

## 2. Silent Mode

**Upstream:** client-side bell suppression controlled by server settings.
This is neither call rejection/forwarding nor microphone/speaker-call mute.
**ESP32:** no setting or production incoming-call ringer yet.

The [checkbox](../server/internal/web/templates/phone-detail.html#L249)
auto-posts `silent_mode=on` to `/phones/{number}/silent-mode`; omitted/other
values become false. The [handler](../server/internal/web/handler_phones.go#L671)
stores explicit `lines.settings.silent_mode` (default false), then normally
pushes the effective flag including Quiet Hours. Pi saves the effective flag,
not the distinction between manual and scheduled silence.

[Pi incoming-call handling](../pi/digitsd/internal/phone/controller.go#L801)
still enters RINGING, blinks the LED, permits answer and arms voicemail after
its ordinary timeout. Only audible `StartRing` is skipped. Distinctive
call-return ringing also honors the flag. Enabling silent mode mid-ring stops
the bell immediately; disabling it mid-ring deliberately does not restart the
bell. An established call's audio is unaffected. Reboot uses cached state;
registration refreshes it. See [live setter](../pi/digitsd/internal/phone/controller.go#L212)
and [startup cache application](../pi/digitsd/cmd/digitsd/main.go#L2081).

Pi uses ordinary UART `RING:START`, `RING:STOP` and `LED:BLINK`; Pico has no
silent-mode settings store. The ringer is a 20 Hz H-bridge output with a
2-second-on/4-second-off standard cadence. `RING:ACK` / `RING:DONE` are Pico
serial responses, not WebSocket settings acknowledgements. Explicit Ring Test
bypasses silent ringing policy upstream. See [serial commands](../pi/digitsd/internal/phone/serial.go#L270),
[Pico ring/LED handler](../firmware/src/phone_fsm.c#L160),
[ringer](../firmware/src/ringer.c#L7) and
[Ring Test dispatch](../pi/digitsd/cmd/digitsd/dispatch.go#L441).

**Upstream integration caveat:** the controller intends to retain blinking
when enabling silence mid-ring, but its `StopRing` becomes Pico `RING:STOP`,
which sets the Pico FSM/LED back to idle. The controller does not reassert
BLINK. Initial silent ringing explicitly blinks; mid-ring LED preservation
is not assured by the concrete Pi/Pico path. This is a source-derived issue,
not a new hardware result. See [Pico idle transition](../firmware/src/phone_fsm.c#L112)
and [STOP handling](../firmware/src/phone_fsm.c#L176).

**Required firmware:** a separate incoming-call indication policy, audible
ringer enable, visual indication and cached effective flag. Preserve the
call state when muting the ringer; explicitly preserve its indicator.
Keep Ring Test as an intentional diagnostic using existing speaker ownership.
This does not require TH-07 wiring or a hook-switch implementation: use the
existing call controls for initial acceptance. No per-line-toggle server
change is needed; the separate Silence All gap is below.

## 3. Quiet Hours

**Upstream:** server-side schedule and timezone evaluation; Pi/Pico only obey
the resulting Silent Mode. **ESP32:** needs that same flag consumer, not a clock
or local schedule engine for baseline compatibility.

The [form](../server/internal/web/templates/phone-detail.html#L327) POSTs
`enabled=on`, `start=HH:MM`, `end=HH:MM`, and `day_0=on` … `day_6=on` to
`/phones/{number}/quiet-hours`. Sunday is index zero. Persisted JSONB includes,
for example:

```json
{"quiet_hours":{"enabled":true,"start":"22:00","end":"07:00","days":[false,true,false,false,false,false,false]}}
```

These fields are **DB fields, not a device message**. The UI's blank-state
22:00/07:00 values do not mean the schedule is enabled. Invalid/equal times
normalize to disabled; valid enabled times with no selected days normalize
to all days (despite an inconsistent source comment). Disabled schedules
retain valid times/day selections. See [handler](../server/internal/web/handler_phones.go#L699)
and [normalization](../server/internal/line/quiethours.go#L110).

The server evaluates `explicit_silent || schedule_active` in the owning
household's IANA timezone from `households.timezone`, using `time.LoadLocation`
and local hour/minute/weekday. Invalid stored timezone falls back to UTC.
Same-day windows are start-inclusive/end-exclusive. An overnight window
belongs to its opening weekday: selected Monday 22:00–07:00 includes Tuesday
morning even if Tuesday is unselected. See [effective settings](../server/internal/line/store.go#L167)
and [window evaluation](../server/internal/line/quiethours.go#L55).

DST follows conversion of the current instant into that timezone. Repeated
fall-back clock minutes are evaluated on each occurrence; nonexistent spring
clock minutes are never sampled, and the schedule may change at the clock
jump. This is a consequence of the local-time predicate, not a fixed-UTC
alarm or a device DST algorithm. The timezone form uses
`POST /settings/timezone` with `timezone=<IANA name>`, requires household admin
and validates via `time.LoadLocation`; it does not immediately fan out a
snapshot. The next scheduler evaluation picks up the new effective state.
See [timezone handler](../server/internal/web/handler_settings.go#L139) and
[validation](../server/internal/household/store.go#L446).

Saving a line setting normally pushes effective state immediately. Registration
recomputes it; the scheduler evaluates connected lines once per minute, sends
on first observation, then only on effective-bit changes. Expect up to a tick
of normal boundary delay. During an active call this only changes ringing
policy. While disconnected across a boundary, Pi keeps its last cached bit;
reboot also uses that bit until another push. ESP32 cannot infer the current
schedule from this contract. See [scheduler](../server/internal/signaling/quiethours.go#L59).

**Genuine server gaps, separated from ESP32 work:**

- “Silence All” is a batch write of each line's explicit flag, not a separate
  household DND bit. Its `/settings/do-not-disturb` handler (`enabled=true`
  or otherwise false) pushes raw state. Turning it off during Quiet Hours can
  incorrectly send false. If the scheduler already remembers effective true,
  subsequent ticks do not repair that false until another relevant push or
  transition. See [batch handler](../server/internal/web/handler_settings.go#L104),
  [batch persistence](../server/internal/line/store.go#L357) and
  [removed DND column](../server/internal/db/db.go#L510).
- The scheduler updates `lastSent` before attempting delivery; delivery errors
  do not cause a steady-state retry. Hub queues may also drop with a success
  return. Reconnection helps convergence but is not an acknowledgement.
  See [transition deduplication](../server/internal/signaling/quiethours.go#L108).
- A normal setting save whose effective-settings lookup fails falls back to
  raw settings, which can also send the wrong silent bit during a quiet window.
  See [save fallback](../server/internal/web/handler_phones.go#L830).

ESP32 should honor the received snapshot and document the offline limitation;
it cannot correct a wrong effective bit without unavailable schedule/reason
data. Fixing these server push paths is justified future server work, not a
reason to invent ESP32-only Quiet Hours messages.

## 4. Answering Machine

**Upstream:** shared settings/reporting, but answering, greeting, recording,
retention and retrieval all run on the Pi. The server does not host a mailbox
or forward calls to a voicemail service. This feature is separate from the
WebUI's similarly named Answering Machine visual theme. **ESP32:** absent;
lab auto-answer and RAM recording diagnostics do not implement it.

### UI, storage settings and reporting contract

The [voicemail controls](../server/internal/web/templates/phone-detail.html#L288)
POST `enabled=on` and integer `ring_timeout_seconds` to
`/phones/{number}/voicemail`. Valid timeout is 5–60 seconds; invalid input
returns HTTP 400 `ring_timeout_seconds must be an integer between 5 and 60`.
`POST /phones/{number}/voicemail-toggle` flips enabled while retaining the
timeout. Both allow household members. DB fields are
`lines.settings.voicemail.enabled` and `.ring_timeout_seconds`. Current new-line
creation persists true and 20 seconds; normalization replaces out-of-range
timeouts with 20. Legacy DB rows without a voicemail block instead load disabled
with timeout 20, because the DB merge overwrites the boolean with its zero value.
This differs from an absent voicemail block on the wire, which preserves cache.
See [handlers](../server/internal/web/handler_phones.go#L741) and
[defaults/merge](../server/internal/line/settings.go#L14),
[line creation](../server/internal/line/store.go#L116) and
[legacy-default test](../server/internal/line/settings_test.go#L277).

Configuration uses the shared `line_settings` snapshot above. An absent/null
`voicemail` object leaves the Pi's previous settings unchanged; explicit
`{"enabled":false,"ring_timeout_seconds":20}` disables it. Pi converts wire
integer seconds into its local `ring_timeout` duration and saves config.
HTTP save success is not proof that local storage or auto-answer works.
There is no voicemail-specific settings acknowledgement/error RPC.

Device → server count reporting is exactly:

```json
{"type":"voicemail_state","voicemail_unheard_count":3}
```

Publish an explicit zero when all messages become heard/deleted. Pi omits
identity fields; the authenticated handler supplies `hardware_id` and `from`.
The server consumes the report without an acknowledgement, clamps negative
counts to zero, sums per-device counts for the line, and removes a device's
count on disconnect. The badge appears only for enabled voicemail with a
positive sum. These are counts, not audio uploads. There is no corresponding
web audio list/play/download/delete endpoint in the inspected implementation.
See [publisher](../pi/digitsd/cmd/digitsd/main.go#L482),
[relay](../server/internal/signaling/relay.go#L284),
[count cache](../server/internal/signaling/hub.go#L1004) and
[badge](../server/internal/web/templates/_partials.html#L25).

### Call routing and local audio

| Direction | Existing message / behavior |
| --- | --- |
| Caller → server | `{"type":"call","to":"<callee>"}` then `{"type":"sdp","to":"<callee>","sdp":"<offer>"}` and ordinary `ice` with string `candidate` |
| Server → callee | `{"type":"ring","from":"<caller>"}`; normal forwarded offer/ICE |
| Callee → server at local timeout | `{"type":"answer","to":"<caller>","sdp":"<answer>"}` followed by ordinary ICE |
| Server → caller | Forwarded answer/ICE with authenticated `from` and stamped `hardware_id`; other ringing devices on the answered line receive ordinary `hangup` |
| Either device → server to end | Ordinary `hangup`, optionally `reason`; media-connect failure uses existing `connect_timeout` |
| Target offline | `{"type":"error","error":"phone not connected"}`; busy/grace-held targets use ordinary `busy` with target `from` |

The incidental zero-count envelope rule applies to this table. There is no
voicemail flag on the answer, dedicated recording message or server media
route. See [server answer routing](../server/internal/signaling/relay.go#L435)
and [Pi auto-answer](../pi/digitsd/cmd/digitsd/voicemail.go#L229).

Pi snapshots voicemail enabled/timeout when a new ring begins and arms a
generation-guarded timer. Silent Mode/Quiet Hours suppress the bell but leave
the same timer and answer path intact. Settings changes do not retime/cancel
an already armed timer or stop an active greeting/recording. At timeout Pi
answers the normal prepared peer, waits for media, sends a default/custom
greeting and a 500 ms, 1 kHz beep, then records the caller. The caller's audio
is suppressed from the local earpiece during unattended recording. Recording
stores received Opus RTP payloads after the recording gate opens, without RTP
headers/timestamps, not local microphone PCM or server WAVs.
See [ring timer](../pi/digitsd/internal/phone/controller.go#L801)
and [audio/recording sequence](../pi/digitsd/cmd/digitsd/voicemail.go#L304).

Pickup during greeting/recording finalizes any nonempty partial message and
continues the same two-way call with speaker/microphone connected. Hangup
finalizes; empty recordings are discarded. The cap is 30,000 stored 20-ms
frames (ten minutes of stored media), then beep/hangup; packet loss/skips can
extend wall-clock recording beyond ten minutes. New calls while busy receive busy. On signaling reconnect,
Pi tears down voicemail/ringing sessions rather than using its normal-call
media-resume path. See [pickup](../pi/digitsd/cmd/digitsd/voicemail.go#L154),
[recording](../pi/digitsd/cmd/digitsd/voicemail.go#L422) and
[reconnect](../pi/digitsd/cmd/digitsd/main.go#L2650).

### Durable data and local controls

Default storage is `/data/digits/voicemail`, beside the config file:

- `<unix_ms>.frames`: repeated little-endian uint32 length followed by raw
  Opus packet bytes. This is not WAV or Ogg.
- `<unix_ms>.meta`: JSON `heard`, `duration_ms`, `recorded_at`.
- `greeting.frames`: custom greeting, up to 60 seconds.
- Maximum 50 messages; FIFO deletion of oldest, even if unheard. Duration is
  stored-frame count × 20 ms, without original RTP timing, so loss/skips can
  shorten playback and measured duration.
- Directories are 0700 and files 0600; no at-rest cryptography in this package.
  Writes use temporary files, fsync and rename. Reboot preserves finalized
  messages/greeting; startup deletes unfinished temporary recordings.

See [store format/limits](../pi/digitsd/internal/voicemail/store.go#L1),
[finalization](../pi/digitsd/internal/voicemail/store.go#L258) and
[retention](../pi/digitsd/internal/voicemail/store.go#L507).

Local service codes are `*98` retrieve unheard then saved messages, `*97`
record greeting (`#` finishes), `*96` audition greeting and `*99` delete it.
During retrieval: `7` delete/next; `9` mark heard/next (saved phase: next);
`#` skip without marking; `*` replay. EOF marks heard. These are local keypad
actions, not new WebSocket messages. Unheard messages change idle LED OFF
to `SLOWER_PULSE`. Pico supplies hook/key/ringer/LED functions only; its
`CALL:CONNECTED`/`RESET` serial handling keeps local retrieval keys working.
See [key handling](../pi/digitsd/internal/phone/controller.go#L549),
[retrieval](../pi/digitsd/cmd/digitsd/voicemail.go#L938),
[greetings](../pi/digitsd/cmd/digitsd/voicemail.go#L555) and
[Pico commands](../firmware/src/phone_fsm.c#L201).

### Upstream limitations and required ESP32 changes

- **Pi store initialization:** store opens only when enabled at boot. A later
  settings push enables config but does not open a missing store, so live
  activation after a disabled boot/store-open failure is ineffective until
  restart. ESP32 should initialize storage safely on enable or open it at
  startup. [Startup](../pi/digitsd/cmd/digitsd/main.go#L2010),
  [settings receiver](../pi/digitsd/cmd/digitsd/dispatch.go#L519),
  [availability check](../pi/digitsd/cmd/digitsd/voicemail.go#L548).
- **Pi reconnect count:** the actual reconnect path publishes with `force=false`.
  An unchanged count can therefore be suppressed after the server removed it
  on disconnect, until a count change or forced snapshot. ESP32 should force
  a snapshot on each new authenticated session. [Reconnect call](../pi/digitsd/cmd/digitsd/main.go#L2667),
  [dedup](../pi/digitsd/cmd/digitsd/main.go#L505),
  [server removal](../server/internal/signaling/hub.go#L468).
- **Server multi-replica gap:** unread counts use only a process-local map,
  without the Redis propagation used for device/update state. A WebUI request
  on another replica may miss counts. This needs a server fix for reliable
  multi-replica badges; it does not block device-local answering.
  [Count storage](../server/internal/signaling/hub.go#L1009).
- **Pi unattended-microphone risk:** mute is applied only after the prompt/beep.
  Injection replaces microphone capture while prompt frames exist, but the
  initial 500 ms lead-in and nominal 100 ms greeting tail can send room audio.
  This is a source-derived risk, not a new acoustic observation. ESP32 should
  mute room capture for the entire unattended session while permitting the
  independent prompt source. [Mute placement](../pi/digitsd/cmd/digitsd/voicemail.go#L326),
  [prompt timing](../pi/digitsd/cmd/digitsd/voicemail.go#L1517),
  [injection bypass](../pi/digitsd/internal/audio/pipeline.go#L276).

Required ESP32 work includes an answering state machine, cancelable ring
timer, greeting/beep transmit source, gated caller recording, durable store,
retention/recovery, local retrieval controls and count reporting. Use bounded
storage queues and existing audio ownership; do not write flash from peer
callbacks or confuse diagnostic PSRAM recordings with persistent messages.
At 24–32 kbit/s, 50 ten-minute messages alone need roughly 90–120 MB before
record overhead, beyond the board's 16 MiB flash. Choose external storage or
explicitly document a capacity reduction; NVS is for configuration, not audio.

Basic compatibility requires no new server media API. Provide equivalent local
controls through an agreed existing console/UI first; full physical service-code
and pickup parity depends on future keypad/hook integration. That dependency
does not authorize TH-07 or hook-switch work in this phase.

## 5. Remote Reboot

**Upstream:** implemented, using an existing command on the registered device
WebSocket. **ESP32:** `restart` is currently ignored.

The [phone operator controls](../server/internal/web/templates/phone-detail.html#L1261)
offer service restart and device reboot. Their
[JavaScript](../server/internal/web/static/phone-detail.js#L168) submits
`POST /phones/{number}/restart` with URL-encoded `mode=service|reboot` and
`hardware_id`, using `Accept: application/json`. Household admin and device
ownership are enforced. Invalid mode returns HTTP 400 JSON
`{"error":"mode must be 'service' or 'reboot'"}`; foreign hardware returns
400 `{"error":"unknown device for line"}`.
See [handler](../server/internal/web/handler_phones.go#L926).

| Direction | Relevant JSON fields |
| --- | --- |
| Server → device | `{"type":"restart","restart_mode":"reboot"}` or `"service"` |
| Pi → server before reboot | `{"type":"update_status","update_status":"rebooting","update_detail":"Device reboot requested"}` |
| Pi → server before service restart | `{"type":"update_status","update_status":"restarting","update_detail":"Service restart requested"}` |

There is no separate restart acknowledgement, command ID, deadline or durable
job. Successful HTTP enqueue/publication returns 200 `{"status":"triggered"}`;
delivery failure returns 502 `{"error":"<hub error>"}`. Without the exact JSON
Accept header the handler redirects 303 to the phone page. Single-instance
offline errors are `hardware <id>: not connected` or legacy
`phone <number>: not connected`; Redis publication can return success with no
confirmed receiver. Offline commands are not replayed at reconnect.

Pi waits 500 ms in a goroutine, then `os.Exit(0)` for service restart or runs
`sudo reboot` for host reboot. Systemd has `Restart=always`, `RestartSec=3`.
There is **no active-call, recording or updater guard** in either handler.
Failed `sudo reboot` only logs. Host reboot is not a Pico UART REBOOT command.
See [Pi dispatch](../pi/digitsd/cmd/digitsd/dispatch.go#L407) and
[service unit](../pi/image/rootfs-overlay/etc/systemd/system/digitsd.service#L23).

The UI waits two seconds then polls
`GET /phones/{number}/online?hardware_id=<id>` once per second for an
offline→online transition: 90 attempts for reboot, 15 for service. JSON is
`{"online":true}` or false. A short disconnect missed by polling can produce
a false timeout. Reconnection restores cached configuration and gets a fresh
settings snapshot; reboot is not factory reset.
See [presence handler](../server/internal/web/handler_phones.go#L474).

**Required firmware:** accept only the existing validated command/modes,
schedule execution outside callbacks, apply an explicit active-call policy,
wait for codec/storage cleanup, preserve pairing/settings, report `rebooting`
through the existing status message and restart the ESP32. Do not claim
`service` compatibility by silently treating it as an identical hardware reboot;
define subsystem restart behavior or report the unsupported action via the
existing failure status. The current restart UI polls presence, not that
failure status, so unsupported service restart would still show a UI timeout.
A safer busy/defer policy is a deliberate firmware
improvement over Pi's immediate behavior, not an existing protocol guarantee.
No server message extension is necessary for basic remote reboot.

## 6. Remote Firmware Updates and Automatic Updates

**Upstream:** working Pi executable replacement and Pico ELF download/flash
pipeline, with version tracking and basic status. It is not an ESP32 OTA
pipeline. **ESP32:** no updater, platform release metadata, OTA partitions,
rollback application logic or update reporting.

### UI, exact WebSocket contracts and status

The [release selectors/install buttons](../server/internal/web/templates/phone-detail.html#L1177)
submit `POST /phones/{number}/update`, accepting form `hardware_id`,
`target_pi_version` and `target_fw_version`. Targets are optional strings;
the server trims them but does not validate semver, artifact existence or
device compatibility. It clears prior status and sends to the selected device.
It requires household admin, has no call-state guard and uses the same
triggered/error HTTP response as restart.
See [update handler](../server/internal/web/handler_phones.go#L867) and
[UI polling](../server/internal/web/static/phone-detail.js#L130).

| Direction | Relevant JSON fields / semantics |
| --- | --- |
| Server → device, manual update | `{"type":"update_trigger","target_pi_version":"<version>","target_fw_version":"<version>"}`. Empty fields omitted; no targets means check latest. |
| Server → connected devices, new release | `{"type":"release_available","latest_pi_version":"<version>","latest_fw_version":"<version>"}`. Empty strings omitted. This is a notification, not an installation command. |
| Device → server, progress | `{"type":"update_status","update_status":"downloading","update_detail":"<text>"}`. Actual Pi vocabulary also includes `applying`, `rebooting`, `success`, `failed`, `up_to_date`; restart uses `restarting`. |
| Device → server, identity/version snapshot | `{"type":"device_info","pi_version":"<version>","pi_commit":"<commit>","firmware_version":"<Pico-version>","firmware_commit":"<Pico-commit>","local_addr":"<LAN-address>","dev_mode":true}`. Empty values/false omitted. |
| Server → device, automatic policy | Shared `line_settings.auto_update`; omitted boolean means false. No device policy ACK. |

The incidental zero-count envelope rule applies. Version/status reports do
not need a client-supplied hardware ID; the WS handler stamps it. The Pi
sends `device_info` after connection/reconnection and firmware-version changes.
The server sanitizes `local_addr` to a private/loopback/link-local address.
There is no device-type, ESP32 version, target board, image size, signature,
operation ID or boot-confirmation field in the current envelope.
See [wire fields](../server/internal/signaling/protocol.go#L182),
[Pi device info](../pi/digitsd/cmd/digitsd/main.go#L2694) and
[hub version state](../server/internal/signaling/hub.go#L1040).

`GET /phones/{number}/update-status?hardware_id=<id>` is household-member
accessible. It returns `{"status":""}` when absent, otherwise
`{"status":"<value>","detail":"<text>","updated_at":"<RFC3339 time>"}`.
These are HTTP keys; do not confuse them with WS `update_status`/`update_detail`.
Status strings are not enum-validated. State lives in hub memory and optionally
Redis with a one-hour TTL, not a durable SQL update job/history. Version data
is connection/presence state, not firmware attestation.
See [status endpoint](../server/internal/web/handler_phones.go#L885),
[snapshot](../server/internal/signaling/hub.go#L910) and
[Redis status](../server/internal/signaling/state_redis.go#L322).

**Result limitation:** any `device_info` received while state is `rebooting`
becomes `success` with `Updated to <pi_version>`, without checking the requested
version, boot identity or actual reboot. Even an ordinary reboot can enter this
path. UI polling stops after about 120 seconds, shorter than the download
deadline. Neither HTTP triggered nor this success heuristic proves a healthy
ESP32 OTA boot. See [relay heuristic](../server/internal/signaling/relay.go#L240).

### Manifest, binary download and trust

When release support is configured, public unauthenticated
`GET /api/updates/releases` returns the following schema (placeholder versions,
URLs and digests below illustrate fields, not real releases):

```json
{
  "pi": {
    "latest": "1.2.3",
    "releases": {
      "1.2.3": {"version":"1.2.3","sha256":"<64 hex characters>","url":"https://<host>/digitsd-aarch64","date":"<date>","notes":"<text>","audio_url":"<url>"}
    }
  },
  "firmware": {
    "latest": "1.2.3",
    "releases": {
      "1.2.3": {"version":"1.2.3","sha256":"<64 hex characters>","url":"https://<host>/firmware.elf","date":"<date>"}
    }
  },
  "server": {"latest":"<version>","releases":{}}
}
```

`notes`, `audio_url`, and schema-level `sha256` use `omitempty`; actual indexed
binary releases require valid checksums. Before initial index load the
endpoint returns 404. It is not mounted if release support is absent.
See [schema](../server/internal/updates/store.go#L9),
[public route](../server/internal/web/handler.go#L538) and
[response](../server/internal/updates/github.go#L126).

The server loads a configured GitHub repository immediately and refreshes
every five minutes. Only `pi/v...`, `fw/v...`, `server/v...` tags are classified;
`firmware` means Pico. Asset matching accepts a filename ending `.elf` or
containing `aarch64`, paired with the exact filename plus `.sha256`. This is
a filename heuristic, not board/content validation; ordinary ESP32 `.bin`
assets/tags have no dedicated support. Only the newest 100 releases are fetched
without pagination; draft/prerelease flags are not explicitly filtered by the
local model. Missing/invalid hashes exclude binary releases; transient hash
fetch failures preserve the previous cache.
See [fetch/cache](../server/internal/updates/github.go#L105),
[tag and asset selection](../server/internal/updates/github.go#L289) and
[server setup](../server/cmd/signald/main.go#L297).

The server may authenticate GitHub API/hash requests with its configured
token, but Pi downloads the manifest and browser-download URLs without device
or GitHub authorization headers. A private-repository token on the server
does not by itself grant clients binary-download access. Pi derives HTTP(S)
base URL from WS(S); plain HTTP is permitted. HTTPS uses standard Go TLS
validation, with normal redirects; no URL host allowlist is enforced.
See [Pi base URL](../pi/digitsd/cmd/digitsd/main.go#L857),
[manifest request](../pi/digitsd/internal/updater/updater.go#L141) and
[download](../pi/digitsd/internal/updater/updater.go#L248).

Pi requires a 64-hex SHA-256 and verifies downloaded bytes. Manifest and
artifacts have no signature/key-ID verification, expiry or board compatibility
metadata. A hash from the same compromised/plaintext manifest is not proof
of firmware authenticity. There is no application download-size ceiling here.
Remote code installation needs a trusted transport/manifest and an explicit
ESP32 firmware authenticity policy; do not simply copy the plaintext path.

### Pi/Pico installation and version tracking

- Empty targets resolve latest newer versions; implicit latest does not
  downgrade valid semver. Explicit targets allow downgrade and equal-current
  targets skip. Unknown target gives `ErrNotInIndex`; unparseable development
  versions fall back to inequality. If either target is specified, the caller
  suppresses the non-targeted component after resolution; resolution can still
  fail on malformed unrelated metadata first.
  [Resolution](../pi/digitsd/internal/updater/updater.go#L199),
  [caller](../pi/digitsd/cmd/digitsd/main.go#L876).
- Manifest deadline is 30 seconds; downloads use 15-second connection/TLS,
  30-second header and ten-minute body/attempt limits. Staging `.tmp` and
  `.tmp.sha` support HTTP Range resume on a later run; final hash mismatch
  discards the partial. Successful download renames the verified artifact.
  Generic download failures do not schedule their own retry.
  [Download implementation](../pi/digitsd/internal/updater/updater.go#L248).
- A process-wide atomic guard excludes simultaneous updater runs, but a
  duplicate manual trigger discards the returned busy error without a new
  status. It does not exclude calls or standalone reboot commands.
  [Guard](../pi/digitsd/cmd/digitsd/main.go#L850),
  [dispatch](../pi/digitsd/cmd/digitsd/dispatch.go#L250).
- Pico firmware is applied first: staged ELF becomes
  `/data/digits/firmware.elf`; Pi invokes `flash-pico.sh`, using OpenOCD/SWD
  program/verify/reset. Missing SWD capability skips firmware. Firmware installs
  through `runTargetedUpdate` require fresh PONG and matching `VERSION` before
  success, including combined Pi+firmware updates. Pico implements UART
  `VERSION` → `VERSION:<version>:<commit>`, bootrom `REBOOT`/`REBOOT:BOOTSEL`
  with `REBOOT:OK`, and separate `REBOOT:WATCHDOG`. Pico has no network updater.
  [Apply/verify](../pi/digitsd/cmd/digitsd/main.go#L908),
  [updater](../pi/digitsd/internal/updater/updater.go#L438),
  [flash script](../pi/image/rootfs-overlay/usr/local/bin/flash-pico.sh#L61),
  [Pico protocol](../firmware/src/phone_fsm.c#L241).
- Pi executable update remounts root writable, copies/chmods/renames the new
  binary, checks installed `-version`, attempts to restore the read-only mount
  and exits for systemd restart. Remount failure logs and relies on next
  startup to restore it. Post-copy version-check failure is a warning, not rollback.
  There is no A/B recovery transaction in this updater.
  [Pi apply](../pi/digitsd/internal/updater/updater.go#L363).

### Automatic policy and exact busy behavior

The [auto-update checkbox](../server/internal/web/templates/phone-detail.html#L268)
POSTs `auto_update=on` to `/phones/{number}/auto-update`; absent/other means
false. It is a line-wide JSONB setting, default false, writable by household
members rather than only admins. Pi persists it, and its development
`skip-auto-update` flag can override/ignore server pushes. Enabling it triggers
a check. See [handler](../server/internal/web/handler_phones.go#L685),
[receiver](../pi/digitsd/cmd/digitsd/dispatch.go#L505) and
[local policy](../pi/digitsd/cmd/digitsd/main.go#L296).

Other triggers are startup with cached enabled, a `release_available`
notification while enabled, and a deferred check after hangup. The server
broadcasts when an already populated cache's latest Pi/FW changes, not on its
initial load. There is no periodic Pi manifest polling loop. Pi stores release
versions even when auto-update is disabled and later uses them as explicit
targets. Only `ErrNotInIndex` gets six attempts one minute apart.
See [release dispatch](../pi/digitsd/cmd/digitsd/dispatch.go#L265),
[auto runner](../pi/digitsd/cmd/digitsd/main.go#L794) and
[broadcast setup](../server/cmd/signald/main.go#L305).

**Deferral exists, but is incomplete:** `runAutoUpdate` samples only
`callPeer != ""` once. If set, it remembers a pending update; hangup rechecks
policy and starts the trigger. That trigger can start **before asynchronous
audio/peer teardown completes**. No idle reservation or later call-state
recheck prevents a call beginning during download/apply. Ringing, diagnostic
audio, local retrieval and pending cleanup are not covered by this single
predicate. Manual updates and reboot bypass it entirely.
See [busy check](../pi/digitsd/cmd/digitsd/main.go#L822) and
[hangup ordering](../pi/digitsd/cmd/digitsd/webrtc.go#L406).

Additional source-derived limitations:

- Startup schedules auto-update from cached policy before first WebSocket
  connection/settings dispatch, waits for SWD probing, and does not recheck
  the opt-in afterward. Cached true can beat a newer server false setting.
  [Startup](../pi/digitsd/cmd/digitsd/main.go#L2059).
- Reconnect refreshes settings but does not check releases when true stays
  true. Missed offline broadcasts or an initial offline HTTP failure are not
  repaired by a general reconnect update check; another trigger is needed.
  [Reconnect](../pi/digitsd/cmd/digitsd/main.go#L2617).
- Automatic runs pass a nil status reporter, so progress is local logs rather
  than the manual `update_status` flow; later `device_info` shows versions.
  [Auto call](../pi/digitsd/cmd/digitsd/main.go#L842).
- Pushed versions become explicit targets and thereby bypass the implicit
  no-downgrade guard. A stale/lower pushed version can select downgrade.
  [Target selection](../pi/digitsd/cmd/digitsd/main.go#L838).

**Required ESP32 work:** platform-specific version/artifact selection, bounded
secure streaming download, image/board/size/hash/authenticity checks, inactive
slot writing, rollback and health confirmation, persistent pending outcome,
honest status/version reports and policy reconciliation after reconnect.
Serialize maintenance; reserve safe idle through commit/reboot, including
ringing, voicemail, diagnostic audio and cleanup. Recheck opt-in before apply
and cancel deferred automatic work when disabled. Decide retry/backoff and
explicit-downgrade policy separately. The legacy `firmware` stream must never
be treated as ESP32 image data.

## Lifecycle summary

This table describes upstream behavior to reproduce or explicitly improve;
current ESP32 does not yet implement the six feature handlers.

| Feature | Offline / disconnected | Reconnect | Active call / media | Reboot and persistence |
| --- | --- | --- | --- | --- |
| Voice Style | Server edits persist; cached client choice remains | Fresh settings snapshot | Pi switches TX filter on next frame | Cache retained; next snapshot authoritative |
| Silent Mode | Cached effective bit remains | Effective bit recomputed/pushed | Stops ringing when enabled; established audio continues | Cache retained; no independent reason/schedule |
| Quiet Hours | No autonomous device transitions | Server recomputes current household-local time | Only ringing policy changes | Cached effective bit may be stale until pushed |
| Answering Machine | Offline device cannot receive calls; local messages remain | Settings refresh; Pi tears down stale VM sessions; count-refresh bug described above | Busy target cannot take another message; settings apply on subsequent rings | Finalized media/settings survive; unfinished temp recording lost |
| Remote Reboot | No durable command delivery | Missed command is not replayed | Pi executes without waiting for call end | Same identity/settings; presence and device-info report return |
| Manual Update | Offline command can fail or be published without receiver | No durable job replay; device-info/status heuristic | Pi does not defer; updater guard only excludes another update | Pi restarts service; firmware-only Pico flash has its own version verification |
| Automatic Update | Cached preference; no reliable missed-release queue | True→true snapshot alone does not recheck releases on Pi | Single preflight callPeer check and pending trigger, not cleanup-safe idle reservation | Cached opt-in can start check before fresh server policy; no generic OTA rollback upstream |

## Security and operational risks

### Control transport and DTLS are separate

The ESP32 application currently **accepts only plaintext `ws://.../ws`** and
rejects `wss://` in its URL assertion. No CA attachment/server TLS verification
is configured in its WebSocket client. The server authenticates the paired
device token on registration, but the ESP32 does not thereby authenticate the
server or encrypt that token and incoming commands. A certificate bundle being
compiled into local ESP-IDF config does not fix application use of plaintext.
See [URL assertion/client config](../esp32/main/signaling.c#L261).

Before enabling trusted remote maintenance, support certificate-validated WSS
and a reliable certificate-time/trust policy using the existing endpoint's TLS
deployment. Preserve credentials and do not log token/download secrets. This
is firmware/control-transport work; it does not require renaming the existing
WebSocket messages. Standard server UI routes already use session and
household authorization; no-Origin requests are permitted by its origin
guard, so do not describe it as a per-command cryptographic signature.

**DTLS fingerprint verification remains independently unresolved.** The pinned
`esp_peer` visible DTLS code selects optional certificate verification, and
comparison of the peer certificate against the signaled SDP fingerprint has
not been established from available source/library inspection. This affects
live/voicemail media peer identity, not the WebSocket commands' authorization.
DTLS/SRTP transport interoperability is not proof of authenticated media peers.
Conversely, fixing DTLS fingerprint checks would not secure plaintext WS or
an unauthenticated update manifest. See [documented limitation](esp32-call-audio.md#dtls-identity-limitation)
and [pinned dependencies](../esp32/main/idf_component.yml).

### Other concrete risks

- OTA requires an explicit authenticity, board compatibility, size and rollback
  policy. Current SHA-256-only Pi metadata is insufficient against a replaced
  manifest. Current ESP32 factory-only layout cannot provide A/B rollback.
- Protect NVS tokens and recorded audio according to the physical-access threat
  model. Current local configuration has no NVS/flash encryption; Pi uses file
  permissions, not encrypted voicemail. OTA migration must preserve pairing.
- Remote calls/voicemail expose microphone privacy risks; preserve unattended
  mute and explicitly handle the unresolved DTLS identity question. Do not
  inherit the Pi greeting-gap microphone behavior.
- Settings are best effort with no apply ACK; caches can diverge after failed
  persistence/delivery. Pi changes memory before saving, so a repeated equal
  snapshot may not retry the failed save. ESP32 should track dirty persistence
  and avoid blocking audio while retrying.
- Household members can enable automatic installation, while manual update and
  restart require household admin. Preserve this distinction intentionally;
  it is not a global-operator-only update policy.
- Reboot/update require maintenance exclusion covering call setup, ringing,
  full duplex, voicemail, diagnostics, outstanding writes and asynchronous
  cleanup. Existing Pi single-check deferral does not provide this guarantee.
- Existing ESP32 timing/headroom is already constrained and occasional local
  playback starvation remains. Revalidate DSP/storage/download load with stage
  counters, heap/stack and physical speech tests. None of this analysis extends
  the current LAN scope to TURN, Internet NAT, groups or signaling-loss recovery.

## Necessary server changes versus firmware work

No server/Pi/Pico code is changed by this document. The following is a proposed
future scope, not an invented replacement protocol:

| Category | Change and justification |
| --- | --- |
| **Required for first-class ESP32 updates** | Introduce an explicitly specified ESP32 platform/artifact selection and accurate version presentation; extend release ingestion and operator targeting/UI accordingly. Current `pi`/`firmware` fields target Linux/Pico and have no board discriminator. A separate artifact service is possible, but would not make the existing Pi/Pico WebUI update selectors ESP32-compatible. Keep current Pi/Pico meanings backward compatible. |
| **Genuine Quiet Hours correctness gaps** | Use effective settings for Silence All fan-out and handle effective-lookup failure without sending a false raw value. Add reliable reconciliation/retry behavior for failed/dropped settings pushes. Existing wire snapshot can be retained; do not claim a new ACK contract already exists. |
| **Genuine multi-replica voicemail reporting gap** | Share per-device unread counts across replicas with disconnect/reconnect lifecycle handling if that deployment mode must show accurate badges. No server audio storage is needed. |
| **Recommended update-result/UI hardening** | Stop treating any device-info report after `rebooting` as proof of update success; distinguish requested/running versions and validated outcome. Adjust UI for ESP32/service semantics and longer/deferred operations. Any new correlation/boot-confirmation fields require a separately agreed backward-compatible contract; none is defined here. |
| **No server change for normal baseline behavior** | Existing line-settings payload supports Voice Style, per-line Silent Mode, effective Quiet Hours and voicemail settings; ordinary call signaling supports local voicemail; existing restart message supports device reboot. |
| **Pi-specific issues, not permission to modify Pi** | Live voicemail store initialization, unchanged-count reconnect suppression, mid-ring Pico LED behavior, greeting microphone leakage, update busy/reconnect/bootstrap races. Implement appropriate ESP32 behavior and record upstream issues separately; leave original Pi/Pico unchanged. |

## Recommended implementation order

1. Preserve the known-good image/configuration and pairing backup procedure.
   Add validated WSS control transport and settle update trust/platform identity
   before enabling remote execution. Resolve media fingerprint verification as
   a separate security workstream.
2. Add bounded `line_settings` parsing, normalized snapshot/cache semantics,
   NVS persistence and reconnect reconciliation. Handle all existing fields
   without enabling unfinished subsystems. Add incoming-call indication and
   Silent Mode; Quiet Hours then uses the same consumer. Track server push gaps
   before claiming full scheduled-silence correctness.
3. Add exact Copper/Modern TX processing and live switching. Measure CPU,
   heap/stack, skipped frames and actual speech; keep noise suppression parity
   explicit.
4. Introduce a shared maintenance/idle coordinator and safe remote reboot.
   Decide `service` handling and version reporting with honest UI semantics.
5. Plan partition/storage migration for both OTA and voicemail, then implement
   dedicated ESP32 release delivery, inactive-slot install, rollback and boot
   health. Validate manual updates first; add automatic policy reconciliation,
   deferral, retry and downgrade prevention afterward.
6. Add durable local answering-machine storage and audio state machine, then
   greeting, retrieval, count reporting and local controls. Physical hook/keypad
   parity remains a separately scoped hardware phase. Revalidate the existing
   call, Ring Test and recording behavior after each step.

## Acceptance tests for subsequent implementation

These are required future checks, **not results of this source-only phase**.
Use fixtures from the baseline and real devices where acoustic/flash behavior
is involved. Never infer audible or OTA interoperability from JSON or unit
tests alone.

### Common settings, session and regression tests

- Verify household member/admin/foreign-household permissions, hardware-specific
  command targeting and line-wide settings fan-out. Reject spoofed device
  identity; retain server binding across renumber/reconnect.
- Decode exact full snapshots, unknown extra fields, missing whole payload,
  absent false booleans after previously true values, absent/null voicemail
  versus explicit false, enum normalization and timeout bounds.
- Apply repeated snapshots idempotently; persist only changes, survive reboot,
  retry NVS failure without erasing pairing, and reconcile offline edits on
  registration. Discard stale-session work; keep callbacks bounded.
- Invalid/untrusted TLS certificate, wrong host and unavailable time/trust
  data must not silently fall back to plaintext maintenance. Test DTLS wrong
  fingerprint separately against both call roles.
- Preserve pairing, ordinary call setup/hangup, both audio directions, Ring
  Test/recording exclusion and cleanup; measure heap/stack and media skips
  under settings, DSP and storage activity.

### Voice Style

- Compare the four-biquad output with upstream coefficients at 48 kHz. Use
  upstream response expectations: ≥30 dB attenuation at 80 Hz, ≥12 dB at
  150 Hz, ≥20 dB at 6 kHz, ≤1 dB loss at 800/1,200/1,500/1,800 Hz.
- Switch styles during each call role without capture stalls/timing drift;
  verify that speaker audio, Opus configuration and microphone gain do not
  change. Confirm cached choice on new call/reboot and server override.
- Record/listen to the same speech at Pion in both modes on hardware; explicitly
  assess the RNNoise difference rather than calling filter selection full
  acoustic parity by itself.

### Silent Mode

- Incoming silent call: no audible ringer, visual indication retained, ordinary
  answer allowed and voicemail timer still armed. Established media is unchanged.
- Enable during ringing: stop only ring audio and preserve indication/call.
  Disable during that ring: no retroactive bell restart; next call rings.
- Verify manual diagnostic Ring Test behavior and speaker ownership separately.
  Reboot/cache, offline edit/reconnect and two devices on one line must match
  authoritative settings, using current controls without new TH-07 wiring.

### Quiet Hours

- Test start-inclusive/end-exclusive same-day and overnight windows, opening-day
  weekday semantics, Sunday indexing, explicit silence OR schedule, invalid/equal
  times and enabled/no-days normalization to all days.
- Test household timezone changes and Europe/Berlin spring-forward/fall-back
  instants on the server. Verify device transition within normal scheduler
  cadence; no ESP32 timezone assumption or local clock dependency.
- Cross a boundary offline: retained cached bit is baseline behavior; reconnect
  must converge to current effective state. Test reboot inside/outside a window.
- Save unrelated settings inside Quiet Hours; run Silence All-off, effective
  lookup failure, queue-full and multi-replica delivery cases. Keep the known
  server defects visible until corrected, rather than passing only the normal
  path or expecting ESP32 to reconstruct missing schedules.

### Answering Machine

- Enable from a disabled boot without restart; test 5/20/60-second settings,
  offline changes, missing optional object and explicit disable. Apply changes
  to subsequent rings without stale timers answering canceled calls.
- Both normal and silent/quiet calls use ordinary answer/ICE after the delay;
  caller hears greeting/beep, local speaker stays quiet, and **no room microphone
  audio** is transmitted during unattended lead-in, greeting gaps or recording.
- Hangup/pickup just before timeout, during greeting, while recording and during
  media failure: no stale answer, no empty message, partial pickup continues
  the call, and all audio/recorder resources release.
- Replay real stored caller audio intelligibly. Test 30,000-frame cap and its
  difference from wall time under loss, 51st-message eviction, storage-full/
  write failures, interrupted finalization, reboot recovery and durable greeting.
- Verify equivalent `*96`/`*97`/`*98`/`*99` and 7/9/#/* retrieval behavior through
  agreed local controls, mark-heard at EOF, disable retaining messages, and
  voicemail indicator. Do not claim physical hook/keypad parity before that work.
- Count changes including zero must reach the WebUI; unchanged nonzero count
  must republish on reconnect. Test multi-device summation and cross-replica UI
  separately. No server-side recording should appear for an offline handset.

### Remote Reboot

- Valid household-admin command reboots only selected hardware; unauthorized,
  invalid mode and foreign-device requests produce existing errors. Exercise
  offline enqueue/publication and repeated commands without assuming delivery.
- Use existing `rebooting`/`restarting` statuses as appropriate; do not invent
  an `accepted` acknowledgement. Confirm defined busy-call policy and completed
  audio/storage cleanup before reset; no pairing/config loss.
- Confirm actual reset and same-identity reconnect, then fresh settings and
  version reporting. Test UI missing a short offline interval and explicit
  `service` behavior rather than relying only on the presence spinner.

### Manual and Automatic Updates

- Correct ESP32 board/version image succeeds; reject Pico ELF, Pi binary,
  wrong chip/board, oversized/truncated image, bad hash/authenticity proof and
  untrusted URL/certificate. No flash operation may start from legacy artifact
  metadata misclassified as ESP32.
- Stream within bounded memory; exercise timeouts, retry/resume policy,
  download interruption, flash-write interruption and power loss around boot
  selection. Verify inactive-slot use, preserved pairing/mailbox and rollback
  when boot health fails. Confirm healthy boot before claiming committed success.
- Test manual/automatic maintenance against call setup, ringing, connected
  duplex, voicemail, Ring Test, recording and cleanup. Prevent calls racing
  destructive commit; reject/coalesce concurrent updates and reboot safely.
- Default automatic policy false; omitted false clears true; disable cancels
  pending work. Cached true must not outrun fresh false on reconnect/startup.
  Already-enabled reconnect must catch a missed release. Test bounded retries
  and prevent automatic downgrade even with stale notifications.
- Compare HTTP enqueue, progress, running version and rollback/health outcome.
  An old/incorrect `device_info` must not be treated as verified desired-version
  success. Test the existing server/UI heuristic separately until corrected.

## Existing source tests and validation performed here

The baseline already contains relevant tests; they establish intended coverage,
not a claim that they were executed in this analysis:

| Area | Source tests |
| --- | --- |
| Settings/permissions/push | [web handlers](../server/internal/web/handler_test.go), [settings handlers](../server/internal/web/handler_settings_test.go), [registration snapshots](../server/internal/signaling/relay_linesettings_test.go), [Pi dispatch](../pi/digitsd/cmd/digitsd/dispatch_test.go) |
| Voice processing | [filter response](../pi/digitsd/internal/audio/filters_test.go), [live switching](../pi/digitsd/internal/audio/pipeline_live_test.go) |
| Silence/schedules | [Pi controller](../pi/digitsd/internal/phone/controller_test.go), [schedule predicates](../server/internal/line/quiethours_test.go), [scheduler](../server/internal/signaling/quiethours_test.go), [Silent Mode browser tests](../server/e2e/tests/09-silent-mode.spec.ts) |
| Voicemail | [server controls](../server/internal/web/handler_voicemail_test.go), [store](../pi/digitsd/internal/voicemail/store_test.go), [greeting](../pi/digitsd/internal/voicemail/greeting_test.go), [daemon](../pi/digitsd/cmd/digitsd/voicemail_test.go), [answer flow](../pi/digitsd/cmd/digitsd/voicemail_answer_test.go) |
| Release/download/status | [GitHub index](../server/internal/updates/github_test.go), [updater](../pi/digitsd/internal/updater/updater_test.go), [Redis state](../server/internal/signaling/state_redis_test.go), [postflash verification](../pi/digitsd/cmd/digitsd/postflash_verify_test.go), [resynchronization](../pi/digitsd/cmd/digitsd/postflash_resync_test.go), [flash script](../pi/image/tests/flash-pico-test.sh) |

This phase inspected sources, cross-checked the feature findings, and checked
document links/JSON examples and the final diff. No feature tests, builds,
downloads, device commands, firmware changes or deployment changes were run.
