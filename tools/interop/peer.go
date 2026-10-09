package main

import (
	"context"
	"fmt"
	"log/slog"
	"math"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/justinlindh/digits/pi/digitsd/internal/codec"
	sigclient "github.com/justinlindh/digits/pi/digitsd/internal/signal"
	owebrtc "github.com/justinlindh/digits/pi/digitsd/internal/webrtc"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

// The wire-facing controller owns SDP/candidate ordering. These callbacks only
// publish events; they never write a WebSocket or wait for a remote description.
type peerEvent struct {
	Kind string
	Text string
	Err  error
}

type peerSnapshot struct {
	Connection       string `json:"connection"`
	ICE              string `json:"ice"`
	DTLS             string `json:"dtls"`
	SRTP             string `json:"srtp"`
	Codec            string `json:"codec,omitempty"`
	ClockRate        uint32 `json:"clock_rate,omitempty"`
	Channels         uint16 `json:"channels,omitempty"`
	CandidatePair    string `json:"candidate_pair,omitempty"`
	TXFrames         uint64 `json:"tx_frames"`
	RXPackets        uint64 `json:"rx_packets"`
	TXBytes          uint64 `json:"tx_opus_bytes"`
	RXBytes          uint64 `json:"rx_opus_bytes"`
	DecodedSamples   uint64 `json:"decoded_samples"`
	EncodeErrors     uint64 `json:"encode_errors"`
	DecodeErrors     uint64 `json:"decode_errors"`
	WriteErrors      uint64 `json:"write_errors"`
	TransportTXBytes uint64 `json:"transport_bytes_sent"`
	TransportRXBytes uint64 `json:"transport_bytes_received"`
}

// callPeer uses the original Digits PeerManager directly. It retains the exact
// Pion defaults, local track, RTCP drainer, SDP methods, bare ICE representation,
// and original Opus decoder. No ALSA, UART or telephone hardware is imported.
type callPeer struct {
	manager *owebrtc.PeerManager
	encoder *codec.Encoder
	audio   string
	ctx     context.Context
	cancel  context.CancelFunc
	emit    func(peerEvent)

	mu             sync.Mutex
	closed         bool
	started        bool
	reading        bool // There is one audio track and one owner of the upstream decoder.
	codec          webrtc.RTPCodecParameters
	wg             sync.WaitGroup
	closeOnce      sync.Once
	txFrames       atomic.Uint64
	rxPackets      atomic.Uint64
	txBytes        atomic.Uint64
	rxBytes        atomic.Uint64
	decodedSamples atomic.Uint64
	encodeErrors   atomic.Uint64
	decodeErrors   atomic.Uint64
	writeErrors    atomic.Uint64
}

func newCallPeer(ctx context.Context, servers []sigclient.ICEServer, audio string, emit func(peerEvent)) (*callPeer, error) {
	if audio != "silence" && audio != "tone" {
		return nil, fmt.Errorf("audio must be silence or tone")
	}
	iceServers := make([]owebrtc.ICEServerConfig, len(servers))
	for i, server := range servers {
		iceServers[i] = owebrtc.ICEServerConfig{
			URLs: append([]string(nil), server.URLs...), Username: server.Username, Credential: server.Credential,
		}
	}
	manager, err := owebrtc.NewPeerManager(owebrtc.NewICEConfig(iceServers))
	if err != nil {
		return nil, fmt.Errorf("peer/Opus initialization: %w", err)
	}
	// SendPCMFrame deliberately hides encode/write errors in the original Pi
	// implementation. A separate original encoder keeps the identical settings
	// while making those errors observable in this diagnostic client.
	encoder, err := codec.NewEncoder(48000, 1, 24000)
	if err != nil {
		_ = manager.Close()
		return nil, fmt.Errorf("Opus encoder: %w", err)
	}
	peerCtx, cancel := context.WithCancel(ctx)
	p := &callPeer{manager: manager, encoder: encoder, audio: audio, ctx: peerCtx, cancel: cancel, emit: emit}
	manager.SetOnICECandidate(func(candidate string) {
		p.publish(peerEvent{Kind: "candidate", Text: candidate})
	})
	manager.SetOnConnectionState(func(state webrtc.PeerConnectionState) {
		slog.Info("peer connection state", "stage", "connection", "state", state.String())
		if state == webrtc.PeerConnectionStateConnected {
			// Pion's aggregate Connected requires both ICE and DTLS; receipt of
			// an answer alone never reaches this branch.
			slog.Info("ICE and DTLS connected; SRTP reception awaits authenticated RTP", "stage", "DTLS")
		}
		p.publish(peerEvent{Kind: "state", Text: state.String()})
	})
	manager.SetOnRemoteTrack(p.remoteTrack)
	return p, nil
}

func (p *callPeer) publish(event peerEvent) {
	if p.ctx.Err() == nil && p.emit != nil {
		p.emit(event)
	}
}

func (p *callPeer) Offer() (string, error) {
	sdp, err := p.manager.CreateOffer()
	if err != nil {
		return "", fmt.Errorf("SDP create offer: %w", err)
	}
	slog.Info("SDP offer created", "stage", "SDP", "bytes", len(sdp))
	return sdp, nil
}

func (p *callPeer) AcceptOffer(sdp string) (string, error) {
	answer, err := p.manager.AcceptOffer(sdp)
	if err != nil {
		return "", fmt.Errorf("SDP accept offer: %w", err)
	}
	slog.Info("remote offer applied; local answer created", "stage", "SDP", "bytes", len(answer))
	return answer, nil
}

func (p *callPeer) SetAnswer(sdp string) error {
	if err := p.manager.SetAnswer(sdp); err != nil {
		return fmt.Errorf("SDP set answer: %w", err)
	}
	slog.Info("remote answer applied; media not yet confirmed", "stage", "SDP")
	return nil
}

func (p *callPeer) AddICE(candidate string) error {
	if candidate != "" && !strings.HasPrefix(candidate, "candidate:") {
		return fmt.Errorf("ICE candidate must be a bare candidate: string")
	}
	if err := p.manager.AddICECandidate(candidate); err != nil {
		return fmt.Errorf("ICE add remote candidate: %w", err)
	}
	return nil
}

// StartMedia is idempotent and creates exactly one encoder/send worker. The
// controller calls it only after Connected; the guard also enforces that here.
func (p *callPeer) StartMedia() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.started || p.manager.ConnectionState() != webrtc.PeerConnectionStateConnected {
		return
	}
	p.started = true
	p.wg.Add(1)
	go p.mediaLoop()
}

func (p *callPeer) mediaLoop() {
	defer p.wg.Done()
	frame := make([]int16, codec.FrameSize)
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	statsTicker := time.NewTicker(5 * time.Second)
	defer statsTicker.Stop()
	var sample uint64
	slog.Info("Opus source started", "stage", "Opus", "source", p.audio, "sample_rate", 48000,
		"pcm_channels", 1, "frame_samples", codec.FrameSize, "bitrate", 24000, "fec", true, "dtx", true)
	for {
		select {
		case <-p.ctx.Done():
			return
		case <-statsTicker.C:
			p.logSnapshot()
		case <-ticker.C:
			if p.audio == "tone" {
				for i := range frame {
					// Quiet 440 Hz (-24 dBFS peak) test source; nothing is sent
					// to physical audio hardware on the development machine.
					frame[i] = int16(2048 * math.Sin(2*math.Pi*440*float64(sample+uint64(i))/48000))
				}
			}
			sample += codec.FrameSize
			encoded, err := p.encoder.Encode(frame)
			if err != nil {
				p.encodeErrors.Add(1)
				p.publish(peerEvent{Kind: "failure", Text: "Opus", Err: fmt.Errorf("Opus encode: %w", err)})
				return
			}
			if err := p.manager.LocalTrack().WriteSample(media.Sample{Data: encoded, Duration: 20 * time.Millisecond}); err != nil {
				if p.ctx.Err() != nil {
					return
				}
				p.writeErrors.Add(1)
				p.publish(peerEvent{Kind: "failure", Text: "SRTP", Err: fmt.Errorf("SRTP RTP sample write: %w", err)})
				return
			}
			p.txFrames.Add(1)
			p.txBytes.Add(uint64(len(encoded)))
		}
	}
}

func (p *callPeer) remoteTrack(track *webrtc.TrackRemote) {
	p.mu.Lock()
	if p.closed || p.reading {
		p.mu.Unlock()
		return
	}
	p.reading = true
	p.codec = track.Codec()
	p.wg.Add(1)
	p.mu.Unlock()
	defer p.wg.Done()
	codecInfo := track.Codec()
	slog.Info("remote audio track", "stage", "SRTP", "mime", codecInfo.MimeType,
		"clock_rate", codecInfo.ClockRate, "sdp_channels", codecInfo.Channels,
		"payload_type", codecInfo.PayloadType, "fmtp", codecInfo.SDPFmtpLine, "ssrc", track.SSRC())
	if !strings.EqualFold(codecInfo.MimeType, webrtc.MimeTypeOpus) {
		p.publish(peerEvent{Kind: "failure", Text: "Opus", Err: fmt.Errorf("Opus expected, negotiated %s", codecInfo.MimeType)})
		return
	}
	for {
		packet, _, err := track.ReadRTP()
		if err != nil {
			if p.ctx.Err() == nil {
				p.publish(peerEvent{Kind: "failure", Text: "SRTP", Err: fmt.Errorf("SRTP decrypted RTP read: %w", err)})
			}
			return
		}
		count := p.rxPackets.Add(1)
		p.rxBytes.Add(uint64(len(packet.Payload)))
		if count == 1 {
			slog.Info("first authenticated/decrypted SRTP audio packet", "stage", "SRTP",
				"sequence", packet.SequenceNumber, "timestamp", packet.Timestamp, "bytes", len(packet.Payload))
		}
		pcm, err := p.manager.Decode(packet.Payload)
		if err != nil {
			p.decodeErrors.Add(1)
			slog.Error("Opus packet decode failed", "stage", "Opus", "error", err, "sequence", packet.SequenceNumber)
			continue
		}
		p.decodedSamples.Add(uint64(len(pcm)))
		// Consume PCM here; it is the upstream decoder's reusable buffer.
		// Counting successful samples verifies codec compatibility without any
		// sound device, microphone, loopback or retained unbounded audio queue.
	}
}

func (p *callPeer) Snapshot() peerSnapshot {
	snapshot := peerSnapshot{
		Connection: p.manager.ConnectionState().String(), ICE: "unconfirmed", DTLS: "unconfirmed", SRTP: "unconfirmed",
		TXFrames: p.txFrames.Load(), RXPackets: p.rxPackets.Load(), TXBytes: p.txBytes.Load(), RXBytes: p.rxBytes.Load(),
		DecodedSamples: p.decodedSamples.Load(), EncodeErrors: p.encodeErrors.Load(), DecodeErrors: p.decodeErrors.Load(), WriteErrors: p.writeErrors.Load(),
	}
	p.mu.Lock()
	snapshot.Codec = p.codec.MimeType
	snapshot.ClockRate = p.codec.ClockRate
	snapshot.Channels = p.codec.Channels
	if p.closed || snapshot.Connection == "failed" || snapshot.Connection == "closed" {
		p.mu.Unlock()
		if snapshot.RXPackets > 0 {
			snapshot.SRTP = "received_before_close"
		}
		return snapshot
	}
	// Pion 4.2.20's ICETransport.Stats populates byte totals but does not
	// populate TransportStats.ICEState, DTLSState or SRTPCipher. Do not report
	// those zero values as actual transport states. Selected pair statistics
	// provide separate ICE success evidence, and aggregate Connected requires
	// a successful DTLS handshake. SRTP is confirmed only by decrypted RTP.
	report := p.manager.GetStats()
	p.mu.Unlock()
	for _, stats := range report {
		if transport, ok := stats.(webrtc.TransportStats); ok {
			snapshot.TransportTXBytes += transport.BytesSent
			snapshot.TransportRXBytes += transport.BytesReceived
		}
		if pair, ok := stats.(webrtc.ICECandidatePairStats); ok && pair.Nominated && pair.State == webrtc.StatsICECandidatePairStateSucceeded {
			snapshot.ICE = "nominated" // Success evidence, not a live ICE state callback.
			local, localOK := report[pair.LocalCandidateID].(webrtc.ICECandidateStats)
			remote, remoteOK := report[pair.RemoteCandidateID].(webrtc.ICECandidateStats)
			if localOK && remoteOK {
				snapshot.CandidatePair = fmt.Sprintf("%s/%s %s:%d <-> %s:%d", local.Protocol, local.CandidateType, local.IP, local.Port, remote.IP, remote.Port)
			}
		}
	}
	if snapshot.Connection == "connected" {
		snapshot.ICE, snapshot.DTLS = "connected", "connected"
	}
	if snapshot.RXPackets > 0 {
		snapshot.SRTP = "receiving"
	}
	return snapshot
}

func (p *callPeer) logSnapshot() {
	s := p.Snapshot()
	slog.Info("peer media counters", "stage", "SRTP", "connection", s.Connection, "ice", s.ICE,
		"dtls", s.DTLS, "srtp", s.SRTP, "tx_frames", s.TXFrames, "rx_packets", s.RXPackets,
		"tx_opus_bytes", s.TXBytes, "rx_opus_bytes", s.RXBytes, "decoded_samples", s.DecodedSamples,
		"encode_errors", s.EncodeErrors, "decode_errors", s.DecodeErrors, "write_errors", s.WriteErrors,
		"transport_bytes_sent", s.TransportTXBytes, "transport_bytes_received", s.TransportRXBytes,
		"candidate_pair", s.CandidatePair)
}

func (p *callPeer) Close() {
	p.closeOnce.Do(func() {
		p.mu.Lock()
		p.closed = true
		p.mu.Unlock()
		p.cancel()
		// Close releases blocking TrackRemote reads and upstream RTCP draining.
		// Never call while holding the signaling controller's lock: Pion TURN
		// TCP gathering can make upstream Close slow for unreachable servers.
		if err := p.manager.Close(); err != nil {
			slog.Warn("peer close failed", "stage", "connection", "error", err)
		}
		p.wg.Wait()
		slog.Info("peer media workers stopped", "stage", "connection", "tx_frames", p.txFrames.Load(), "rx_packets", p.rxPackets.Load())
	})
}
