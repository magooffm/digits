# Digits ESP32 signaling, audio and WebRTC

ESP-IDF **v5.5.3**, target **esp32s3**, Waveshare ESP32-S3-AUDIO-Board
(ESP32-S3R8, 16 MB flash). This firmware connects Wi-Fi, registers with
Digits, pairs, reconnects, plays the existing device Ring Test through
the integrated speaker, and captures the two onboard microphones through
the ES7210 ADC. Capture supplies reusable 48 kHz signed 16-bit mono PCM;
an optional development diagnostic reports levels over serial. Optional saved
audio tests either record two seconds after boot or record while KEY1 is held
(up to ten seconds), then play the recording after release.
WebRTC supports one incoming or outgoing call with serial development controls,
live microphone Opus encoding and decoded speaker playback. Full duplex is the
default; transmit-only and receive-only modes support separate hardware checks.
Full-duplex tests confirmed clear Pion-to-ESP32 speech while capture continued.
Recorded ESP32 microphone words were also confirmed clear after offline
gain/filter preparation and replay through Pion. Initial repeat cycles and
post-call diagnostic recovery passed. A clean acoustic-loop probe estimated
about 170 ms round trip; occasional playback delays and calibrated one-way
latency remain under assessment. The
nine-call baseline established encoded transport, and the earlier audible
Ring Tests checked the local speaker.
See the [call audio implementation and validation guide](../docs/esp32-call-audio.md).
Phase 1 now consumes and caches server line settings, adds incoming-call speaker
ringing, and applies effective Silent Mode/Quiet Hours suppression. See the
[settings and ringing acceptance guide](../docs/esp32-settings-ringing.md) for
cache semantics, tests and pending physical acceptance. Voice Style, voicemail
and Auto Update preferences are cached only in this phase.
Hook switch and call buttons work through the board keys or GPIOs; see
[Phone controls](#phone-controls). Handset wiring remains unused.
The console defaults to the ESP32-S3's native USB Serial/JTAG port.

## Configure, build, flash and monitor

With ESP-IDF v5.5.3 installed:

```sh
. /path/to/esp-idf/export.sh
cd esp32
idf.py set-target esp32s3
idf.py menuconfig
# Digits development configuration:
#   Wi-Fi SSID / password and server URL: optional fallbacks, see "Setup network and page"
#   Setup network password: 8-63 characters
#   (server URL like ws://192.168.1.50:8080/ws)
idf.py build
idf.py -p /dev/cu.usbmodemYOUR_DEVICE flash monitor
```

Exit the monitor with Ctrl+]. Linux ports are usually `/dev/ttyACM0`.
If using a USB-UART bridge instead of native USB, select UART under
Component config -> ESP System Settings -> Channel for console output.
URLs other than `ws://.../ws` fail fast.

### Version

The firmware version comes from git tags named `esp32/vX.Y.Z` (other tags
such as upstream's `server/v...` are ignored):

| Build from | Version |
| --- | --- |
| the tagged commit | `esp32-0.4.0` |
| three commits later | `esp32-0.4.0-3-gabc1234` |
| with uncommitted changes | `...-dirty` |
| before the first `esp32/v` tag | `esp32-0.0.0-gabc1234` |

The `esp32-` prefix keeps it apart from Pi versions in the web UI.

```sh
git tag esp32/v0.1.0   # then build; the version is read when CMake configures
```

CMake reconfigures on commits, checkouts and new tags; after editing without
committing, `-dirty` appears on the next configure. The version is in the
boot log ("App version"), on the setup page, and after every connection the
phone reports it to the server as `device_info` (with commit and local IP),
where the web UI shows it as the software version. The Pi/Pico update
buttons and the automatic-updates setting do nothing on the ESP32: the
server only knows Pi and Pico releases. Firmware is updated over USB or on
the setup page.

### Setup network and page

Wi-Fi network and Digits server are saved on the phone (NVS namespace
`digits_wifi`) and survive flashing. The menuconfig SSID, password and
server URL are only fallbacks until something is saved; they may stay empty.

The phone opens its setup network `Digits-Setup-XXXX` (the last digits of
its MAC address) only when needed:

- no network or server configured (a fresh phone),
- not paired yet: the page shows the pairing code,
- 2 minutes without reaching the Wi-Fi or the server (meanwhile it retries
  every 30 seconds and closes the setup network once both work again, so a
  router that boots slower than the phone after a power cut needs nothing),
- on request, for 30 minutes: hold call button A or B for 8 seconds with the
  handset down (a short ring confirms), press **Recovery mode** on the
  phone's page in the web UI (nothing is erased; the web UI calls the
  network "Digits-Recovery"), or type `wifi setup`.

**Password.** The setup network uses WPA2 with the built-in **Setup network
password** (menuconfig, 8-63 characters), the same on every phone you build.
That password is the login: the page answers only on the setup network,
never on the home network, and WPA2 also encrypts the home Wi-Fi password
while you type it. Without a password the network is open and the log warns.

**The page.** Joining the network opens it by itself, as on hotel Wi-Fi;
otherwise go to `http://192.168.4.1`.

- **Status.** Wi-Fi, server and number, refreshed every few seconds.
- **Pairing.** While unpaired, the server's 6-digit code. Note it, switch
  back to the home Wi-Fi, open the server's Phones page (the link is shown),
  enter the code and choose the number. The server has no automatic number
  assignment: the number is always chosen there.
- **Wi-Fi and server.** Network (nearby ones are suggested), password (empty
  keeps the saved one of the same network) and `ws://HOST:PORT/ws`. Saving
  restarts the phone.
- **Update firmware.** Upload `build/digits_signaling.bin`. It goes to the
  idle firmware slot, is checked (image checksum and project name) and the
  phone restarts into it. The new firmware confirms itself once the server
  answers, or after 5 minutes; if it restarts before that, the bootloader
  goes back to the previous one.
- **Restart.**

```text
wifi                         # network, server, setup network state
wifi set "My Home" password  # save and restart; omit the password for an open network
wifi server ws://192.168.1.50:8080/ws
wifi forget                  # back to the build defaults
wifi setup                   # open the setup network for 30 minutes
wifi setup lan               # same, page also on the home network until restart (testing)
wifi close
```

The page uses ESP-IDF's HTTP server and the DNS server from ESP-IDF's
captive portal example, copied unchanged to `components/dns_server` (public
domain). ESP-IDF's own `wifi_provisioning` was not used: it needs
Espressif's phone app instead of a web page. Form parsing, address checks and
HTML escaping are in `wifi_setup_form.c`, tested by
`tests/wifi_setup_form_test.c`; the page is `main/setup_page.html`.

Verified on the board on 2026-10-28 (page fetched over the LAN with `wifi
setup lan`): the new partition layout kept pairing, the setup network stays
closed in normal operation, status and network list, invalid forms and junk
uploads refused, a 1.9 MB firmware uploaded in 7 s, booted from the other
slot and confirmed itself once the server answered, and without `lan` the
page refuses the home network. Earlier: opening after 120 s offline, saving
and `wifi forget`. Not yet verified: the page opening by itself on a phone
that joins the setup network, the 8-second button hold, the web UI's
Recovery button, a rollback after a failing update, and pairing a fresh
phone through the page.

Wi-Fi scans all channels and joins the strongest access point of the
configured name. With the default fast scan, a mesh network sometimes handed
the board a -91 dBm access point where DHCP took a minute or more. Once
online, the clock is set from **Time server** (`pool.ntp.org`) in **Time
zone** (Central European by default), so files on the SD card get real dates.

Wi-Fi credentials and server URL are build configuration, not source constants.
`sdkconfig`, `sdkconfig.old` and `build/` are ignored because they contain the
configured values. Credentials are embedded in the development firmware;
do not distribute a configured binary or generated configuration containing
your password. No actual network credentials are supplied by this project.

For the temporary toolchain used during development in this workspace:

```sh
export IDF_TOOLS_PATH=/private/tmp/digits-idf-tools
. /private/tmp/digits-esp-idf/export.sh
```

### Shortcuts

`esp32/Makefile` wraps `idf.py`. It expects ESP-IDF at `~/esp/esp-idf-v5.5.3`
and the board at `/dev/ttyACM0`; override with `IDF_PATH=` or `PORT=`.

```sh
make -C esp32 deploy      # build, flash, open the console (Ctrl+] exits)
make -C esp32 build       # or: flash, monitor, menuconfig, size
make -C esp32 test        # every host test, normal and sanitized
```

In VS Code, Ctrl+Shift+B runs `deploy`. The other targets are under
Terminal > Run Task as "ESP32: ...". Flashing keeps NVS.

## Phone controls

`phone.c` maps the hook switch and two call buttons onto the existing WebRTC
commands. Rules follow the Pi controller:

- Lifting the handset while a call rings answers it.
- Replacing the handset ends any call, outgoing or connected.
- With the handset lifted and dial tone playing, button A or B calls its number.
- Buttons pressed on-hook, during a call, or after a call ended (busy or
  reorder) are logged and ignored. Replace the handset first, as on the Pi.
- The incoming ringtone is silent while the handset is lifted. If a call
  arrives off-hook, replacing the handset starts ringing; it is not rejected.

### Call-progress tones

With the handset lifted, the speaker plays the Pi's tones (`pi/tones`),
generated in `tone_gen.c` instead of stored as WAV files:

| When | Tone |
| --- | --- |
| Handset lifted, no call | Dial, 350+440 Hz continuous |
| Calling out until answered | Ringback, 440+480 Hz, 2 s on / 4 s off |
| Busy, or the far end left while ringing | Busy, 480+620 Hz, 0.5 s on / 0.5 s off |
| Unreachable, failed, or the other side hung up | 1 s silence, then reorder, 480+620 Hz, 0.25 s on / 0.25 s off |
| Dial tone unused for 15 s | Same reorder treatment |
| 45 s of reorder | Silence until the handset is replaced |

The Pi's spoken announcements and its howler tone are left out: there are no
recordings on the ESP32, and the howler is too loud for the board speaker.
`call_tones.c` owns the speaker only while a tone plays. Call audio waits
until it has released the speaker, so ringback hands over cleanly when the
call connects. Tests: `tests/tone_gen_test.c`, `tests/call_tones_test.py`,
and tone cases in `tests/phone_fsm_test.c` and `tests/webrtc_interop_test.py`.

Verified on the board on 2026-10-24: dial tone on lift, ringback on a button,
reorder after "phone not connected", ignored button after a failed call, and
reorder after 15 s of dial tone. Not yet verified: the audible tones, busy
tone from a busy peer, and the ringback-to-call-audio hand-off on an answered
call.

### Caller-chosen ringtones

The caller decides how the other phone rings. Each call button stores a
ringtone next to its number:

```text
phone ringtones          # list: 0 standard ring, 1-4 melodies
phone ringtone a 2       # calls from button A ring the other phone with Ode to Joy
phone preview 2          # hear it on this phone (handset down, no call)
```

The choice travels as one session-level line in the SDP offer,
`a=x-digits-ringtone:N`. The unchanged server relays the offer as-is; Pi
phones and other clients ignore the line. The callee removes it before the
offer reaches the peer library, waits up to 500 ms after `ring` for the
offer, then rings with that ringtone, or with the standard ring if none
arrives. A call from the console (`webrtc call`) adds nothing.

The melodies are public-domain tunes synthesized in `ringtones.c` with a
music-box timbre. One cycle is the melody plus a 2 s pause. Songs are defined
in `ringtone_songs.h` as note strings; the index is the wire value, so append
new songs and never reorder them. The Pion helper reads the same table and a
test fails if the two drift apart. Tests: `tests/ringtones_test.c`, melody
cases in `tests/incoming_ring_test.py` and `tests/call_tones_test.py`, and
offer handling in `tests/webrtc_interop_test.py`.

**Own recordings and songs.** Files in `esp32/ringtones/` named
`NN-Name.ext` (NN 10-99, any format ffmpeg reads) are built into the firmware
and selectable like the melodies, for example `phone ringtone a 12`. The
build converts them to 24 kHz mono at the ringer level, trims silence, cuts
at 30 s, and packs them into the 8 MB `ringtones` partition, flashed with the
app (`make -C esp32 flash`); NVS is not touched. The folder is ignored by git
except its README, which has the details. Flash every phone from the same
folder: a phone without the requested clip rings with the standard ring.
Numbers 0-9 stay reserved for built-in melodies, so adding songs never
renumbers anything. Tests: `tests/pack_ringtones_test.py` and clip cases in
`tests/ringtones_test.c`.

Verified on the board on 2026-10-24: listing, setting, rejecting an invalid
number and saving per button; a test clip was packed, listed, previewed and
removed again. Not yet verified: how the melodies sound, and a
call that rings the other phone with the chosen melody.

### Ringtone selectors on this phone

The other way around: each call button has a selector, a rotary switch or
fader, that decides how calls *from* that button's number ring on this phone.
All selectors share one position map, and every position is a ringtone.
Callers that are on no button keep their own choice, and a selected clip
this phone lacks falls back to it too.

```text
phone positions          # the map: positions 1-12 and their ringtones
phone position 3 12      # position 3 rings with clip 12
phone selector a 3       # no switch fitted yet: set selector A from the console
phone status             # per button: outgoing ringtone and how it rings here
```

Defaults: positions take the built-in ringtones and clips in order; unused
positions ring standard; console selectors start at position 1. Positions,
the map and console selector positions are saved in NVS. A map saved by
older firmware, whose last position meant "caller's choice", rings standard
there until you set it with `phone position`. Under **Phone controls**, set the number of positions
(2-12, matching the switch's end stop) and an analog pin per selector.

Suggested hardware (in Germany): a 1-pole 12-way rotary switch per call button,
TRU Components SR26A4-1X12-18F20 (Conrad 2353898, 6 mm knurled shaft; check
the datasheet) with a PB Fastener 3030420 knob, on a resistor ladder: eleven
equal resistors (1 kOhm) in a chain between 3.3 V and GND, each switch contact
on one tap, the wiper to the analog pin with 100 nF to GND. A 10 kOhm linear
fader or rotary pot works without the ladder but has no detents; readings need
20% of a step past a border to change, so neither flickers. Use ADC1 pins
(GPIO1-9; ADC2 does not work while Wi-Fi runs). Avoid GPIO3 (strapping),
GPIO10 (I2C clock) and, in GPIO input mode, GPIO4-6 (hook, A, B): for example
GPIO7, 8 and 9.

Button C exists only in GPIO mode (default GPIO39); board-key mode keeps A
and B. Logic: `ring_select.c`, tested by `tests/ring_select_test.c`; the
WebRTC worker asks the phone through a resolver hook when ringing starts.

Verified on the board on 2026-10-24: map listing, editing, console
selectors and status (when the last position was still reserved for the
caller's choice; since 2026-10-26 it is a ringtone like the others). GPIO mode with selectors on GPIO7-9
compiles. Not yet verified: physical switches, and an incoming call ringing
with the selector's choice.

### Ringtones from the SD card

Put MP3 files in a `ringtones` folder on a FAT32 microSD card in the board's
slot, named after the selector position they take over. `ringtone-03.mp3`
then plays whenever a selector stands on position 3, instead of that
position's mapped ringtone. Positions without a file keep their map entry.
The files stay on this phone: callers never see or choose them.

```text
ringtones/
  ringtone-03.mp3
  ringtone-07.mp3
```

- **Names.** `ringtone-NN.mp3`, NN from 01 to the number of selector
  positions; `ringtone-3.mp3` and any letter case work too. Other files are
  logged and ignored.
- **Streaming.** Files play straight from the card, whole songs of any
  length: a decoder task fills a one-second buffer while ringing, so memory
  use does not depend on the file. Each ring cycle plays the song, pauses
  two seconds and starts again. The card is mounted at boot and read for
  file names and lengths (from the MP3 headers); restart the phone after
  changing files.
- **Quality.** Mono (the board has one speaker), leading silence skipped.
  48 kHz files need no resampling; other rates go through
  Espressif's high-quality resampler (`esp_audio_effects`, complexity 3)
  instead of the linear interpolation packed clips use. Songs keep their
  dynamics: **SD card ringtone level** scales them by a fixed percentage
  (12 by default, which puts a full-scale master at the standard ring's
  peak). For the best result, store 48 kHz MP3s at 192 kbps or more.
- **Failures.** A file that cannot be decoded when it should ring falls back
  to the standard ring for that call and to the mapped ringtone afterwards.
- **Console.** `phone ringtones` shows the card status and the files as
  numbers 101-112; `phone positions` and `phone status` show which positions
  a file takes over; `phone preview 103` plays one. They cannot be sent as an
  outgoing ringtone: `phone ringtone a 103` is refused.
- **Setup.** On by default under **Phone controls → Ringtones from the SD
  card**. The card uses GPIO40 (CLK), GPIO42 (CMD) and GPIO41 (D0) in 1-bit
  mode, as in Waveshare's demos; keep buttons off those pins. Long file
  names are enabled in `sdkconfig.defaults`; an older `sdkconfig` needs
  **Component config → FAT Filesystem support → Long filename support** set
  to the heap option.

Verified on the board on 2026-10-26: boot with and without a card, five
48 kHz files found with their lengths, previews streaming with no buffer
underruns (heard through the board microphone), console output and the
refused outgoing number. Not yet verified: an incoming call ringing with an
SD file, and resampling of non-48 kHz files.

### Silence

Incoming calls ring unless the line is silent. The server decides that from
the web toggle and quiet hours; the phone only receives the result. A local
choice (the planned silence button, for now the console) overrides it until
the server's value next changes, so the most recent change wins:

- Silenced here, then quiet hours end: calls ring again.
- Quiet hours, then set to ring here: calls ring until quiet hours end.

The choice is kept in NVS across restarts. The server cannot be told about
it: the wire protocol has no message for that, so the web page keeps
showing the server's own setting.

```text
phone silence on|off|toggle   # what the silence button will do
phone status                  # "Incoming calls: silent (server says ring, overridden here)"
```

The rule is `silence_policy.h`, tested by `tests/silence_policy_test.c`.

### Answering machine

With voicemail on for the line (web UI, per line, with a 5-60 second ring
timeout), an unanswered call is picked up after that timeout, as on the Pi.
The caller hears the greeting and a 500 ms, 1 kHz beep; their audio is then
stored on the SD card. It needs a card; without one the phone keeps ringing.

- **Privacy.** From the moment it answers, the room microphone is never
  sent: every transmitted frame comes from the greeting, then silence. The
  Pi only mutes after the beep. Nothing is played on the speaker meanwhile.
- **Pickup.** Lifting the handset keeps what was recorded and continues as a
  normal call. Replacing it hangs up.
- **Files.** `voicemail/new/000012-1002.opus` (number, caller) holds unheard
  messages, `voicemail/saved/` heard ones. They are standard Ogg Opus files
  with the caller's original packets, so any player opens them on a
  computer; the clock gives them real dates. Up to 50 messages of up to
  10 minutes each, as on the Pi (**Answering machine** in menuconfig); a new
  one beyond that removes the oldest heard message first. Messages under half
  a second (a hang-up at the beep) are discarded.
- **Greeting.** The Pi's default greeting is built in. Put your own as
  `voicemail/greeting.mp3` on the card (any MP3; it streams like the SD
  ringtones and is sent at full level, see **SD card greeting level**).
- **Badge.** The number of unheard messages goes to the server on every
  connection and whenever it changes, so the web page shows it.

```text
vm                # status: card, counts, greeting
vm list           # number, new, caller, date, length
vm play           # all new messages, oldest first, then marks them heard
vm play 12        # one message
vm greeting       # hear the greeting
vm stop           # stop playing; lifting the handset also stops it
vm heard 12 | vm delete 12 | vm delete heard
```

Playback uses the speaker while the line is idle; an incoming call stops it.
Physical buttons for it are not assigned yet.

Verified on the board on 2026-10-27 with the PC helper as caller: answered
after the server's 20 s, the caller's recording held the greeting, the beep
and then digital silence (peak 4 of 32767), an 11.8 s message was stored,
listed with its date, played back and marked heard, and the count went to
the server as 1 and then 0. Lifting the handset during a message kept the
3.9 s already recorded and switched to a normal call; replacing it hung up.
A local silence choice survived a restart. Host tests: `ogg_opus_test.c`
(against an ffmpeg file; written files pass `opusinfo`),
`voicemail_store_test.c`, `voicemail_prompt_test.c`, and answering-machine
cases in `call_audio_worker_test.py` and `webrtc_interop_test.py`. Not yet
verified: a custom `greeting.mp3`, a full card, the 10-minute limit.

### Inputs

Inputs are chosen under **Digits development configuration → Phone controls**:

| Mode | Hook | Call A | Call B |
| --- | --- | --- | --- |
| Board keys (default) | KEY1, each press lifts or replaces | KEY2 | KEY3 |
| Direct GPIO | level switch, default GPIO4, low = lifted | GPIO5 to ground | GPIO6 to ground |

Board keys are TCA9555 P11-P13. KEY1 doubles as the hold-to-record key, so the
board-key mode is unavailable while that diagnostic is selected.

Each phone calls different people, so button numbers are stored per device
in NVS rather than in the build. They survive flashing:

```text
phone set a 3140002     # save the number for button A
phone clear a           # fall back to the menuconfig default (empty)
phone status            # handset, call phase, configured numbers
phone hook up|down      # simulate the handset
phone press a|b         # simulate a call button
```

The logic is a pure state machine in `phone_fsm.c`, tested on the host by
`tests/phone_fsm_test.c`. `tests/incoming_ring_test.py` covers ringtone
suppression races.

For a two-way test against a second phone on the PC, see the helper's
[live two-way call](../tools/interop/README.md#live-two-way-call).

Verified on the board on 2026-10-23: idle on-hook presses are ignored, lift
plus button A sends `call` and creates a peer, and replacing the handset
hangs up and releases it. Not yet verified: physical KEY1-KEY3 presses, an
incoming call answered by lifting, and silence while off-hook.

## Persistent storage

Default NVS partition, namespace `digits`:

| Key | NVS type | Schema |
| --- | --- | --- |
| `hardware_id` | string | Lowercase UUID v4, 36 characters plus NUL. Generated with `esp_fill_random` after Wi-Fi starts, committed before registration. |
| `pairing` | blob | 98 bytes: version byte `1`, 32-byte NUL-terminated number, 65-byte NUL-terminated original token. Both strings are saved as a single NVS value, then committed. |

Line settings use a separate namespace `digits_settings`, key `snapshot`, with
a versioned 12-byte blob. Its asynchronous worker never erases or modifies
pairing. See the [cache contract](../docs/esp32-settings-ringing.md#exact-settings-and-cache-behavior).

Call button numbers use namespace `digits_phone`, string keys `call_a` and
`call_b` (digits only, at most 31). A missing key means the build default.

The token is never hashed by the device or printed in application logs.
Missing `pairing` means unpaired. Existing invalid identity/record or NVS
initialization errors stop startup instead of silently discarding identity.
The token capacity covers v1.99.0's 64-character hex token; number capacity
is 31 characters. Overlong or empty incoming credentials are rejected.
On commit failure, automatic signaling retries stop and no deliberate
credential reconnect occurs. Investigate storage and reboot; never erase
NVS as routine recovery for a paired device.

Regular `idf.py flash` preserves NVS. `erase-flash` loses identity and pairing.
Pairing is committed server-side before `paired` is sent: losing that message
or erasing the saved token can require administrator intervention to unpair
the old device. Wi-Fi driver storage is RAM; development Wi-Fi settings come
from the current firmware configuration.

## Wire contract

Verified against upstream `server/v1.99.0`, commit
`a4edaaa557c06b3b687be9e92a869fb0cbcf68fc`.

Unpaired registration (exact generated shape, example UUID):

```json
{"type":"register","number":"unpaired","hardware_id":"12345678-1234-4234-8234-123456789abc"}
```

Paired registration (example number and 64-character original token):

```json
{"type":"register","number":"3140001","hardware_id":"12345678-1234-4234-8234-123456789abc","device_token":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}
```

The synchronous WebSocket CONNECTED handler sends `register` immediately
with a 3-second send timeout, before allowing queued peer signaling. No
register-success acknowledgement exists, so a successful send log does not
claim authentication success. Peer commands require stored pairing credentials.

`pairing_code` is retained as a JSON string, preserving leading zeros.
`paired` commits number and token before requesting a reconnect outside the
WebSocket event callback. `line_renumber` commits the changed number while
retaining the token; an unchanged number does not cause a reconnect loop.
`error` logs the server's exact error string without changing credentials.
`ring_test` queues a one-second speaker test without replying to the server.
The peer dispatcher handles one-to-one call, SDP, ICE and ICE-server messages.
Other message types are logged and ignored.

Espressif `esp_websocket_client` **1.6.1** is pinned in the manifest. Its receive
path automatically echoes incoming PING payloads as PONG control frames.
Application code must not send JSON pong messages or duplicate that reply.
The 1024-byte receive buffer exceeds the RFC maximum control payload of 125
bytes. Outbound heartbeat uses 30-second pings and a 15-second pong timeout.
Server heartbeats use 30-second pings and a 45-second read deadline.
Text messages are assembled across receive-buffer chunks and continuation
frames up to 16,384 bytes; control frames can interrupt assembly. Raw SDP is
separately limited to 12,288 bytes. Peer output uses an eight-item queue tagged
with the WebSocket session; old work cannot be replayed after reconnect.

Deliberate credential reconnects are immediate. Failed/short-lived connections
use delays of 6, 12, 24, 48, then 60 seconds maximum. A connection surviving
45 seconds resets the backoff and gets an immediate reconnect after a drop.
Backoff is not reset merely by TCP/WebSocket upgrade, avoiding a rapid loop
when the server rejects a token. Wi-Fi retries every three seconds; signaling
waits for the station to obtain an IP before opening a new connection.
The BEGIN callback waits until `start()` returns, avoiding the pinned client's
startup STOPPED-bit race on immediately failing connections. Stop/destroy run
only in the owner loop, never in the WebSocket callback.

Reference definitions:

- `server/internal/signaling/protocol.go`: `Message`, message type constants.
- `server/internal/web/handler_ws.go`: `handleWS`, `wsReject`, heartbeat constants.
- `server/internal/pairing/pairing.go`: `GenerateCode`, `ClaimDevice`, `ClaimDeviceToLine`.
- `server/internal/device/store.go`: `AuthStatus`, `BoundLineNumber`.
- `server/internal/web/handler_phones.go`: `handlePhonesPairPost`, `handlePhoneNumberPost`.
- `pi/digitsd/internal/signal/client.go`: `Client.Connect`.
- `pi/digitsd/cmd/digitsd/dispatch.go`: pairing and renumber handling.
- `pi/digitsd/cmd/digitsd/main.go`: `reconnectLoop`, `nextReconnectBackoff`.
- Pinned client implementation: https://github.com/espressif/esp-protocols/blob/websocket-v1.6.1/components/esp_websocket_client/esp_websocket_client.c

## WebRTC interoperability milestone

The [integration design](../docs/esp32-webrtc-interoperability.md) records the
exact upstream protocol and compatibility review completed before coding.
The implementation uses `esp_peer` directly; it does not initialize Espressif's
capture/render framework or reopen the existing I²S/I²C peripherals.

The manifest and lock file pin:

| Component | Version | Purpose |
| --- | --- | --- |
| ESP-IDF | 5.5.3 | Target SDK; includes mbedTLS 3.6.5 |
| `espressif/esp_peer` | 1.5.7 | ICE, DTLS and encoded RTP peer API |
| `espressif/esp_libsrtp` | 1.0.0 | Bundled libSRTP 3.0.0 |
| `espressif/esp_websocket_client` | 1.6.1 | Existing Digits transport |
| `espressif/esp_codec_dev` | 1.6.2 | Existing ES8311/ES7210 drivers |
| `espressif/esp_audio_codec` | 2.6.2 | Direct Opus encode/decode APIs |

The initial transport milestone sent validated `f8 ff fe` Opus silence and
counted received packets. The call audio implementation replaces that source
with ES7210 PCM: 960 signed 16-bit mono samples at 48 kHz per 20 ms frame.
Dedicated workers encode Opus and decode incoming audio for ES8311 playback;
the peer worker alone submits encoded packets. Capture sequence determines
PTS, and stale/backlogged frames are discarded without a catch-up burst.
The SDP still advertises `opus/48000/2`; local PCM remains mono.

The ESP32 encoder uses 32 kbit/s CBR, VOIP mode, complexity 0 and no FEC/DTX.
The original Pi/Pion 24 kbit/s FEC/DTX encoder remains unchanged. Playback uses
a bounded 60 ms initial jitter target, eight slots and half-amplitude PCM.
Missing frames use Opus PLC; a sustained outage mutes output and waits for a
new playout window. These implementation settings do not establish measured
latency or speech intelligibility; use the [three-phase audio checks](../docs/esp32-call-audio.md#physical-validation-procedure).

Under **Digits development configuration → WebRTC interoperability**, enable
the peer and USB console. Auto-answer is disabled by default. The commands in
`idf.py monitor` are:

```text
webrtc status
webrtc call 3140002
webrtc answer
webrtc hangup
webrtc audio tx
webrtc audio rx
webrtc audio full
```

Audio mode applies only while idle after cleanup; full duplex is the default.
The mode command also works while signaling is offline. Calls still require
online pairing. Audio startup is asynchronous after `CONNECTED`, and hangup
waits to free its context until both codec workers stop. New calls and mode
changes are rejected while `cleanup_pending=yes`.

Use an actual online Pion device number in the same authorized Digits family.
No calls are made automatically at boot. An incoming `ring` retains the caller
and offer until `webrtc answer`; answering before the offer arrives records
the intent. Outgoing order is `call`, then `sdp` containing the offer; incoming
acceptance sends `answer` containing the answer. SDP remains a JSON string with
its original CRLF. Pion's `ice.candidate` is a bare `candidate:...` string, with
no JSON candidate object, MID, m-line index or added `a=` prefix. Espressif's
local candidates are included in its SDP; any early remote trickle candidates
are buffered until remote SDP is applied. The local ringing bound is 60 seconds;
the post-answer transport deadline is 10 seconds, matching the Pi client.

Each peer, retained SDP, candidate list and per-call configuration is released
on hangup, failed setup, timeout or signaling loss. Signaling reconnects preserve
pairing and start without a peer. Foreign-peer, conference and stale-session
messages cannot start a second peer. The server's ordinary `hangup` ignores
`to` and ends the authenticated origin's active calls, so unrelated rings or
conference messages must not cause a speculative hangup.

### Configuration and memory

New configurations use the board's Octal PSRAM at 40 MHz through `malloc`,
a 4,096-byte internal-allocation threshold, a 65,536-byte internal reserve,
and the supported 240 MHz CPU setting. DTLS and DTLS-SRTP are enabled. Existing
`sdkconfig` values take precedence over defaults: when updating an older
checkout, select these settings in menuconfig rather than assuming the new
defaults replace existing selections. In particular select **Custom partition
table CSV → partitions.csv**, **Octal PSRAM → 40 MHz → malloc integration**,
and **CPU frequency → 240 MHz**. Call audio also needs **FreeRTOS tick rate →
1,000 Hz**; older configurations may retain 100 Hz despite the new default.
These changes leave Wi-Fi/server settings alone.

NVS remains at `0x9000`, size `0x6000`, and PHY at `0xf000`, size `0x1000`.
Since 2026-10-28 two 3 MiB firmware slots (`ota_0` at `0x20000`, `ota_1` at
`0x320000`, with `otadata` at `0x10000`) replace the single 4 MiB factory app,
for updates from the setup page with rollback; the ringtone pack partition
shrank to the remaining 9.9 MiB (SD card ringtones are the main way now).
Regular flashing writes the partition table and app without erasing the
saved identity/token; the change to this layout needs one USB flash.

The peer worker uses a 20 KiB internal stack on core 1 at priority 6, above
microphone capture and codec workers (5) and Ring Test/recorded playback (4).
This priority was raised from 3 after the first full-duplex continuity failure;
the corrected build completed clear full-duplex calls with both endpoints as
caller and four additional cleanup cycles. Its inbound queue
holds eight copied events; limits are 32 remote candidates of up to 512 bytes,
eight STUN URLs of up to 255 bytes, 12 KiB raw SDP and 16 KiB signaling JSON.
The peer's transmit pool and audio jitter buffer are explicitly 8 KiB each,
with 16 transmit entries and eight gathered candidates. The development console
adds a 6 KiB stack (4 KiB before `vm list` read files) at priority 3 and
eight RAM-only history entries. Queue
exhaustion aborts affected setup rather than silently losing control messages.
Per-call configuration, URLs and applied SDP remain valid until peer closure.
The call encoder uses a 36 KiB internal stack and the decoder uses 16 KiB,
both at priority 5 (48/24 KiB until 2026-10-27; at those sizes 27,136 and
16,012 bytes stayed unused, and once more features ran, the internal heap
had no 48 KiB block left after peer creation). Tasks that never write flash
(SD streamer, answering-machine recorder and player, microphone level log)
keep their stacks in PSRAM, and so do mbedTLS's DTLS buffers
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, 2026-10-28). Measured over two 60 s calls
each: free internal RAM during a call 27-29 KB before, 54 KB after (lowest
20-22 KB before, 46 KB after), with no more skipped or late audio frames.
Moving Wi-Fi/lwIP buffers to PSRAM as well (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`)
gave 60 KB but skipped 49-136 outgoing frames per minute, so they stay
internal;
bounded PCM, packet queues and jitter storage prefer PSRAM. The microphone
single-consumer reservation excludes recording diagnostics from call capture,
and the shared speaker mutex excludes overlapping playback. The peer receive
timeout is 5 ms and its active wait is one tick, removing the previous extra
10 ms delay. Connected-loop duration, source PTS gaps and stage ages are
reported separately from Pion's RTP sequence-gap diagnostics.

`Before peer creation`, `Peer created`, `Connected`, `Peer released` and status
logs report internal free/minimum/largest-block memory, PSRAM free space and
worker stack high-water space in bytes. These are the measurements needed to
determine real handshake peaks and retained memory. Linker/image size alone
does not establish runtime memory use; `idf.py size` reports the static footprint.

### Verification and remaining limits

Before the Ring Test and teardown corrections, all three configurations built
with ESP-IDF 5.5.3 and the locked components:

| Configuration | Application binary |
| --- | --- |
| Microphone diagnostics, recording disabled | 1,179,936 bytes |
| KEY1 press-to-record | 1,185,328 bytes |
| Two-second boot recording | 1,183,088 bytes |

Each fit the 4 MiB application partition with 72% remaining. The earlier default
linker report used 138,711 bytes of DIRAM, leaving 203,049 bytes in that linker
region, plus 16,384 bytes of dedicated IRAM. These figures describe static
sections, not free heap after Wi-Fi, task creation or a DTLS handshake.

The corrected build using a separate copy of the supplied configuration, with
KEY1 hold-to-record enabled to match the already flashed board, also passes
with ESP-IDF 5.5.3: its application is 1,185,200 bytes (`0x1215b0`), leaving
72% of the 4 MiB app partition free. The other two configurations' figures
above are historical; their corrected-build sizes are not recorded here.

The worker and WebSocket tests compile the complete production sources against
real cJSON and the installed peer headers, with deterministic peer/RTOS/transport
mocks. From the repository root after exporting ESP-IDF:

```sh
python3 esp32/tests/webrtc_interop_test.py --idf-path "$IDF_PATH"
python3 esp32/tests/webrtc_interop_test.py --idf-path "$IDF_PATH" --sanitize
python3 esp32/tests/signaling_webrtc_test.py --idf-path "$IDF_PATH"
python3 esp32/tests/signaling_webrtc_test.py --idf-path "$IDF_PATH" --sanitize
python3 esp32/tests/call_audio_worker_test.py
python3 esp32/tests/call_audio_worker_test.py --sanitize
```

The 53 peer lifecycle cases and 12 WebSocket transport cases pass both normal
and AddressSanitizer/UndefinedBehaviorSanitizer runs. Existing Ring Test deadline,
microphone capture, recorded playback and KEY1 worker regression checks also
pass. The actual call-audio workers also pass normal and sanitized host runs,
including 1,000 fixed-clock capture frames with 6 ms peer polls, 100 modeled
two-descriptor playback cycles, injected stalls and cleanup/failure paths.
Pure jitter/render checks pass for reorder, missing/late packets, timestamp
wrap and conservative PCM gain. See the [audio guide](../docs/esp32-call-audio.md#validation-status)
for commands and model limits. Host tests verify sequencing and resource
ownership; peer/codec/I²S mocks do not establish transport or acoustic results.

The teardown regressions verify that transport failures after connection send
ordinary `hangup`, while setup failures retain `connect_timeout`. Audio adapter
checks cover queued encoded frames, preserved PTS, asynchronous startup and
shutdown, new-call exclusion during cleanup, idle-only audio modes and the
short scheduling wait. The revised ESP-IDF 5.5.3 call-audio build passes compilation:
its image is 1,369,904 bytes (`0x14e730`), leaving 67% of the 4 MiB partition
free, with ELF SHA-256 prefix `808a295d05`, and has been flashed.
The private hardware configuration selects a
1,000 Hz tick, while the supplied `sdkconfig` stays unchanged. Its microphone
and full-duplex physical results follow; initial repeat-cycle cleanup is verified.
Historical image sizes above refer to the earlier silence milestone.

The first physical TX-only call on the initial audio image `559ad7bda5`
failed immediately with a `digits_call_tx` stack overflow before audio
transmission. The correction
increases the initial 16 KiB codec stacks to 48 KiB for TX and 24 KiB for RX,
following the pinned codec's stack guidance. The revised firmware's TX-only
transport retest sent/decoded Opus and measured 27,136 bytes minimum free TX
stack without the immediate overflow.
The host codec mocks do not
measure the real Opus library's stack use. See the
[audio failure and retest record](../docs/esp32-webrtc-interoperability.md#first-physical-audio-failure-and-corrected-tx-transport-retest).

A Pion-originated TX-only call ran about 267.7 seconds and sent/received
13,372 Opus packets. Nine local source skips were about 0.0673% of source
slots; Pion saw no missing RTP sequences or recording errors. Nine timestamp
gaps matched those locally unsent frames. The finalized 267.44-second mono
WAV is quiet (peak 1,805, RMS 21.05); the user spoke for 10–15 seconds, but
recorded speech intelligibility remains unverified. An ESP32-originated RX-only
call then looped a known 7.842-second synthesized speech WAV, and the user
confirmed **“Speech is clear.”** RX minimum free stack was 16,000 bytes of
24 KiB; decode/write errors were zero. Its PLC, late-timestamp and producer-gap
observations remain separate continuity diagnostics, detailed in the
[physical audio results](../docs/esp32-call-audio.md#microphone-to-pion-transport-and-recording).
Neither call measured end-to-end latency or established long-duration stability.

The first full-duplex call on the priority-3 image lasted about 222 seconds;
the user described it as **“not so clear.”** ESP32's TX queue/pop drops totaled
718, about 6.46% of 11,116 encoded frames. Pion separately inferred 413 missing
RTP sequences, and ESP32 used 799 PLC frames with 90 inferred producer feed
gaps. These stage counters do not identify radio loss. The priority-6 correction
keeps core assignment and Wi-Fi power-save settings unchanged for its first
comparison and adds RSSI, power-save mode and capture-timeout diagnostics.
Its completed 121-second ESP32-originated full call prompted **“speech is
fine”** from the user. ESP32 sent 6,031 packets and Pion received all of them;
25 local pop drops were about 0.413% source skips. RX counted zero PLC,
late/missing timestamps, arrival drops or producer feed gaps. TX/RX minimum
free stacks were 27,136/16,012 bytes and internal minimum free was 39,915 bytes.
An amplified/filtered private copy of this call's microphone recording was
then looped in a 193-second Pion-originated full call, and the user confirmed
**“My words are clear.”** Pion received all 9,587 submitted packets, while
50 source PTS skips matched its timestamp gaps. RX used three PLC frames
and counted one inferred producer gap, with zero decode/write errors or
arrival-queue drops. TX/RX stack minima remained 27,136/16,012 bytes; internal
minimum free was 37,955 bytes, and idle internal/PSRAM free returned to
158,735/8,362,080 bytes. Two concurrent KEY1 requests declined with `Speaker
busy` while speech continued; concurrent Ring Test could not be dispatched
because the WebUI disabled its button. After hangup, the user confirmed Ring
Test and KEY1 recording/playback worked as usual; logs show 48,000 ring frames,
98,880 recorded samples and resumed capture. Four additional calls completed
cleanup with stable PSRAM free, a 63,488-byte largest internal block and idle
internal free of 158,467–158,499 bytes. The first two had local playback
skips/feed gaps despite zero inferred missing RTP sequences at Pion; occasional
local scheduling delays remain. Of five acoustic-loop probes, one had no RTP
or source timestamp gaps and estimated about 170 ms for Pion → speaker →
microphone → Pion. It is a single observation with uncalibrated software
origins and uncertainty that can exceed a 20 ms frame; it establishes neither
one-way delay nor jitter or a proven timing bound. See the
[latency qualifications](../docs/esp32-call-audio.md#single-acoustic-round-trip-estimate).
The final console check showed online pairing, idle/full mode and
`cleanup_pending=no`, with internal free/minimum/largest block
158,475/37,955/63,488 bytes and PSRAM free 8,362,080 bytes. The current boot
logged no capture I²S read faults or RX DMA overruns. Pion and serial tools
stopped normally, and the board stayed running. Actual playback DMA underruns,
raw speech level and long-duration stability remain separate limitations.
Originals and firmware transmit gain were preserved. The pinned encoder hides
underlying UDP callback errors, so
successful application submission does not prove wire delivery. See the
[first full-duplex result](../docs/esp32-call-audio.md#first-full-duplex-continuity-failure).

For subsequent hardware checks, use an existing Digits/Pion handset or the
[Pion test helper](../tools/interop/README.md), with both endpoints on the same
LAN in an authorized Digits family and working registration:

1. Boot the ESP32 and enter `webrtc status`. Confirm signaling is online,
   paired is yes and the peer is idle.
2. Enter `webrtc call PION_NUMBER` and answer on the existing handset. Confirm
   `PAIRING`, `PAIRED`, `CONNECTING` and `CONNECTED`, then increasing transmit
   and receive counters and the Pi's connected state.
3. End the call with `webrtc hangup`. Dial the ESP32's saved number from the
   existing handset, then enter `webrtc answer` after the incoming-call log.
   Repeat the connection and packet checks, then hang up from the Pi side.
4. Repeat both directions and compare `Peer released` memory. Exercise Ring
   Test, KEY1 recording if enabled, WebSocket PING/PONG and Wi-Fi loss/recovery.
   Signaling loss should release the call and reconnect with the saved identity.

An SDP answer or `PAIRED` alone is insufficient. `First remote encoded audio
packet received after SRTP processing` is a transport reception check. For
audible interoperability, additionally follow the separate microphone-WAV,
speaker speech and full-duplex checks in the
[audio validation guide](../docs/esp32-call-audio.md#physical-validation-procedure).
Record both endpoints' logs and resource reports.

Only IPv4 UDP host/STUN connections are enabled in this milestone. Unsupported
TURN/TURNS, TCP and IPv6 server URLs are diagnosed and skipped; there is no
claim of relay or general Internet/NAT coverage. Video, data channels, group
calls, ICE restart and surviving signaling loss are outside this step.

The peer core is prebuilt. Its visible DTLS code uses optional certificate
verification; comparison of the remote certificate to the signaled fingerprint
could not be established. Do not treat this milestone as verified peer identity
authentication or production readiness. No server or peer-library workaround
was introduced.

**Nine LAN calls verified encoded transport interoperability on 2026-10-11
(Europe/Berlin): seven before the corrections and two afterward.**
The initial board firmware (`7b94a10-dirty`, ESP-IDF 5.5.3, `esp_peer` 1.5.7)
and the Fedora 44 Pion helper completed four Pion-to-ESP32 and three
ESP32-to-Pion calls through the unchanged Digits deployment, including a
successful call after manually reconnecting Pion with the saved identity.
Every call connected, received SRTP-protected Opus in both directions and
returned both peers to idle. Pion's encode/decode/write error counters stayed
at zero; no board reboot or crash was observed during those seven calls.
Existing microphone capture and level diagnostics continued throughout the calls.

After warmup, idle internal free memory stayed around 166,200–166,400 bytes,
PSRAM free returned to 8,362,080 bytes and minimum peer-worker stack headroom
was 11,408 bytes. This measurement covers the WebRTC worker only, not the
separate `digits_ring` worker. This short run does not prove absence of leaks.
The initial firmware mislabeled post-connection transport closure as
`connect_timeout`; the corrected behavior is recorded below. Approximately
21.5% of send slots were locally skipped in one first-call status sample
(1,222 submitted, 334 skipped, zero blocked) with Wi-Fi RSSI around -90 dBm.
Skipped slots are not a network-loss measurement. See the
[hardware verification record](../docs/esp32-webrtc-interoperability.md#live-hardware-verification)
for endpoint versions, packet counters and resource observations. Audible call
audio, concurrent Ring Test/recorded playback, ESP32 network-loss recovery and
long-duration stability remain unverified.

**The initial firmware's subsequent Ring Test smoke check failed.** At uptime 1,033,603 ms,
`Playing 1s speaker test` was followed by a `digits_ring` stack-overflow panic,
a corrupted backtrace and a software reset. Repeated requests produced five
such panics in the capture, with one intervening Ring Test completing normally;
the user heard the tone only on some presses. The board retained number
`3140001` and reconnected after reset. This failed the initial firmware's smoke
check while preserving its bounded seven-call LAN transport result.

The corrected firmware (SHA-256 prefix `6970a82cf`) has been flashed using the
separate private configuration copy described above. The supplied `sdkconfig`
selected recording disabled and was left unchanged; the copy enabled KEY1
hold-to-record to preserve the already flashed board's behavior. Bootloader,
application and partition table were updated without erasing NVS; the original
hardware ID and number `3140001` were retained. One further call in each
direction established host ICE, DTLS/SRTP and bidirectional Opus reception,
with zero Pion codec/write errors, and returned both endpoints to idle.
Pion's 48 kHz Opus negotiation uses two SDP channels; received ESP32 silence
decodes into 960 mono samples per 20 ms frame. A Pion hangup reproduced DTLS
closure before signaling hangup; ESP32 logged `peer transport failed` and now
sends ordinary `hangup` without `connect_timeout`. An ESP32 local hangup was
received by Pion with an empty reason.

After those two calls, idle internal free memory was 160,315 bytes, internal
minimum was 116,123 bytes, largest internal block was 63,488 bytes and PSRAM
free was 8,362,080 bytes. Peer-worker stack headroom was 11,432 bytes. These
corrected-build measurements are separate from the initial seven-call values
and do not measure the Ring Test worker's stack.

The Ring Test correction moves its 1,920-byte PCM block off the worker stack
into static storage and increases the stack from 4,096 to 8,192 bytes. Host
checks pass. After both corrected-build calls, 15 hardware Ring Tests reached
their normal stop logs between boot uptimes 148,103 and 185,893 ms, with no
stack overflow, panic or further reset after the initial USB reset. Minimum
Ring Test worker stack headroom was 5,932 bytes of its 8,192-byte stack.
Playback queued 47,520 or 48,000 frames within the existing deadline;
maximum render time was 218 microseconds and maximum producer gap was
219 microseconds, against a 10,000-microsecond DMA block.

At uptime 196,223 ms and the final USB status at 262,183 ms, the board remained
online, paired and idle with internal
free memory of 160,339 bytes; internal minimum, largest block, PSRAM free and
peer-worker stack headroom matched the post-call values above.
**The repeated Ring Test hardware check passed:** the user confirmed a
consistently audible tone on every WebUI button press. These 15 completed
playbacks are a bounded short retest, not a long-duration firmware stability result.
Live microphone encoding and received-audio playback are now implemented.
The newer RX-only call confirmed clear speech; its TX recording and remaining
full-duplex checks are separate from this historical silence/Ring Test baseline.

## Ring Test contract

The device's Ring Test button in the Digits WebUI calls authenticated
`POST /phones/{number}/ring-test`, with form field `hardware_id` selecting
the handset. `Handler.handlePhoneRingTest` constructs
`Message{Type: TypeRingTest}`; `Hub.SendToHardware` delivers it over the
device's registered WebSocket (the legacy fallback uses `Hub.SendTo`).
The exact v1.99.0 serialized command is:

```json
{"type":"ring_test","voicemail_unheard_count":0}
```

Only `type` matters here. The count is a default envelope field without
`omitempty`, not a Ring Test parameter. There is no duration, frequency,
volume, WebSocket stop message, or device acknowledgement. The HTTP result
`{"status":"triggered"}` and UI "Done" confirm command dispatch, not playback.

The reference Pi `daemonCallbacks.handleSignal` sends UART `RING:TEST`, then
its goroutine sends `RING:STOP` after one second. Pico `process_pi_command`
bypasses the phone/hook state machine for the test and starts its physical
20 Hz bell driver. No audio file or PCM ringtone is involved. ESP32 uses a
locally generated 440 + 480 Hz speaker tone for the same brief hardware test.

Reference implementation:

- `server/internal/web/static/phone-detail.js`: `doRingTest`, `amDoRingTest`.
- `server/internal/web/handler_phones.go`: `handlePhoneRingTest`, `sendDeviceCommandAndRespond`.
- `server/internal/signaling/protocol.go`: `TypeRingTest`, `Message`, `Message.Marshal`.
- `server/internal/signaling/hub.go`: `SendToHardware`, `SendTo`.
- `pi/digitsd/cmd/digitsd/dispatch.go`: `daemonCallbacks.handleSignal`, `TypeRingTest` case.
- `firmware/src/phone_fsm.c`: `process_pi_command`.
- `firmware/src/ringer.c`: `ringer_start`, `ringer_update`.

## Speaker output

Board wiring is verified against Waveshare's v1.1 schematic. Codec setup
follows Espressif's official ESP-IDF v5.5.3 ES8311 example:

| Signal | Board connection |
| --- | --- |
| I2C SDA / SCL | ESP32 GPIO11 / GPIO10, 100 kHz |
| ES8311 address | 7-bit `0x18`; codec library takes its 8-bit constant `0x30` |
| I2S MCLK / BCLK / LRCK | GPIO12 / GPIO13 / GPIO14 |
| I2S DAC data | GPIO16, ESP32 TX to ES8311 |
| I2S ADC data | GPIO15, ES7210 SDOUT1/TDMOUT to ESP32 RX |
| NS4150B PA_CTRL | TCA9555 address `0x20`, port 1 bit 0 (`P10` / `Extend_IO8`), high enables |
| Speaker | ES8311 differential output -> NS4150B -> H3 speaker connector |

The shared `audio_board.c` infrastructure owns I2C and the native I2S0
full-duplex TX/RX pair. The ESP32 supplies clocks; ES8311 and ES7210 are
slaves: Philips I2S, signed 16-bit samples in two 16-bit slots, 48 kHz,
MCLK 256 x sample rate = 12.288 MHz, BCLK = 1.536 MHz. Ring Test sends
identical samples to both speaker slots at this rate. Espressif
`esp_codec_dev` **1.6.2** initializes the ES8311 DAC and ES7210 ADC paths.
The expander writes preserve every other pin; PA is latched low before
configuring its direction. TX and RX retain the same clock/slot configuration
during operation; microphone diagnostics never reconfigure speaker output.

The initial DAC volume is **50/100** and the generated PCM peak is capped at
25% of full scale. Short fades reduce clicks. This is an intentionally low
starting level; actual loudness must be assessed on the board. The amplifier
is disabled and DAC muted while idle. DMA auto-clear prevents a stale buffer
from repeating after playback. Codec initialization failure is logged and
registration/pairing continue; a test then logs that audio is unavailable.

`digits_ring_test_request` only overwrites a one-element queue without waiting.
A separate FreeRTOS task performs codec/amp control and finite-timeout I2S
writes while holding the shared speaker reservation. Call playback and the
optional microphone record/playback worker use that same reservation to keep
playback exclusive. Ring Test requests during a receive/full call are skipped
while the call owns the speaker; retry after hangup and completed cleanup.
It disables PA after one second regardless of the WebSocket's state. Pending
overlapping requests are coalesced without extending the first deadline;
unlike the Pi they do not restart the waveform. The normal WebUI disables
its button for two seconds, so this distinction does not affect normal use.
Audio does not write NVS or send signaling messages.

At the final deadline boundary, the worker stops queuing PCM when there is
less than one DMA block plus a scheduler tick left, then waits for the normal
one-second stop. With the default 100 Hz tick this margin is 20 ms. This avoids
I2S waits rounding down to zero ticks and reporting a spurious final-block
timeout. Timeouts earlier in playback remain errors and stop the test.

Ring Test caches one 25 ms common period of its 440+480 Hz tone during
initialization, before the playback task starts. At 48 kHz this is 1,200 mono
samples (2,400 bytes of internal RAM). Streaming copies the table into stereo
buffers and applies integer fades; it performs no sine calculations or
allocations. Frequencies, volume, peak ceiling and the one-second stop remain
the same.

This addresses a likely cause of irregular gaps after adding microphone
capture: the old renderer calculated two sine functions per sample. Moving
from 16 to 48 kHz tripled that workload to 96,000 calls per second while native
capture also runs. Late buffer refills can produce silent DMA blocks even
when subsequent writes succeed. After each test, `Stream timing` reports
queued frames, maximum render time and the longest producer gap between
writes, against the 10,000 us block duration. These are timing measurements,
not a hardware underrun counter. The cached renderer still requires an
audible Ring Test on the board to confirm the reported crackles are resolved.
A regular flutter is part of the chosen dual tone's 40 Hz amplitude beating;
uneven crackles or gaps are not.

Official hardware/software references:

- [Waveshare v1.1 schematic](https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board_1.1.pdf)
- [Waveshare ESP-IDF demos and explanations](https://docs.waveshare.com/ESP32-S3-AUDIO-Board/ESP-IDF)
- [Espressif v5.5.3 ES8311 I2S example](https://github.com/espressif/esp-idf/tree/v5.5.3/examples/peripherals/i2s/i2s_codec/i2s_es8311)
- [Espressif esp_codec_dev 1.6.2](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2/readme)
- [Official Waveshare example package](https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board-Demo.zip)

The official Waveshare example package was downloaded and inspected for the
microphone milestone. Its `ESP-IDF/esp_sr_02/main/hardeware_driver/bsp_board.[ch]`
confirms the shared I2S/I2C pins and ADC initialization. Its four-channel
packing differs from this firmware, as documented below.

## Microphone capture

The Waveshare v1.1 schematic's U8 ES7210 connects the physical onboard
microphones as follows. Schematic designators identify the electrical paths;
tap near each microphone to confirm its position on your assembled board.

| Signal/channel | Configuration |
| --- | --- |
| Onboard MIC1 | ES7210 MIC1P/MIC1N, ADC channel 1; standard-I2S left/WS-low slot |
| Onboard MIC2 | ES7210 MIC2P/MIC2N, ADC channel 2; standard-I2S right/WS-high slot |
| ADC MIC3 | Speaker echo-reference path from ES8311 OUTP/OUTN; disabled |
| ADC MIC4 | Unused input; disabled |
| ES7210 I2C address | 7-bit `0x40` (A0/A1 low); codec component takes 8-bit `0x80` |
| Microphone bias | ES7210 MICBIAS12, 2.87 V |
| Serial input | GPIO15, ES7210 SDOUT1/TDMOUT through schematic R47 |
| Serial output format | 48 kHz Philips I2S; two signed 16-bit samples per frame, MSB first, one BCLK delay |
| Native RX buffer layout | Little-endian `int16_t`: `[MIC1, MIC2, MIC1, MIC2, ...]` |
| Application output | 48 kHz signed 16-bit mono; MIC1 by default, selectable MIC2 or average |

The ADC selects only `ES7210_SEL_MIC1 | ES7210_SEL_MIC2`, so the pinned driver
uses ordinary stereo rather than TDM. It configures native 16-bit samples;
no arbitrary bit shifts, channel guessing or higher-depth truncation are
needed. Initialization checks ES7210 interface registers `0x11 == 0x60`,
`0x12 == 0x00` and slave mode in `0x08`. The board driver checks the native
I2S clock and slot configuration. Format or I2C failures are logged; successful
speaker and Digits signaling operation continue if ADC initialization fails.

The official Waveshare ESP-SR demo enables four ADC channels. It uses two
32-bit standard-I2S containers to carry four 16-bit TDM samples and describes
the little-endian memory order as `RMNM` (reference, mic, unused, mic).
Everest's datasheet Figure 2e gives the TDM wire order `1,3,2,4`; packing two
samples per 32-bit word produces memory order `3,1,4,2`. Its extraction of
memory positions 1 and 3 must not be copied into our native 16-bit stereo
capture. Espressif's ES7210 channel-map table explicitly defines standard
two-channel order `(1,2)`.

References for microphone format and ordering:

- [Everest ES7210 datasheet](https://files.waveshare.com/wiki/common/ES7210-datasheet.pdf), clock modes and digital audio interface, pages 3–6.
- [Espressif ES7210 channel-map definitions](https://github.com/espressif/esp-audio-dev/blob/main/esp_codec_dev/device/es7210/es7210.c), `order_info`; this is mapping evidence, not a dependency upgrade.
- [ESP-IDF v5.5.3 ES7210 recording example](https://github.com/espressif/esp-idf/blob/v5.5.3/examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm/main/i2s_es7210_record_example.c), codec configuration and clock setup; that example uses TDM.
- Local pinned component: `managed_components/espressif__esp_codec_dev/device/es7210/es7210.c`, `es7210_mic_select`, `es7210_set_bits`, `es7210_config_fmt`, `es7210_set_fs`.

`digits_microphone_init()` starts a dedicated capture task at priority 5.
Each native RX DMA descriptor holds 480 stereo frames (10 ms); two descriptors
are allocated. The task assembles 960-frame blocks (20 ms), preserves partial
reads without losing sample/slot alignment, and discards the first 200 ms
after ADC startup. `microphone_pcm_convert()` selects either microphone or
averages using a signed 32-bit sum before division by two. It also computes
per-channel and mono statistics when diagnostics are enabled.

The application queue holds four copied mono frames (80 ms, 7,680 bytes of
PCM plus frame headers). Its producer never waits: a full queue drops the
oldest frame. Capture uses one bounded 3,840-byte stereo staging buffer and
one 1,920-byte mono staging buffer. Normal capture has no accumulating
recording or PSRAM requirement; the optional record/playback modes below
allocate bounded buffers in PSRAM. A separate priority-3 logger consumes only
the latest one-second statistics report, keeping serial formatting/writes outside the
capture task. Neither audio task performs WebSocket or NVS work.

### Enable the development diagnostic

From the repository root, reuse the existing Wi-Fi/server configuration:

```sh
export IDF_TOOLS_PATH=/private/tmp/digits-idf-tools
. /private/tmp/digits-esp-idf/export.sh
cd esp32
idf.py menuconfig
# Digits development configuration -> Onboard microphone:
#   Enable microphone level diagnostic at boot: enabled
#   ES7210 microphone gain (dB): 24
#   Microphone channel for mono PCM: MIC1 (left I2S slot)
idf.py build
idf.py -p /dev/cu.usbmodemYOUR_DEVICE flash monitor
```

Select the currently connected serial port. An existing ESP-IDF installation
can use its normal `export.sh` instead of the temporary workspace toolchain.
Do not run `set-target` or erase flash just to enable diagnostics. Regular
flash retains NVS identity, number and the original device token.

`CONFIG_DIGITS_MIC_DIAGNOSTIC` defaults **off**. The diagnostic can also be
toggled from an application task with `digits_microphone_set_diagnostic(true)`
or `false`; there is no console command or new Digits signaling message.
Disabling it stops level reporting/statistics, while capture, speaker output
and Digits operation keep running. Gain defaults to 24 dB and can be set to
0–36 dB; prefer multiples of 3 dB to match hardware gain steps. Reduce gain
if clipping appears. The pinned driver's gain mapping is discrete: a request
of 33 dB maps to 30 dB, while 36 dB selects 36 dB. The default 24 dB is exact.
Mono source selection is a build setting.

Expected startup format (illustrative; levels below are not measurements):

```text
audio_board: Speaker ready: ES8311 DAC, 48000 Hz, MCLK=12288000 Hz; PA=TCA9555 P10; volume=50
microphone: Capture ready: ES7210 MIC1+MIC2, 48000 Hz, 16-bit Philips stereo -> mono left/MIC1, gain=24 dB; diagnostics=on
microphone: mono RMS=200.0 (-44.3 dBFS) peak=1000 (3.1%) [#######.............]; L RMS=200.0 peak=1000; R RMS=180.0 peak=900; samples=48000
```

Reports arrive approximately once per second and contain RMS, peak, a mono
dBFS meter and both raw microphone levels. Speaking or tapping nearby should
increase these values visibly; raw PCM is never continuously printed.
Persistent RMS below two sample units on either microphone or mono causes
a near-silence warning after five consecutive diagnostic reports. Clipping
is counted at absolute PCM magnitude at least 32,760. Read failures and RX
DMA overruns are reported, with affected frames marked discontinuous; errors
remain visible even when level diagnostics are disabled. An empty window
reports that no complete PCM frames were received. Silence warnings indicate
a diagnostic condition and do not change gain or stop normal Digits operation.

Microphone acceptance checks on the board:

1. Confirm both audio readiness logs and saved paired registration. Speak
   near the microphones and observe RMS/peak rising above quiet-room levels.
2. Tap near each microphone gently; verify both raw slot meters respond and
   confirm the physical MIC1/MIC2 positions. Rebuild with MIC2 selected to
   check the other mono path if necessary.
3. Check the frame count is approximately 48,000 samples per one-second
   report after warmup. Persistent zero/silent channels, clipping, read errors
   or DMA overruns require investigation before using PCM for encoding.
4. Run the Digits WebUI Ring Test while capturing. Expect the same finite
   one-second tone; microphone levels naturally rise from acoustic pickup.
   Confirm WebSocket server PING/PONG continues and the handset stays online.
5. Disable level diagnostics, rebuild/flash, and verify pairing, Ring Test and
   reconnection still work. Reboot to verify saved credentials are retained.

The initial microphone milestone was built and host-tested without a
connected USB serial device. On 2026-10-09 the user subsequently reported
promising microphone diagnostic levels on the board. No specific acoustic
measurements or record/playback result are claimed by that report. Complete
the checks above and the optional playback check below on the actual board.
No microphone-to-speaker loopback runs.

### Optional two-second record-then-playback test

The `Microphone record/playback diagnostic` menuconfig choice defaults to
**Disabled**. Its two alternatives are the existing boot test
(`CONFIG_DIGITS_MIC_RECORD_PLAYBACK`) and the [KEY1 hold/release test](#optional-key1-hold-to-record-release-to-play-test)
(`CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK`). Only one mode can be selected.

When the boot test is selected, a dedicated
task waits five seconds after startup, prints a prompt, and records exactly
96,000 mono samples (100 frames of 960 samples, two seconds at 48 kHz).
It then stops publishing microphone frames and acknowledges ADC input mute
before enabling the speaker. Native RX continues draining on the shared I2S
bus; this avoids interrupting the speaker clocks. Playback reads only the
completed saved buffer. After playback the amplifier is gated off, capture
resumes and discards another 200 ms of settling samples.

The test needs a fixed **192,000-byte (187.5 KiB)** allocation in the
ESP32-S3R8's 8 MB Octal PSRAM. It does not fall back to internal memory if
PSRAM is absent or the allocation fails. Output uses the same 48 kHz stereo
speaker path and DAC volume 50/100. A fixed gain is calculated for each saved
recording, as described [below](#saved-recording-playback-levels); output is
limited to absolute PCM magnitude 8,192 and fades over at most 5 ms at each
end. Normal capture remains bounded as described above, and the recording
buffer is freed when the one-shot test finishes.

Enable PSRAM first so the dependent test option appears in menuconfig. Reuse
the current development configuration and saved credentials:

```sh
export IDF_TOOLS_PATH=/private/tmp/digits-idf-tools
. /private/tmp/digits-esp-idf/export.sh
cd esp32
idf.py menuconfig
# Component config -> ESP PSRAM:
#   Support for external, SPI-connected RAM: enabled
#   SPI RAM config -> Mode (QUAD/OCT) of SPI RAM chip in use: Octal Mode PSRAM
#   SPI RAM config -> Type of SPIRAM chip in use: Auto-detect
#   SPI RAM config -> Set RAM clock speed: 40Mhz clock speed
#   SPI RAM config -> SPI RAM access method:
#     Make RAM allocatable using malloc() as well
#   SPI RAM config -> Ignore PSRAM when not found: enabled
# Digits development configuration -> Onboard microphone:
#   Microphone record/playback diagnostic:
#     Run a 2-second record-then-playback test once after boot
#   Enable microphone level diagnostic at boot: enabled (optional)
#   ES7210 microphone gain (dB): 24
idf.py build
idf.py -p /dev/cu.usbmodem21122101 flash monitor
```

Use the currently connected port if its name changes. Retain the WebRTC
configuration's malloc integration so large peer allocations can use PSRAM.
The recording itself explicitly uses `heap_caps_malloc` with
`MALLOC_CAP_SPIRAM`, independently of the ordinary allocation threshold.
"Ignore PSRAM when not found" allows signaling and normal capture to boot
even if PSRAM initialization fails; the recording test then logs a missing
buffer and skips playback. Configuration and a regular flash preserve the
paired UUID, number and original device token. After installing this
milestone's 4 MiB app partition table, changing recording modes needs no
further partition change. No flash erase is required.

Keep the monitor open immediately after reboot. Expected application messages
for a successful test (illustrative until tested on the board):

```text
record_test: One-shot microphone test in 5s; speak when 'Recording 2s now' appears
record_test: Recording 2s now: speak near the microphone
record_test: Saved PCM: RMS=200 peak=1000 DC=0; AC RMS=200; playback gain=10.24x (peak cap=8192)
record_test: Recording stopped: 96000 samples; playing saved 2s audio with bounded gain
record_test: Playback PCM: RMS=2000 peak=8192 limited=100/96000 samples
record_test: Playback finished; amplifier disabled, microphone capture resumed
```

Speak a short phrase when the recording prompt appears. Remain quiet during
playback and verify the speaker repeats the saved phrase once, then becomes
silent. Level reporting may show `Input muted for recorded playback; level
reports paused` while capture publication is paused. After completion,
microphone reports should resume and WebSocket PING/PONG should continue.
Reboot to repeat; there is no automatic repetition or continuous loopback.
Select **Disabled** in the record/playback choice and rebuild/flash after
verification.

The speaker is reserved throughout recording and playback, preventing Ring
Test audio from being recorded or overlapping playback. A Ring Test arriving
during this short interval logs that the speaker is reserved and is skipped;
retry it after the completion message. The WebUI's HTTP success still means
dispatch, not playback. Outside this optional test, Ring Test operates as
before. The diagnostic holds the microphone consumer reservation throughout
recording and playback. Call capture uses the same reservation, so they cannot
consume PCM or pause/resume the ADC concurrently. Busy requests fail cleanly;
keep diagnostics idle while validating call audio.

Recording gaps, read failures, a failed ADC mute or speaker write failure abort
the test. An incomplete recording is not played. Cleanup gates off the
amplifier and requests capture resume after any attempted pause, including a
pause timeout. Treat `Capture resume failed` as a failed test and investigate
before relying on capture. The diagnostic never modifies NVS or sends a new
Digits message. Playback of saved speech through this recording diagnostic
remains to be verified on physical hardware.

### Optional KEY1 hold-to-record, release-to-play test

Select `Hold KEY1 to record; release to play (maximum 10 seconds)` in the
same menuconfig choice to enable `CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK`.
Reuse the Octal PSRAM, 40 MHz and malloc integration
configuration [above](#optional-two-second-record-then-playback-test); this
mode has the same PSRAM dependency and leaves the boot test disabled.
From the repository root:

```sh
export IDF_TOOLS_PATH=/private/tmp/digits-idf-tools
. /private/tmp/digits-esp-idf/export.sh
cd esp32
idf.py menuconfig
# Component config -> ESP PSRAM:
#   Reuse the Octal / 40 MHz / malloc integration configuration above.
# Digits development configuration -> Onboard microphone:
#   Microphone record/playback diagnostic:
#     Hold KEY1 to record; release to play (maximum 10 seconds)
#   Enable microphone level diagnostic at boot: enabled (optional)
#   ES7210 microphone gain (dB): 24
idf.py build
idf.py -p /dev/cu.usbmodemYOUR_DEVICE flash monitor
```

KEY1 is the lowest button on the right when the USB connector is at the top.
The buttons run from top to bottom **RESET, BOOT, KEY3, KEY2, KEY1**, as shown
in the [official board photo](https://docs.waveshare.com/assets/images/ESP32-S3-AUDIO-Board-details-intro-b85598a5d92aa52b773d6565b09ee360.webp).
The [v1.1 schematic](https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board_1.1.pdf)
connects KEY1's `K1` signal through `Extend_IO9` to TCA9555 P11, active low.
BOOT and RESET keep their hardware functions. Button sampling polls every
10 ms and requires 30 ms of stable input before accepting an edge.

1. Start with KEY1 released and wait for the startup prompt. A key held during
   startup is ignored until released.
2. Hold KEY1 and speak near the microphones after `KEY1 held: recording now`
   appears. Release to hear the saved phrase through the speaker once.
3. Wait for playback to finish, then use a fresh press for another recording.
   Presses during playback are ignored; a released key must be observed before
   the next test can start. Very short taps can contain no complete PCM frame.
4. If held for ten seconds, recording stops and waits for release. Release
   KEY1 to play the saved recording; holding longer does not extend it.

Expected messages for a one-second recording (illustrative, not a measured
hardware trace):

```text
record_button: K1 ready: TCA9555 P11 (Extend_IO9), active-low; debounce=30 ms
record_test: Hold KEY1 to record, release to play (maximum 10s); waiting for released key
record_test: KEY1 held: recording now (maximum 10s); release to play
record_test: Saved PCM: RMS=200 peak=1000 DC=0; AC RMS=200; playback gain=10.24x (peak cap=8192)
record_test: KEY1 released: playing 1000 ms (48000 samples) with bounded gain
record_test: Playback PCM: RMS=2000 peak=8192 limited=50/48000 samples
record_test: KEY1 playback finished; amplifier disabled, capture resumed
```

At the cap, expect `10s recording limit reached; release KEY1 to play`.
The mode reserves the speaker during recording and playback. Once the cap is
reached it mutes the ADC and releases the speaker while waiting, so the Digits
WebUI Ring Test can still run. If the speaker is busy when KEY1 is released,
the saved test is discarded with `Speaker busy after release; recording
discarded`; start again with a fresh press after the speaker becomes free.

Each test makes one bounded **964,000-byte PSRAM allocation**: 960,000 bytes
for up to 480,000 signed 16-bit mono samples (ten seconds at 48 kHz), plus
4,000 bytes for 500 frame completion timestamps. The allocation is aligned
for the 64-bit timestamps. Only complete 20 ms frames acquired after the
speaker reservation are stored, excluding older queued audio such as a
recent Ring Test. Release trims tail frames whose I2S read completion timestamp
is after the first raw sample of the confirmed release edge; these timestamps
are not calibrated ADC sample times, so the saved duration is approximate.
The allocation is freed after playback or an abort, and allocation failure
does not use internal RAM as a fallback.

Playback uses only the saved mono buffer, with the same fixed per-recording
gain and output ceiling as the boot test. ADC input is muted and capture
publication paused during playback, while native RX keeps draining the shared
I2S bus. Completion disables the amplifier, resumes capture and its settling
warmup. There is no continuous microphone-to-speaker loopback. Button read
errors, recording gaps, pause failures and speaker write failures abort the
test and perform the same amplifier/capture/buffer cleanup. Investigate a
`Capture resume failed` message before relying on capture again. Normal
WebSocket registration, PING/PONG, pairing and NVS storage are unchanged.

Firmware builds pass for diagnostics disabled, the two-second boot test and
the KEY1 mode. Host regressions pass for debounce, hold/release recording,
release-tail trimming, the ten-second cap, Ring Test speaker ownership and
error cleanup. These results preceded the saved-recording gain adjustment.
The user subsequently flashed KEY1 mode and supplied a roughly 3.5-second
recording/playback trace with no I2S write errors and normal capture resume,
but heard no saved audio. The same firmware's Ring Test remained audible.
The gain adjustment builds successfully in all three configurations. Numeric
rendering tests pass under ASan/UBSan, and boot/button workflow regressions
verify quiet PCM with a DC offset receives about 10x gain, correct output
metrics and safe cleanup. On the current `808a295d05` call-audio firmware,
the post-call hardware check recorded 98,880 samples over 2,060 ms and resumed
capture after playback; the user confirmed KEY1 recording/playback worked as
usual. Additional board acceptance checks include
startup-held handling, a short phrase, repeated recordings, the ten-second
cap, a Ring Test while waiting at the cap, and normal capture/signaling after
playback. [Microphone acceptance checks](#enable-the-development-diagnostic)
and the [existing Ring Test documentation](#flash-and-test-ring-test) cover
the shared audio paths. Select **Disabled** and rebuild/flash when finished.

### Saved-recording playback levels

Both development playback modes remove the saved recording's mean DC offset
and choose one fixed gain for its entire playback. The target is AC RMS
2,048 signed 16-bit sample units (about -24 dBFS), with gain bounded between
0.25x and 32x. Recordings with AC RMS below 16 retain quarter gain; this avoids
boosting numerical near-silence and does not identify or suppress general
room noise. Samples are limited to +/-8,192 before the existing fades, matching
the Ring Test's PCM peak ceiling. Limiting, gain bounds and fades can make the
actual output RMS lower than the target.

This changes only rendering of the completed saved buffer. Microphone ADC
gain stays at its configured value, speaker DAC volume stays at 50/100, and
Ring Test rendering is unchanged. Capture frames remain raw signed 16-bit
mono PCM; no automatic gain control is added to capture or the call Opus
input. Speaker reservation, ADC mute, RX draining and capture resume keep
the same behavior in both playback modes.

The supplied diagnostic levels were mono RMS approximately 160–200, with
peaks around 700–1,100. The earlier quarter-amplitude renderer reduced that
RMS to about 40–50, roughly 38–40 dB quieter than the Ring Test's RMS of
approximately 4,096 through the same output path. This is consistent with
inaudible playback despite completed writes. DC and AC measurements
now distinguish a steady offset from varying audio; neither RMS nor a
successful write proves that the recording contains intelligible speech.

After flashing the gain revision, hold KEY1 and speak a short phrase near
the microphones after the recording prompt, then release and remain quiet
during playback. Read the new `Saved PCM` and `Playback PCM` lines:

- `Saved PCM` reports raw RMS and peak, removed mean (`DC`), AC RMS after DC
  removal, and the fixed playback gain.
- `Playback PCM` reports measured rendered output RMS and peak after gain,
  limiting and fades, plus the number of peak-limited mono samples. These
  are samples supplied to TX, not an acoustic speaker measurement.

Output peak must stay at or below 8,192. Confirm that the phrase is audible,
capture reports resume and Digits server PING/PONG continues. For the boot
mode, perform the same check when `Recording 2s now` appears after reboot.
Use the reported input/output levels when investigating remaining silence;
do not assume that increasing microphone gain is required.

Host record/playback rendering and workflow checks, from the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -I esp32/main \
  esp32/main/record_playback_pcm.c esp32/tests/record_playback_pcm_test.c \
  -lm -o /tmp/digits-record-playback-pcm-test
/tmp/digits-record-playback-pcm-test
python3 esp32/tests/record_playback_test.py
python3 esp32/tests/record_button_test.py
python3 esp32/tests/button_record_playback_test.py
```

Rendering checks cover signed extremes, identical stereo slots, short/variable
recording lengths, fade endpoints and block boundaries. The gain checks cover
DC removal, quiet input, gain bounds, peak limiting and rendered statistics.
The workflow tests compile the actual boot/button test bodies with fake
capture/speaker APIs to check ordering and error cleanup. The button regression
checks debounce, startup holds, fresh presses and read failures. These checks
complement the microphone/Ring Test host regressions; they do not verify
PSRAM, ADC audio, the physical button or speaker acoustics on the board.

### Feeding the call Opus encoder

`microphone.h` exposes a reserved single-consumer API used by the call encoder
and recording diagnostics:

```c
if (digits_microphone_acquire(0)) {
    digits_microphone_frame_t frame;
    if (digits_microphone_read(&frame, 100) == ESP_OK) {
        // 960 signed 16-bit mono samples: 20 ms at 48 kHz.
        // Check sequence, discontinuity flags and freshness before encoding.
    }
    digits_microphone_release(); // Same task that acquired the reservation.
}
```

`digits_microphone_read()` copies a frame into caller-owned storage. A zero
timeout polls; waits are limited to 1,000 ms. Consumers never own or retain a
DMA pointer. Every frame contains `sequence`, `flags`, `captured_at_us` and
1,920 bytes of PCM. The timestamp is RX read-completion time, not a calibrated
ADC timestamp. The first published sequence is 1; sequence gaps and
`DIGITS_MIC_FRAME_DISCONTINUITY` identify dropped/lost capture audio. The call
encoder checks freshness and sequence gaps, preserving known missing frame
time in outgoing PTS. The queue remains finite when no consumer is active.
Saved-recording diagnostic playback gain does not alter this PCM API or add
capture AGC. The [call audio guide](../docs/esp32-call-audio.md) describes encode,
send, jitter, decode and output diagnostics; server protocol behavior is unchanged.

Host microphone PCM/statistics checks, from the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -I esp32/main \
  esp32/main/microphone_pcm.c esp32/tests/microphone_pcm_test.c -lm \
  -o /tmp/digits-microphone-pcm-test
/tmp/digits-microphone-pcm-test
```

The tests cover signed extremes, independent slot selection, averaging,
in-place conversion, RMS/dBFS, clipping and statistics across block boundaries.
Keep running the Ring Test PCM/deadline checks below after shared audio changes.

Host capture-worker regression:

```sh
python3 esp32/tests/microphone_capture_test.py
```

This compiles the actual capture worker with fake I2S/copy queues. It checks
sample alignment across partial timeout reads, discarding a frame spanning
a lost DMA block, bounded oldest-frame drops and diagnostics-off streaming.
It does not simulate ADC hardware or establish an acoustic result.

## Flash and test Ring Test

The existing Wi-Fi/server `sdkconfig` is reused. From the repo root:

```sh
export IDF_TOOLS_PATH=/private/tmp/digits-idf-tools
. /private/tmp/digits-esp-idf/export.sh
cd esp32
idf.py build
idf.py -p /dev/cu.usbmodem21122101 flash monitor
```

Use your current serial port if its name changes. Regular flash preserves
the existing UUID, number and device token; no partition changes are needed.
Exit monitor with Ctrl+].

1. Verify `Speaker ready` and paired `register sent` in the console.
2. Open the paired line/device detail page in the Digits WebUI. Select the
   ESP32 handset and wait for **Online**.
3. Click **Ring Test**. Expect a short, quiet tone followed by silence:

   ```text
   ring_test: Playing 1s speaker test (440+480 Hz, volume=50)
   ring_test: Speaker test stopped; amplifier disabled
   ```

4. Repeat after the button re-enables, and leave the monitor open for at least
   two server PINGs (30-second intervals). The device should remain online.
5. Reboot and confirm the saved identity/number register and Ring Test works
   again. Disconnect/reconnect server or Wi-Fi to check recovery.

If the button does nothing, check whether `/static/phone-detail.js` loads.
The existing `DEV_MODE=true` Docker setup previously served missing scripts.
This is independent of firmware. An unchanged v1.99.0 server can be tested
from the browser console **on the logged-in device detail page** using the
same authenticated endpoint:

```js
const number = document.querySelector('[data-phone-number]').dataset.phoneNumber;
const hardwareId = document.getElementById('operator-panel').dataset.hardwareId;
if (!number || !hardwareId) throw new Error('Select the ESP32 device detail page first');
fetch(`/phones/${encodeURIComponent(number)}/ring-test`, {
  method: 'POST',
  headers: {Accept: 'application/json'},
  body: new URLSearchParams({hardware_id: hardwareId})
}).then(async r => console.log(r.status, await r.json()));
```

Expected HTTP result: `200 {status: "triggered"}`. Check the firmware log and
speaker to confirm playback. No server changes are required by this client.

For a permanent fix with `DEV_MODE=true`, mount the exact upstream static
assets into the released container. Development mode serves files from
`/app/internal/web/static`, while the release image only contains `signald`.
Prepare the assets from the repo root:

```sh
git archive --format=tar.gz --output=/tmp/digits-v1.99.0-static.tar.gz \
  a4edaaa557c06b3b687be9e92a869fb0cbcf68fc server/internal/web/static
```

Copy/extract the archive on the **Docker host**, then add a read-only bind
mount to the existing signald service in Portainer/Compose. Replace the
source with the absolute path to the extracted static directory:

```yaml
volumes:
  - type: bind
    source: /absolute/path/on/docker-host/server/internal/web/static
    target: /app/internal/web/static
    read_only: true
```

Keep `DEV_MODE=true`, redeploy the container, verify
`/static/phone-detail.js` returns HTTP 200, and force-reload the browser.
`DEV_STATIC_DIR` is not an environment setting in v1.99.0. This repair uses
upstream files and container configuration; no server source changes.

Historical Ring Test validation, before the 48 kHz microphone/shared-audio
change: on 2026-10-08 the connected Waveshare board reported
`Speaker ready`. A direct authenticated Ring Test request to the unchanged
v1.99.0 server returned HTTP 200 `{"status":"triggered"}`; the user confirmed
an audible tone and Ring Test serial logs. The WebUI script still returned
404 at that point; long-running heartbeat and recovery tests remain the
separate checks listed above.

The final-block timeout fix was subsequently flashed to that same board.
Its ELF hash prefix was `95a7f6cf9`; reboot retained the same UUID, number
and token-backed registration. Three separate Ring Tests completed with
normal stop logs and no playback errors, while six server PINGs were answered
over about three minutes. The host regression reproduced the original
990 ms timeout and passed all 70 cases after the fix. Extended soak and
network interruption/recovery checks remain separate acceptance tests.

The user subsequently applied the static-directory bind and confirmed the
WebUI Ring Test button works. These earlier hardware results exercised the
16 kHz speaker-only firmware. The user has also confirmed an audible Ring Test
on the later 48 kHz KEY1 microphone firmware while investigating quiet saved
recordings. Later testing on 2026-10-11 exposed repeated `digits_ring` stack
overflows and resets after seven successful WebRTC calls, as recorded in the
[live hardware results](#verification-and-remaining-limits). Those earlier
audible tests do not establish current Ring Test reliability. Repeat Ring Test
and heartbeat checks on the corrected firmware now flashed. Fifteen subsequent
Ring Tests completed without a stack panic, and the user confirmed consistent
audible tones. A subsequent post-call check on `808a295d05` also confirmed
Ring Test and KEY1 recording/playback worked as usual, with normal capture
resume, as recorded in the [call-audio recovery results](../docs/esp32-call-audio.md#pion-originated-full-duplex-and-diagnostic-exclusion).

Host PCM checks (no ESP-IDF needed), from the repo root:

```sh
cc -std=c11 -Wall -Wextra -Werror -I esp32/main \
  esp32/main/ring_tone.c esp32/tests/ring_tone_test.c -lm \
  -o /tmp/digits-ring-tone-test
/tmp/digits-ring-tone-test
```

Host DMA/deadline regression (requires Python 3 and a host C compiler):

```sh
python3 esp32/tests/ring_deadline_test.py --stack-usage
```

This compiles the actual worker bodies with a 100 Hz scheduler/DMA model,
sweeps 70 DMA timing phases/setup delays and 40 producer-preemption cases, and
checks that genuine mid-playback stalls still fail while ordinary final
completion stops without an error. It also checks overlap coalescing and
subsequent playback. With the static PCM block, the host compiler reports a
160-byte worker frame versus 2,096 bytes previously. This measures only the
worker's own host stack frame; it does not establish nested driver stack use,
physical I2C timing or replace the board test above.

## Expected console sequence

Example only; timestamps, UUID, code and number vary:

```text
credentials: Generated persistent hardware_id=12345678-1234-4234-8234-123456789abc
digits: Boot hardware_id=12345678-1234-4234-8234-123456789abc number=unpaired
wifi: Station has IP address
signaling: WebSocket connected
signaling: register sent: number=unpaired hardware_id=... token=absent (no register acknowledgement)
signaling: PAIRING CODE: 001234 (TTL: 600 seconds)
signaling: Server PING received; transport replies with PONG
```

Claim the code in the existing Digits UI. Then:

```text
signaling: paired persisted: number=3140001; reconnecting
signaling: WebSocket connected
signaling: register sent: number=3140001 hardware_id=... token=present (no register acknowledgement)
signaling: Ignoring message type=line_settings
signaling: Server PING received; transport replies with PONG
```

Subsequent boot prints the same UUID and saved number, without generating a
new identity or requesting pairing. A number change logs
`line_renumber persisted: number=...; reconnecting`. Network failures log
`WebSocket disconnected` followed by `Retry in ... seconds`. Rejected tokens
log `Server error: invalid device_token` and are retained for diagnosis.
If an unpaired code expires, reboot to reconnect and request a new code;
automatic pre-expiry code refresh is outside this milestone.

## Hardware validation

After configuring real credentials, flash the board and check:

1. Pair through the existing UI, then verify the device appears online.
2. Leave it idle for several minutes to verify repeated server PING/PONG.
3. Reboot and confirm the same identity and saved number register.
4. Renumber through the UI and verify persistence after another reboot.
5. Interrupt Wi-Fi/server availability and verify bounded retries and recovery.

A compiler build does not establish these hardware/network acceptance results.
