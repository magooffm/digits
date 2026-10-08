# AGENTS.md

## Project: Digits ESP32-S3 Telephone

This repository is a fork of Justin Lindh's Digits project.

The goal of this fork is to develop a simplified physical telephone client
based on an ESP32-S3 while keeping the existing Digits server (`signald`)
as compatible with upstream as possible.

Before making architectural changes, inspect the existing upstream
implementation and treat it as the reference for protocol behavior.

---

## 1. Project Goal

Build three physical telephones that can communicate over the Internet using
the existing Digits infrastructure.

Each telephone represents one participant.

The physical user interface is deliberately simpler than the original Digits
telephone.

Each phone should have:

- one telephone handset
- one hook switch
- two dedicated call buttons for the other two participants
- optionally one "All" button for a group/party-line call
- optional status LEDs
- no numeric keypad
- no display required

The intended interaction is approximately:

1. Lift handset.
2. Press a participant button.
3. Remote telephone rings.
4. Remote user lifts handset.
5. Audio connection is established.
6. Hanging up terminates the call.

Incoming calls should ring while the handset is on-hook.

---

## 2. Hardware Direction

### Main controller

Replace the original Raspberry Pi Zero 2 W + Raspberry Pi Pico architecture
with a single ESP32-S3.

Current development board:

Waveshare ESP32-S3-AUDIO-Board

Relevant hardware includes:

- ESP32-S3R8
- 8 MB PSRAM
- 16 MB flash
- Wi-Fi
- ES8311 audio DAC/codec
- ES7210 ADC
- NS4150B speaker amplifier
- onboard microphones
- physical buttons
- I2S audio interfaces

Use ESP-IDF as the primary development framework.

Do not assume Arduino compatibility is a project requirement.

---

## 3. Telephone Handset

The intended handset is:

Bosch / Blaupunkt TH-07
Part number: 7607 570 512

The handset contains an earpiece and microphone connected through a small
multi-pin connector.

IMPORTANT:

The handset pinout has NOT yet been established.

Do not infer electrical function from wire colors.

Before connecting the handset to the ESP32 audio hardware:

1. identify connector pins
2. measure resistance between candidate earpiece pins
3. identify microphone pins
4. determine microphone type and required bias
5. determine suitable audio levels and impedance

Do not apply arbitrary voltages to the handset.

The original Digits handset implementation used a telephone receiver of
approximately 140 ohms connected to a codec line output. That does not prove
that the TH-07 has the same electrical characteristics.

---

## 4. Audio Architecture

Target architecture:

TH-07 handset
    <->
audio codec / analog interface
    <-I2S->
ESP32-S3
    <->
WebRTC audio

The Waveshare board's NS4150B amplified speaker output may not be appropriate
for the telephone earpiece.

Investigate whether the ES8311 differential output should be accessed before
the NS4150B amplifier.

Likewise, investigate how to connect the handset microphone to an available
ES7210 microphone input.

Do not finalize the analog interface until the TH-07 has been measured.

---

## 5. Buttons and Hook Switch

Do not reproduce the original numeric keypad matrix.

Use direct GPIO inputs.

Logical inputs:

- CALL_A
- CALL_B
- CALL_ALL (optional)
- HOOK

The Waveshare board's existing buttons may be used during early development.

Production hardware may use external physical buttons.

Use appropriate GPIO pull-ups/pull-downs and software debouncing.

---

## 6. Software Architecture

The original Digits client runs on Linux/Raspberry Pi and uses Pion WebRTC.

Do NOT attempt to port the Go/Pion client directly to the ESP32.

The ESP32 implementation should use Espressif's native WebRTC components,
including ESP-WebRTC / esp_peer where appropriate.

Expected software stack:

ESP-IDF
    |
ESP-WebRTC / esp_peer
    |
Digits signaling compatibility
    |
signald

The existing Digits Pi implementation is the behavioral reference.

---

## 7. Server Compatibility

A major project goal is:

KEEP THE DIGITS SERVER UNMODIFIED IF POSSIBLE.

The ESP32 should adapt to the existing Digits server protocol rather than
changing the server to accommodate the ESP32.

Before implementing a protocol component:

1. inspect the existing server implementation
2. inspect the existing Pi/digitsd client
3. identify the actual message format and state transitions
4. document the behavior
5. implement the equivalent ESP32 behavior

Do not invent a parallel signaling protocol unless compatibility proves
impossible.

---

## 8. Current Development Server

A working Digits server is already running locally using Docker/Portainer.

Current server version:

signald v1.99.0

Container image:

ghcr.io/justinlindh/digits/signald:v1.99.0

PostgreSQL is used as the database.

The server currently runs on a local LAN without HTTPS.

For development:

DEV_MODE=true

The normal magic-link login is not usable over plain HTTP because the server
sets the `digits_session` cookie with the `Secure` attribute.

The development session endpoint works:

/auth/dev-session?email=dev@digits.local

This has successfully been used to access the UI and create a Family.

Do not remove `Secure` from production authentication merely to support the
development environment.

Production deployment should eventually use HTTPS.

---

## 9. SMTP

smtp4dev has been used during development to inspect Digits magic-link email.

This is development infrastructure only.

It is not part of the telephone architecture.

---

## 10. Networking / WebRTC

The desired media architecture is peer-to-peer WebRTC whenever possible.

Conceptually:

Phone A
   |
   | signaling
   v
signald
   ^
   | signaling
   |
Phone B


After negotiation:

Phone A <------ WebRTC audio ------> Phone B

The signaling server should not normally carry the audio stream.

WebRTC mechanisms involved may include:

- ICE
- STUN
- TURN
- DTLS
- SRTP
- Opus

STUN should be sufficient for many NAT configurations.

TURN may be added later for networks where direct ICE connectivity fails.

TURN is NOT required for the initial same-LAN development phase.

Do not add TURN complexity until basic WebRTC interoperability works.

---

## 11. Primary Development Milestone

The first major milestone is NOT the finished telephone.

It is:

ESP32-S3 successfully interoperates with an existing Digits client through
the unmodified Digits server.

A useful test configuration is:

Existing Digits/Pion client
          |
          |
       signald
          |
          |
      ESP32-S3

Success criteria:

1. ESP32 connects/provisions/registers as required by Digits.
2. ESP32 participates in Digits signaling.
3. ICE negotiation succeeds.
4. DTLS/SRTP session succeeds.
5. Opus audio can be exchanged.
6. Call setup and teardown work reliably.

Only after this milestone should significant effort go into final handset
electronics and enclosure design.

---

## 12. Suggested ESP32 Source Structure

A possible structure is:

esp32/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── app_main.c
    ├── phone.c
    ├── phone.h
    ├── buttons.c
    ├── buttons.h
    ├── hook.c
    ├── hook.h
    ├── audio.c
    ├── audio.h
    ├── signaling.c
    ├── signaling.h
    ├── webrtc.c
    ├── webrtc.h
    ├── provisioning.c
    └── provisioning.h

This is a proposed structure, not a requirement.

Prefer clear component boundaries over following this layout mechanically.

---

## 13. Phone State Machine

A likely high-level state model is:

ON_HOOK
    |
    | handset lifted
    v
READY
    |
    | participant button
    v
CALLING
    |
    v
RINGING / CONNECTING
    |
    v
CONNECTED
    |
    | handset replaced
    v
ON_HOOK

Incoming:

ON_HOOK
    |
    | incoming call
    v
INCOMING_RING
    |
    | handset lifted
    v
CONNECTED

The exact state machine should be refined after studying the existing Digits
call/signaling model.

Do not force this proposed state machine onto the protocol if upstream Digits
requires different semantics.

---

## 14. Party-Line / "All" Button

A third button may eventually call both other participants.

This feature is secondary.

Do not let group-call support complicate the first peer-to-peer implementation.

First establish reliable one-to-one calls.

Then investigate how Digits currently represents multi-party calls and whether
the existing server already provides suitable semantics.

---

## 15. Privacy

Consider providing a hardware-level microphone disconnect when the handset is
on-hook.

Software muting alone should not automatically be considered sufficient for
the final hardware design.

This is not required for the first development prototype.

---

## 16. Upstream Relationship

Keep the fork easy to synchronize with upstream Digits.

Recommended Git setup:

origin   = project fork
upstream = https://github.com/justinlindh/digits.git

Avoid unnecessary changes to existing upstream files.

Prefer adding the ESP32 implementation as a separate directory/component.

Before modifying existing server code, explain why the change is necessary.

---

## 17. Working Rules for Coding Agents

When working on this repository:

### Do

- inspect existing code before implementing behavior
- use the upstream Pi client as the protocol reference
- use the upstream signald implementation as the server reference
- distinguish verified behavior from assumptions
- cite relevant source files/functions when explaining protocol behavior
- keep changes small and reviewable
- preserve upstream compatibility
- favor ESP-IDF-native solutions
- document protocol discoveries
- compile/test after meaningful changes
- report uncertainties explicitly

### Do not

- redesign the Digits server without a demonstrated need
- invent signaling messages based on assumptions
- assume Pion APIs exist on ESP32
- assume the TH-07 electrical pinout
- connect unknown handset wires to powered outputs
- implement TURN before basic LAN WebRTC works
- implement group calls before one-to-one calls work
- make unrelated formatting/refactoring changes to upstream code

---

## 18. Research Before Implementation

Before writing the ESP32 signaling client, analyze the existing repository and
answer at least these questions:

1. How does a Digits physical phone identify itself?
2. How is a phone provisioned?
3. What credentials are stored on the phone?
4. How does the phone connect to signald?
5. Is signaling HTTP, WebSocket, or another protocol?
6. What authentication is required?
7. What messages represent:
   - online/offline state
   - outgoing call
   - incoming call
   - answer
   - hangup
   - WebRTC offer
   - WebRTC answer
   - ICE candidates
8. How are SDP offers/answers generated and exchanged?
9. Which codecs does the existing client negotiate?
10. What assumptions does the server make about the Pion client?
11. Which parts of the existing Pi implementation are hardware-specific?
12. Which parts can be reproduced directly on ESP32?
13. Does the protocol depend on any server behavior not documented externally?

Produce a protocol/architecture note before implementing the complete client.

---

## 19. Immediate Next Task

The immediate task for the coding agent is:

DO NOT begin by rewriting the client.

First inspect the repository, especially the server and existing Pi client.

Produce a concise architecture report covering:

- repository components relevant to a physical phone
- provisioning flow
- authentication
- signaling transport
- signaling messages
- WebRTC setup
- audio codec configuration
- call lifecycle
- dependencies that cannot run on ESP32
- components that need to be reimplemented
- likely compatibility risks

For every important conclusion, reference the relevant source file and function
or type.

After the analysis, propose the smallest first ESP32 milestone.

Wait for review before performing large architectural changes.