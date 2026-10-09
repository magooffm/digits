# Digits ESP32 signaling and audio

ESP-IDF **v5.5.3**, target **esp32s3**, Waveshare ESP32-S3-AUDIO-Board
(ESP32-S3R8, 16 MB flash). This firmware connects Wi-Fi, registers with
Digits, pairs, reconnects, plays the existing device Ring Test through
the integrated speaker, and captures the two onboard microphones through
the ES7210 ADC. Capture supplies reusable 48 kHz signed 16-bit mono PCM;
an optional development diagnostic reports levels over serial. Optional saved
audio tests either record two seconds after boot or record while KEY1 is held
(up to ten seconds), then play the recording after release.
WebRTC, Opus, calls, call buttons, hook handling and handset wiring are unused.
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
writes while holding the shared speaker reservation. The optional microphone
record/playback worker uses that same reservation to keep playback exclusive.
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
#     Make RAM allocatable using heap_caps_malloc(..., MALLOC_CAP_SPIRAM)
#   SPI RAM config -> Ignore PSRAM when not found: enabled
# Digits development configuration -> Onboard microphone:
#   Microphone record/playback diagnostic:
#     Run a 2-second record-then-playback test once after boot
#   Enable microphone level diagnostic at boot: enabled (optional)
#   ES7210 microphone gain (dB): 24
idf.py build
idf.py -p /dev/cu.usbmodem21122101 flash monitor
```

Use the currently connected port if its name changes. The PSRAM access option
above keeps ordinary allocations in internal memory; the recording explicitly
uses `heap_caps_malloc` with `MALLOC_CAP_SPIRAM`. The alternative "Make RAM
allocatable using malloc() as well" also satisfies the test's dependency.
"Ignore PSRAM when not found" allows signaling and normal capture to boot
even if PSRAM initialization fails; the recording test then logs a missing
buffer and skips playback. Configuration and a regular flash preserve the
paired UUID, number and original device token. No partition change or erase
is required.

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
before. The diagnostic is the sole PCM consumer while it runs; a future Opus
consumer should run with this development test disabled.

Recording gaps, read failures, a failed ADC mute or speaker write failure abort
the test. An incomplete recording is not played. Cleanup gates off the
amplifier and requests capture resume after any attempted pause, including a
pause timeout. Treat `Capture resume failed` as a failed test and investigate
before relying on capture. The diagnostic never modifies NVS or sends a new
Digits message. Playback of speech on physical hardware remains to be verified.

### Optional KEY1 hold-to-record, release-to-play test

Select `Hold KEY1 to record; release to play (maximum 10 seconds)` in the
same menuconfig choice to enable `CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK`.
Reuse the Octal PSRAM, 40 MHz and `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
configuration [above](#optional-two-second-record-then-playback-test); this
mode has the same PSRAM dependency and leaves the boot test disabled.
From the repository root:

```sh
export IDF_TOOLS_PATH=/private/tmp/digits-idf-tools
. /private/tmp/digits-esp-idf/export.sh
cd esp32
idf.py menuconfig
# Component config -> ESP PSRAM:
#   Reuse the Octal / 40 MHz / heap_caps_malloc configuration above.
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
metrics and safe cleanup. The revised firmware requires flashing and acoustic
acceptance; no audible saved-speech result is claimed for it yet. For board acceptance, check
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
mono PCM; no automatic gain control is added to capture or the future Opus
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

### Feeding a future Opus encoder

`microphone.h` exposes a reusable single-consumer API:

```c
digits_microphone_frame_t frame;
esp_err_t err = digits_microphone_read(&frame, 100);
if (err == ESP_OK) {
    // frame.pcm contains 960 signed 16-bit mono samples: 20 ms at 48 kHz.
    // Check frame.sequence and DIGITS_MIC_FRAME_DISCONTINUITY before encoding.
    // Feed a future 48 kHz mono Opus encoder here, in its own task.
}
```

`digits_microphone_read()` copies a frame into caller-owned storage. A zero
timeout polls; waits are limited to 1,000 ms. Consumers never own or retain a
DMA pointer. Every frame contains `sequence`, `flags`, `captured_at_us` and
1,920 bytes of PCM. The timestamp is RX read-completion time, not a calibrated
ADC timestamp. The first published sequence is 1; sequence gaps and
`DIGITS_MIC_FRAME_DISCONTINUITY` identify dropped/lost audio. A future encoder
must handle those gaps explicitly instead of joining samples across them as
continuous audio. The queue is intentionally finite even when no consumer
exists yet. Saved-recording diagnostic playback gain does not alter this PCM
API or add capture AGC. No encoding, RTP, WebRTC or server protocol change is
implemented.

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
recordings. Re-run Ring Test and heartbeat checks after the playback gain
adjustment; its saved-speech acoustic result remains to be verified.

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
