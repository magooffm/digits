# Digits ESP32 signaling and Ring Test

ESP-IDF **v5.5.3**, target **esp32s3**, Waveshare ESP32-S3-AUDIO-Board
(ESP32-S3R8, 16 MB flash). This firmware connects Wi-Fi, registers with
Digits, pairs, reconnects, and plays the existing device Ring Test through
the integrated speaker. PSRAM and calling are unused. Microphone input,
WebRTC, buttons, hook handling and handset wiring are outside this milestone.
The console defaults to the ESP32-S3's native USB Serial/JTAG port.

## Configure, build, flash and monitor

With ESP-IDF v5.5.3 installed:

```sh
. /path/to/esp-idf/export.sh
cd esp32
idf.py set-target esp32s3
idf.py menuconfig
# Digits development configuration:
#   Wi-Fi SSID
#   Wi-Fi password
#   Digits WebSocket URL: ws://192.168.1.50:8080/ws (use your LAN server)
idf.py build
idf.py -p /dev/cu.usbmodemYOUR_DEVICE flash monitor
```

Exit the monitor with Ctrl+]. Linux ports are usually `/dev/ttyACM0`.
If using a USB-UART bridge instead of native USB, select UART under
Component config -> ESP System Settings -> Channel for console output.
Empty or oversized Wi-Fi settings and URLs other than `ws://.../ws` fail fast.

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

## Persistent storage

Default NVS partition, namespace `digits`:

| Key | NVS type | Schema |
| --- | --- | --- |
| `hardware_id` | string | Lowercase UUID v4, 36 characters plus NUL. Generated with `esp_fill_random` after Wi-Fi starts, committed before registration. |
| `pairing` | blob | 98 bytes: version byte `1`, 32-byte NUL-terminated number, 65-byte NUL-terminated original token. Both strings are saved as a single NVS value, then committed. |

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
with a 3-second send timeout. It is the only application message this
milestone sends. No register-success acknowledgement exists, so a successful
send log does not claim authentication success.

`pairing_code` is retained as a JSON string, preserving leading zeros.
`paired` commits number and token before requesting a reconnect outside the
WebSocket event callback. `line_renumber` commits the changed number while
retaining the token; an unchanged number does not cause a reconnect loop.
`error` logs the server's exact error string without changing credentials.
`ring_test` queues a one-second speaker test without replying to the server.
Other message types are logged and ignored; this firmware cannot answer calls.

Espressif `esp_websocket_client` **1.6.1** is pinned in the manifest. Its receive
path automatically echoes incoming PING payloads as PONG control frames.
Application code must not send JSON pong messages or duplicate that reply.
The 1024-byte receive buffer exceeds the RFC maximum control payload of 125
bytes. Outbound heartbeat uses 30-second pings and a 15-second pong timeout.
Server heartbeats use 30-second pings and a 45-second read deadline.
Text messages are assembled across receive-buffer chunks and continuation
frames up to 4096 bytes; control frames can interrupt assembly.

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
| I2S DAC data | GPIO16 (TX only; no microphone RX channel) |
| NS4150B PA_CTRL | TCA9555 address `0x20`, port 1 bit 0 (`P10` / `Extend_IO8`), high enables |
| Speaker | ES8311 differential output -> NS4150B -> H3 speaker connector |

The ESP32 is I2S master and ES8311 is slave: Philips I2S, signed 16-bit,
two identical channels, 16 kHz, MCLK 256 x sample rate = 4.096 MHz.
Espressif `esp_codec_dev` **1.6.2** initializes only the ES8311 DAC path.
The expander writes preserve every other pin; PA is latched low before
configuring its direction. No ES7210 or microphone input is initialized.

The initial DAC volume is **50/100** and the generated PCM peak is capped at
25% of full scale. Short fades reduce clicks. This is an intentionally low
starting level; actual loudness must be assessed on the board. The amplifier
is disabled and DAC muted while idle. DMA auto-clear prevents a stale buffer
from repeating after playback. Codec initialization failure is logged and
registration/pairing continue; a test then logs that audio is unavailable.

`digits_ring_test_request` only overwrites a one-element queue without waiting.
A separate FreeRTOS task owns codec/amp control and finite-timeout I2S writes.
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

Official hardware/software references:

- [Waveshare v1.1 schematic](https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board_1.1.pdf)
- [Waveshare ESP-IDF demos and explanations](https://docs.waveshare.com/ESP32-S3-AUDIO-Board/ESP-IDF)
- [Espressif v5.5.3 ES8311 I2S example](https://github.com/espressif/esp-idf/tree/v5.5.3/examples/peripherals/i2s/i2s_codec/i2s_es8311)
- [Espressif esp_codec_dev 1.6.2](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2/readme)

Waveshare's ESP-IDF documentation was inspected, but the board's full demo ZIP
could not be downloaded because download approval was cancelled. Its exact
implementation was not verified; the native Espressif example and schematic
are the implementation references above.

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

Hardware check on 2026-10-08: the connected Waveshare board reported
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

Host PCM checks (no ESP-IDF needed), from the repo root:

```sh
cc -std=c11 -Wall -Wextra -Werror -I esp32/main \
  esp32/main/ring_tone.c esp32/tests/ring_tone_test.c -lm \
  -o /tmp/digits-ring-tone-test
/tmp/digits-ring-tone-test
```

Host DMA/deadline regression (requires Python 3 and a host C compiler):

```sh
python3 esp32/tests/ring_deadline_test.py
```

This compiles the actual worker bodies with a 100 Hz scheduler/DMA model,
sweeps 70 timing phases/setup delays, and checks that genuine mid-playback
stalls still fail while ordinary final completion stops without an error.
It also checks overlap coalescing and subsequent playback. It does not
establish physical I2C timing or replace the board test above.

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
