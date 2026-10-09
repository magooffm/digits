package main

import (
	"context"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/justinlindh/digits/pi/digitsd/internal/codec"
)

// This test uses real Pion ICE/DTLS/SRTP and the original Digits libopus codec.
// It validates two host peers, never an ESP32 or physical interoperability.
func TestCallPeerLocalBidirectional(t *testing.T) {
	for _, callerAudio := range []string{"silence", "tone"} {
		t.Run("caller-"+callerAudio, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
			defer cancel()
			type event struct {
				side int
				peerEvent
			}
			events := make(chan event, 128)
			var overflow atomic.Bool
			callback := func(side int) func(peerEvent) {
				return func(e peerEvent) {
					select {
					case events <- event{side, e}:
					case <-ctx.Done():
					default:
						overflow.Store(true)
					}
				}
			}
			caller, err := newCallPeer(ctx, nil, callerAudio, callback(0))
			if err != nil {
				t.Fatal(err)
			}
			calleeAudio := "tone"
			if callerAudio == "tone" {
				calleeAudio = "silence"
			}
			callee, err := newCallPeer(ctx, nil, calleeAudio, callback(1))
			if err != nil {
				caller.Close()
				t.Fatal(err)
			}
			defer caller.Close()
			defer callee.Close()
			peers := []*callPeer{caller, callee}
			caller.StartMedia()
			callee.StartMedia()
			if caller.Snapshot().TXFrames != 0 || callee.Snapshot().TXFrames != 0 {
				t.Fatal("media started before Connected")
			}
			offer, err := caller.Offer()
			if err != nil {
				t.Fatal(err)
			}
			// Exact upstream Pion defaults: one audio m-line, Opus SDP uses
			// /2 even though the actual PCM encoder is mono; RTP/RTCP mux.
			for _, required := range []string{"m=audio ", "opus/48000/2", "a=rtcp-mux", "a=fingerprint:", "a=ice-ufrag:", "a=setup:actpass"} {
				if !strings.Contains(offer, required) {
					t.Fatalf("offer missing %q", required)
				}
			}
			answer, err := callee.AcceptOffer(offer)
			if err != nil {
				t.Fatal(err)
			}
			if err := caller.SetAnswer(answer); err != nil {
				t.Fatal(err)
			}
			// The SDP transaction has completed before any gathered candidate
			// is delivered. The controller has a separate wire-order test.
			check := time.NewTicker(20 * time.Millisecond)
			defer check.Stop()
			candidates := [2]int{}
			for {
				select {
				case <-ctx.Done():
					t.Fatalf("host peer handshake/media timeout: caller=%+v callee=%+v", caller.Snapshot(), callee.Snapshot())
				case e := <-events:
					switch e.Kind {
					case "candidate":
						if !strings.HasPrefix(e.Text, "candidate:") {
							t.Fatalf("not a bare ICE candidate: %q", e.Text)
						}
						candidates[e.side]++
						if err := peers[1-e.side].AddICE(e.Text); err != nil {
							t.Fatal(err)
						}
					case "state":
						if e.Text == "connected" {
							peers[e.side].StartMedia()
							peers[e.side].StartMedia() // idempotent worker start
						}
						if e.Text == "failed" {
							t.Fatalf("host connection failed: %+v", peers[e.side].Snapshot())
						}
					case "failure":
						t.Fatal(e.Err)
					}
				case <-check.C:
					left, right := caller.Snapshot(), callee.Snapshot()
					if left.RXPackets < 12 || right.RXPackets < 12 || left.DecodedSamples < 12*960 || right.DecodedSamples < 12*960 || left.TXFrames < 12 || right.TXFrames < 12 {
						continue
					}
					for i, snapshot := range []peerSnapshot{left, right} {
						if snapshot.Connection != "connected" || snapshot.ICE != "connected" || snapshot.DTLS != "connected" || snapshot.SRTP != "receiving" {
							t.Fatalf("peer %d transport not verified: %+v", i, snapshot)
						}
						if snapshot.Codec != "audio/opus" || snapshot.ClockRate != 48000 || snapshot.DecodedSamples < 12*960 {
							t.Fatalf("peer %d codec/media not verified: %+v", i, snapshot)
						}
						if snapshot.EncodeErrors != 0 || snapshot.DecodeErrors != 0 || snapshot.WriteErrors != 0 || snapshot.TXFrames < 12 || candidates[i] == 0 {
							t.Fatalf("peer %d media errors or missing transmit/candidates: %+v", i, snapshot)
						}
					}
					if overflow.Load() {
						t.Fatal("peer callbacks overflowed bounded host event queue")
					}
					t.Logf("verified host ICE/DTLS/SRTP/Opus: caller=%+v callee=%+v", left, right)
					// Close both peers while media is running and verify the owned
					// workers exit. Double-close exercises cancellation idempotence.
					closed := make(chan struct{})
					go func() {
						caller.Close()
						callee.Close()
						caller.Close()
						callee.Close()
						close(closed)
					}()
					select {
					case <-closed:
					case <-time.After(2 * time.Second):
						t.Fatal("host peer media workers did not close within 2s")
					}
					return
				}
			}
		})
	}
}

func TestCallPeerRejectsMalformedSDPAndCandidate(t *testing.T) {
	peer, err := newCallPeer(context.Background(), nil, "silence", nil)
	if err != nil {
		t.Fatal(err)
	}
	defer peer.Close()
	if _, err := peer.AcceptOffer("not SDP"); err == nil || !strings.Contains(err.Error(), "SDP") {
		t.Fatalf("malformed SDP did not produce an SDP diagnostic: %v", err)
	}
	for _, candidate := range []string{`{"candidate":"candidate:1"}`, "a=candidate:1", "candidate:bad"} {
		if err := peer.AddICE(candidate); err == nil || !strings.Contains(err.Error(), "ICE") {
			t.Fatalf("malformed candidate did not produce an ICE diagnostic: %q %v", candidate, err)
		}
	}
}

func TestCallPeerRejectsUnknownSource(t *testing.T) {
	if _, err := newCallPeer(context.Background(), nil, "microphone", nil); err == nil {
		t.Fatal("unsupported audio source accepted")
	}
}

func TestOriginalCodecDecodesESP32Silence(t *testing.T) {
	decoder, err := codec.NewDecoder(48000, 1)
	if err != nil {
		t.Fatal(err)
	}
	// This is the exact pre-encoded 20 ms Opus packet used by the current
	// ESP32 interoperability firmware, not its future microphone encoder.
	for i := 0; i < 100; i++ {
		pcm, err := decoder.Decode([]byte{0xf8, 0xff, 0xfe})
		if err != nil {
			t.Fatal(err)
		}
		if len(pcm) != codec.FrameSize {
			t.Fatalf("ESP32 silence decoded to %d samples, expected 960", len(pcm))
		}
		for _, sample := range pcm {
			if sample != 0 {
				t.Fatal("ESP32 silence did not decode to zero PCM")
			}
		}
	}
}

func TestCallPeerDoesNotAcceptWrongDTLSFingerprint(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	type event struct {
		side int
		peerEvent
	}
	events := make(chan event, 128)
	callback := func(side int) func(peerEvent) {
		return func(e peerEvent) {
			select {
			case events <- event{side, e}:
			case <-ctx.Done():
			}
		}
	}
	caller, err := newCallPeer(ctx, nil, "silence", callback(0))
	if err != nil {
		t.Fatal(err)
	}
	defer caller.Close()
	callee, err := newCallPeer(ctx, nil, "silence", callback(1))
	if err != nil {
		t.Fatal(err)
	}
	defer callee.Close()
	peers := []*callPeer{caller, callee}
	offer, err := caller.Offer()
	if err != nil {
		t.Fatal(err)
	}
	answer, err := callee.AcceptOffer(offer)
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(answer, "\r\n")
	replaced := false
	for i, line := range lines {
		if strings.HasPrefix(line, "a=fingerprint:") {
			lines[i] = "a=fingerprint:sha-256 " + strings.Repeat("00:", 31) + "00"
			replaced = true
		}
	}
	if !replaced {
		t.Fatal("local answer had no DTLS fingerprint")
	}
	// This remains syntactically valid SDP, so rejection must occur during
	// remote certificate verification after ICE, rather than SDP parsing.
	if err := caller.SetAnswer(strings.Join(lines, "\r\n")); err != nil {
		t.Fatal(err)
	}
	calleeConnected := false
	for {
		select {
		case <-ctx.Done():
			t.Fatalf("wrong fingerprint did not fail promptly: %+v", caller.Snapshot())
		case e := <-events:
			switch e.Kind {
			case "candidate":
				if err := peers[1-e.side].AddICE(e.Text); err != nil {
					t.Fatal(err)
				}
			case "state":
				if e.side != 0 {
					if e.Text == "connected" {
						calleeConnected = true
					}
					continue
				}
				if e.Text == "connected" {
					t.Fatal("Pion accepted a certificate with a mismatched SDP fingerprint")
				}
				if e.Text == "failed" || e.Text == "closed" {
					snapshot := caller.Snapshot()
					if (snapshot.Connection != "failed" && snapshot.Connection != "closed") || snapshot.DTLS != "unconfirmed" || snapshot.RXPackets != 0 || snapshot.TXFrames != 0 {
						t.Fatalf("mismatched fingerprint did not reject connection before media: %+v", snapshot)
					}
					// Pion can close the ICE agent synchronously after failed
					// certificate verification. Its public stats then no longer
					// expose nomination, and the remote DTLS client can fail on
					// the alert before emitting Connected. Retain neither state
					// as an invented success. The only mutation relative to the
					// successful local positive controls is the SDP fingerprint;
					// Pion's WARN log carries the precise validation error.
					// A DTLS CloseNotify can close the PeerConnection before
					// the aggregate Failed callback is delivered, especially
					// under the race detector. Both terminal states reject it.
					t.Logf("valid SDP, mutated DTLS fingerprint rejected before media (callee Connected observed=%v): %+v", calleeConnected, snapshot)
					return
				}
			}
		}
	}
}
