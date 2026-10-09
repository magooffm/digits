# Headless Digits/Pion interoperability client

This Fedora/macOS command-line handset uses the unchanged Digits `server/v1.99.0`
protocol at **`a4edaaa557c06b3b687be9e92a869fb0cbcf68fc`**. It pairs as an
independent device, makes or manually answers one-to-one calls, generates Opus
audio and decodes received RTP audio without microphones, speakers, ALSA or Pico
hardware. Existing server, Pi client and ESP32 sources are unchanged by this tool.

The local module imports the original
[signaling Client](../../pi/digitsd/internal/signal/client.go),
[Message types](../../pi/digitsd/internal/signal/protocol.go),
[Pion PeerManager](../../pi/digitsd/internal/webrtc/peer.go),
[ICE configuration](../../pi/digitsd/internal/webrtc/ice.go) and
[Opus codec](../../pi/digitsd/internal/codec/opus.go). Its module name is beneath
the original Pi module, so Go's `internal` package rules permit these imports;
`go.mod` resolves that module through `replace ... => ../../pi/digitsd`.
Keep the full repository checkout rather than copying this directory alone.
The inspected server and Pi trees have an empty diff against `a4edaaa`.
Before updating those upstream packages, check their drift from this reference:

```sh
# From the repository root; no output means the reference still matches.
git diff a4edaaa -- server pi/digitsd
```

## Build requirements

Requirements are Go **1.26.1 or newer**, a C compiler, `pkg-config` and libopus.
CGO must be enabled. The versions used for macOS validation are Go **1.26.1**,
Pion **4.2.20** and libopus **1.5.2**. Pion and Go dependencies are pinned in
`go.mod`/`go.sum`; distribution package versions may be newer.

`make` applies the upstream Opus wrapper's `nolibopusfile` build tag, omitting
Ogg/file decoding; only libopus is required. The equivalent build is:

```sh
CGO_ENABLED=1 go build -tags nolibopusfile -o bin/digits-pion .
```

## Build on Fedora

Fedora 43 and 44 are supported targets. Install the native compiler, Go and
[Opus development package](https://packages.fedoraproject.org/pkgs/opus/opus-devel/):

```sh
sudo dnf install golang gcc make pkgconf-pkg-config opus-devel
# From the full repository checkout:
cd tools/interop
export CGO_ENABLED=1
go version
pkg-config --modversion opus
make build
make test
make race
make vet
```

Check that Go reports **1.26.1 or newer**; current
[Fedora Go packages](https://packages.fedoraproject.org/pkgs/golang/golang/)
satisfy this requirement. Fedora's
[pkgconf-pkg-config](https://packages.fedoraproject.org/pkgs/pkgconf/pkgconf-pkg-config/)
supplies the `pkg-config` executable and `opus-devel` supplies headers and the
native Opus library. The
[Go race detector](https://go.dev/doc/articles/race_detector) also requires CGO
and a C compiler on Linux; Linux amd64 and arm64 are supported.

Build on Fedora itself: the macOS binary and its `.dylib` cannot run on Linux.
The resulting Linux binary links against Fedora's libopus; its runtime package
is [opus](https://packages.fedoraproject.org/pkgs/opus/opus/), installed as a
dependency of `opus-devel`. No ALSA, PipeWire, audio
device access or Raspberry Pi packages are required. Continue with the shared
pairing and two-direction test procedure below:

```sh
./bin/digits-pion --server ws://SERVER_HOST:8080/ws \
  --state state/pion.json --host-only --trace-dir traces
```

Replace the host and port with your signald address. Native execution makes the
development machine's LAN addresses available to ICE without container NAT.

## Build on macOS

If Command Line Tools are missing, run `xcode-select --install` and finish the
installer. With Homebrew installed:

```sh
brew install go pkgconf opus
# From the full repository checkout:
cd tools/interop
export CGO_ENABLED=1
go version
pkg-config --modversion opus
make build
make test
make race
make vet
```

If Homebrew's pkg-config search path needs configuring:

```sh
export PKG_CONFIG_PATH="$(brew --prefix opus)/lib/pkgconfig"
```

For the toolchain and native library already prepared in this development
workspace, these commands select the validated versions without Homebrew:

```sh
export PATH="/private/tmp/digits-go-toolchain/go/bin:$PATH"
export PKG_CONFIG=/private/tmp/digits-client-native/bin/pkgconf
export PKG_CONFIG_PATH=/private/tmp/digits-client-native/lib/pkgconfig
export PKG_CONFIG_ALLOW_SYSTEM_LIBS=1
export CGO_LDFLAGS="-Wl,-rpath,/private/tmp/digits-client-native/lib"
export GOMODCACHE=/private/tmp/digits-go-modcache
export GOCACHE=/private/tmp/digits-go-buildcache
export CGO_ENABLED=1
cd /Users/marcomhopp/Documents/GitHub/digits/tools/interop
go version
"$PKG_CONFIG" --modversion opus
make build
make test
make race
make vet
```

These `/private/tmp` installations are local development artifacts and may need
reinstalling after cleanup. The linker rpath resolves their libopus dylib.

## Run and pair

Use your running signald address and port:

```sh
export DIGITS_SERVER_URL='ws://SERVER_HOST:8080/ws'
./bin/digits-pion --state state/pion.json --host-only --trace-dir traces
```

Replace `SERVER_HOST` and the example port. `--host-only` makes the first LAN
test use host candidates without STUN/TURN delays. For ordinary server-supplied
STUN configuration, omit that flag; TURN URLs are filtered for this milestone.
`wss://host[:port]/ws` is also supported, using normal TLS verification.

First run creates a random UUID v4 and saves it immediately. The serial-style
CLI prints `PAIRING CODE`, its string value, and `ttl_seconds`; leading zeroes
are retained. In the Digits WebUI choose **Pair a new handset**, enter that code
and the required fields, and assign the new handset to the same family as the
ESP32. On `paired`, the client atomically saves the number and original token
before reconnecting. There is no successful `register` acknowledgement; confirm
the handset is **Online** in the WebUI. Use `status` to see its assigned number.

The state file's schema is:

```json
{
  "version": 1,
  "hardware_id": "UUID-v4",
  "number": "unpaired",
  "device_token": ""
}
```

Pairing replaces `number` and `device_token` together; `line_renumber` updates
only the number. Files are `0600`, their containing directory is `0700`, and a
process lock prevents simultaneous reuse of the same state file. Writes use
temporary-file sync, atomic rename and directory sync. Invalid existing state
is reported rather than replaced with a new identity.

Without `--state`, Fedora/Linux uses
`~/.config/digits-interop/device.json`, or
`$XDG_CONFIG_HOME/digits-interop/device.json` when `XDG_CONFIG_HOME` is set.
macOS uses `~/Library/Application Support/digits-interop/device.json`. These
defaults follow [Go's `os.UserConfigDir`](https://pkg.go.dev/os#UserConfigDir).
`state/`, `traces/` and `bin/` in this directory are ignored by Git. Each
independent client needs its own state file and pairing; do not reuse the
ESP32's UUID or token.

Flags:

| Flag | Purpose |
| --- | --- |
| `--server URL` | WebSocket endpoint; also `DIGITS_SERVER_URL` |
| `--state PATH` | Persistent independent device identity |
| `--audio tone\|silence` | Quiet 440 Hz tone by default, or Opus-encoded silence |
| `--host-only` | Ignore server ICE servers for same-LAN tests |
| `--trace-dir DIR` | Private per-call SDP and bare ICE files; disabled by default |
| `--hardware-id UUID --number NUMBER --device-token TOKEN` | Import this client's existing paired identity into a **new** state file |

`DIGITS_DEVICE_TOKEN` may supply the import token. Existing state accepts explicit
credential flags only when they match its identity. The original 64-character
hexadecimal token is persisted unchanged and is never logged or hashed. Trace
files contain raw SDP/ICE, including network addresses and ICE credentials,
without device tokens; the trace directory is `0700`, files are `0600`.

Interactive commands:

```text
call NUMBER
answer
hangup
status
reconnect
help
quit
```

An incoming call waits for `answer`. Calling requires paired online signaling
and an idle client. `hangup` releases the peer and media workers. `reconnect`
ends any call and opens a fresh registered WebSocket; `quit` or Ctrl+C exits.

## Wire and media contract

The original `signal.Client.Connect()` sends `register` as the first WebSocket
message and handles server control-frame PING/PONG in its reader. Unpaired
registration uses `number: "unpaired"` plus the saved hardware ID. Paired
registration includes the saved number and original token.

Outgoing call order is `call` with `to`, then `sdp` with `to` and raw offer SDP,
then `ice` messages. Incoming `ring`/`sdp` are retained for manual acceptance;
acceptance sends `answer` with raw answer SDP before local ICE. `candidate` is
the bare `candidate:...` string, without an `a=` prefix, MID, m-line index or
JSON candidate object. Early remote candidates wait until remote SDP is applied.
Espressif candidates already embedded in SDP are accepted by Pion unchanged.

Pion keeps the original client's codecs, ICE/DTLS/SRTP defaults, Opus track and
RTCP draining. PCM is signed 16-bit, **48 kHz mono**, **960 samples/20 ms**;
the original encoder uses VoIP mode, **24 kbit/s**, complexity **5**, FEC and
DTX. Opus SDP advertises `opus/48000/2`, including for mono PCM. Tone peak is
2048/32768 (approximately -24 dBFS). Received Opus is decoded and counted,
then discarded; no system sound device is opened.

## Reproduce an ESP32 hardware test

The ESP32 must run the previously prepared WebRTC firmware, with its WebRTC
peer and USB console enabled and Wi-Fi/server settings configured. If it has
not been flashed yet, install ESP-IDF **5.5.3** and configure the firmware using
the [ESP32 setup instructions](../../esp32/README.md). Select your installed
ESP-IDF environment and the board's serial port, then build, flash and monitor:

```sh
source /path/to/esp-idf/export.sh
cd /path/to/digits/esp32
idf.py -p /dev/ttyACM0 build flash monitor
```

Replace both paths with your installation and checkout. `/dev/ttyACM0` is an
example Linux USB Serial/JTAG port; use the actual device, including
`/dev/serial/by-id/...` when available. On macOS use the board's
`/dev/cu.usbmodem...` port instead. Close other serial monitors first. For the
already prepared KEY1 configuration, retain its existing build-directory and
SDK configuration selection:

```sh
idf.py -B build/button-record \
  -D SDKCONFIG="$PWD/build/button-record.sdkconfig" \
  -p /dev/ttyACM0 build flash monitor
```

This requires that local SDK configuration to exist; configure a fresh checkout
using the ESP32 setup instructions. Regular flashing preserves existing pairing.
Exit IDF monitor with Ctrl+].

Keep both devices on the same LAN and paired into the same authorized family.
Record **PION_NUMBER** from Pion `status` and **ESP_NUMBER** from ESP32
`webrtc status`; both devices must appear Online in Digits.

1. **Pion → ESP32:** enter `call ESP_NUMBER` in Pion. After the ESP32 logs an
   incoming ring/offer, enter `webrtc answer` in its USB console.
2. Wait 5–10 seconds. Enter Pion `status` and ESP32 `webrtc status`. Record the
   evidence listed below, then enter Pion `hangup`. Both ends must return idle.
3. **ESP32 → Pion:** enter `webrtc call PION_NUMBER` on ESP32. After Pion prints
   `Incoming call`, enter `answer` in Pion. Repeat the same status/counter checks,
   then enter ESP32 `webrtc hangup` and confirm both ends release their peer.
4. Repeat both directions at least three times. Test Pion `reconnect`, verify
   the same paired number returns Online, and repeat a call. Once idle, run the
   Digits WebUI Ring Test on ESP32 and confirm its existing speaker test works.

Success requires transport and media evidence, not only SDP exchange:

- Both peers reach **connected**; Pion reports ICE and DTLS connected.
- Pion logs `first authenticated/decrypted SRTP audio packet`, followed by
  increasing `rx_packets` and `decoded_samples`, `audio/opus` at 48 kHz, and
  zero encode/decode/write errors. ESP32's current silence packets decode to
  960 mono samples each; totals should advance accordingly.
- Pion `tx_frames` increases and ESP32 logs/counts received encoded Opus
  packets. This confirms both directions, rather than one endpoint merely
  transmitting to an unconfirmed receiver.
- Hangup closes the media workers and peer; subsequent calls work without
  retained call state. Capture ESP32 memory diagnostics before/after repeated
  calls to check resource release.

The ESP32 milestone currently counts received encoded audio and transmits
silence; it does not render the Pion tone or stream its microphone. No audible
call audio is expected, even when transport interoperability succeeds.

## Diagnose each stage separately

| Stage | Evidence and failure interpretation |
| --- | --- |
| Signaling | First-message registration, Online status, correct number/family and manual ring/answer. Exact server `error` messages are printed. |
| SDP | `offer created`, `remote offer applied` or `remote answer applied`; parsing/set-description errors carry stage `SDP`. Inspect private `.sdp` traces for Opus, ICE credentials, fingerprint and setup role. An applied answer alone proves no media connection. |
| ICE | A nominated succeeded candidate pair is separate ICE evidence; inspect bare `.candidate` traces and the selected pair. No nomination within the setup deadline suggests routing, address/candidate or firewall trouble. |
| DTLS | Pion aggregate `connected` requires successful ICE and DTLS. A nominated ICE pair with DTLS unconfirmed warrants Pion DTLS logs; use concrete handshake/fingerprint errors before assigning cause. |
| SRTP | Only successfully read/decrypted RTP confirms reception. Increasing `rx_packets` proves it; `tx_frames` only proves local sends. Connected with zero received packets does not establish a cryptographic failure. |
| Opus | Track codec, 48 kHz clock, increasing decoded samples and `decode_errors=0` confirm codec reception. Encoding, decoding and sample-write errors are counted separately. |

The unchanged PeerManager keeps its PeerConnection private. Its `GetStats()`
does not populate actual `DTLSState` or SRTP cipher fields, so this tool does
not present those zero values as transport measurements. It reports nominated
pair evidence, aggregate connected state and authenticated RTP separately.
Pion warnings are enabled by default. For underlying handshake diagnostics:

```sh
PION_LOG_DEBUG=all ./bin/digits-pion \
  --state state/pion.json --host-only --trace-dir traces
```

If ICE stalls on Fedora, inspect the LAN interface's active firewall zone and
any recent SELinux denials before assigning the failure to DTLS or SRTP:

```sh
firewall-cmd --state
firewall-cmd --get-active-zones
firewall-cmd --zone=YOUR_ACTIVE_ZONE --list-all
getenforce
sudo ausearch -i -m avc,user_avc -ts recent
```

Replace `YOUR_ACTIVE_ZONE` with the zone containing the LAN interface. These
commands inspect configuration and logs; they do not change firewall or SELinux
policy. Compare the selected ICE addresses/ports with routing and packet-flow
evidence. See Fedora's [firewalld documentation](https://fedoraproject.org/wiki/Firewalld)
and [SELinux debugging guide](https://fedoraproject.org/wiki/SELinux/Debugging).

The post-answer setup deadline is **10 seconds**, matching the original Pi
client; the local ringing bound is 60 seconds. Signaling loss ends the peer and
reconnects with a bounded 6/12/24/48/60-second backoff for unstable sessions.
Pion keeps its original IPv4/IPv6 candidate behavior while ESP32 is configured
for IPv4; candidate interoperability still needs hardware observation.
No TURN, ICE restart, signaling grace-period recovery, conference/group calls
or physical audio integration is added. The ESP32 upstream fingerprint
verification uncertainty remains documented in its
[compatibility note](../../docs/esp32-webrtc-interoperability.md).

## Verification status

On **2026-10-10**, a native **Fedora 44 ARM64 VM** passed `make build`,
`make test`, `make race`, `make vet` and the binary's `--help` smoke test.
Its distribution packages were Go **1.26.8**, GCC **16.2.1** and libopus
**1.6**, with CGO enabled and Pion **4.2.20** from the unchanged module lock.
The Linux binary resolved `libopus.so.0` from Fedora's `/lib64`.
The same full test suite verifies real local ICE/DTLS/SRTP and bidirectional
Opus, signaling fixtures, pairing persistence and credential permissions.
An interactive `status`/`quit` smoke test also verified Linux's XDG default
state path, `0600`/`0700` permissions and the same hardware ID on a second run.

The [interoperability CI workflow](../../.github/workflows/interop-ci.yml)
is configured to build and run the tests, race detector, vet and CLI help in
official Fedora **43/44 x86_64 containers**, using native Fedora Go/Opus
dependencies. It preserves the full checkout so the original Pi packages remain
available. This workflow has not been run yet; Fedora 43 and x86_64 runtime
verification remain pending CI. VM/container tests exercise local peers and do
not verify the development machine's LAN firewall, SELinux policy or a physical
ESP32 connection.

The macOS build, `go vet`, and all **28 top-level tests plus subcases** pass,
including the Go race detector. Tests exercise the real Pion/libopus peer
stack, local ICE/DTLS/SRTP, bidirectional Opus reception, signaling fixtures
and state persistence. Both silence/tone role combinations receive and decode
at least 12 packets per direction, with zero codec/write errors. The original
decoder also accepts 100 ESP32 `f8 ff fe` packets as 960 zero samples each.

No unexpected SDP, ICE, DTLS or SRTP failure remains in those host tests. The
negative test deliberately supplies an incorrect SDP fingerprint and observes
Pion's DTLS error `remote certificate does not match any fingerprint`, with no
connected state or media accepted. Depending on DTLS CloseNotify scheduling,
the aggregate connection can terminate as failed or closed.

These tests and compilation are distinct from a live ESP32 ↔ Pion call.
**Physical interoperability has not been demonstrated.**
Record both endpoint logs and per-stage results when performing the procedure
above; unresolved hardware failures must remain classified by their evidence.
