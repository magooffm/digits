package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"time"

	sigclient "github.com/justinlindh/digits/pi/digitsd/internal/signal"
)

const (
	maxSDP         = 12288
	maxCandidate   = 512
	maxCandidates  = 32
	ringTimeout    = 60 * time.Second
	connectTimeout = 10 * time.Second // upstream digitsd's post-answer deadline
)

type signalingConnection interface {
	Send(*sigclient.Message) error
	Inbox() <-chan *sigclient.Message
	Done() <-chan struct{}
	Close() error
}

type connectionResult struct {
	connection signalingConnection
	err        error
}
type callEvent struct {
	session, callID uint64
	event           peerEvent
}
type callSession struct {
	id                                           uint64
	remote, phase, offer                         string
	caller, answerRequested, localSDP, remoteSDP bool
	localICE, remoteICE                          []string
	peer                                         *callPeer
	deadline                                     time.Time
}

// A single controller owns signaling, call state and all SDP/ICE ordering.
// Pion callbacks only enqueue tagged events. The original WS reader handles
// PING/PONG independently, including while libopus or SDP operations run.
type application struct {
	store                                *stateStore
	options                              options
	connection                           signalingConnection
	connecting                           bool
	results                              chan connectionResult
	events                               chan callEvent
	overflow                             chan callEvent
	connect                              func(deviceState) (signalingConnection, error)
	session, callID                      uint64
	call                                 *callSession
	servers                              []sigclient.ICEServer
	retryAt, connectedAt, pairingRefresh time.Time
	backoff                              time.Duration
	pendingHangup                        bool
	traceSequence                        uint64
}

func newApplication(store *stateStore, config options) *application {
	a := &application{store: store, options: config, results: make(chan connectionResult),
		events: make(chan callEvent, 128), overflow: make(chan callEvent, 1)}
	a.connect = func(state deviceState) (signalingConnection, error) {
		client := sigclient.NewClient(config.server, state.Number, state.HardwareID, state.DeviceToken)
		return client, client.Connect() // register is always the first WS message
	}
	return a
}

func (a *application) run(parent context.Context, commands <-chan string) error {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	defer func() {
		a.endCall(a.connection != nil, "client exit")
		if a.connection != nil {
			_ = a.connection.Close()
		}
	}()
	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	a.beginConnect(ctx)
	for {
		var inbox <-chan *sigclient.Message
		var done <-chan struct{}
		if a.connection != nil {
			inbox, done = a.connection.Inbox(), a.connection.Done()
		}
		select {
		case <-ctx.Done():
			return nil
		case result := <-a.results:
			a.connecting = false
			if result.err != nil {
				if result.connection != nil {
					_ = result.connection.Close()
				}
				slog.Error("WebSocket connection failed", "stage", "signaling", "error", result.err)
				a.scheduleRetry(false)
				continue
			}
			a.connection = result.connection
			a.connectedAt = time.Now()
			a.retryAt = time.Time{}
			state := a.store.Snapshot()
			slog.Info("WebSocket connected; register sent (no registration acknowledgement exists)", "stage", "signaling", "number", state.Number, "session", a.session)
			if state.DeviceToken == "" {
				a.pairingRefresh = time.Now().Add(9 * time.Minute)
			} else {
				if a.pendingHangup {
					a.pendingHangup = false
					if err := a.send(&sigclient.Message{Type: sigclient.TypeHangup}); err != nil {
						a.disconnect(false)
						continue
					}
				}
				if err := a.send(&sigclient.Message{Type: sigclient.TypeRequestICE}); err != nil {
					a.disconnect(false)
				}
			}
		case <-done:
			slog.Warn("WebSocket disconnected; releasing peer", "stage", "signaling")
			a.disconnect(false)
		case message := <-inbox:
			if message != nil {
				if err := a.receive(ctx, message); err != nil {
					return err
				}
			}
		case event := <-a.events:
			a.peerEvent(event)
		case event := <-a.overflow:
			if a.call != nil && event.session == a.session && event.callID == a.call.id {
				slog.Error("Peer event queue full; ending affected call", "stage", "signaling")
				a.endCall(true, "event queue exhausted")
			}
		case line, ok := <-commands:
			if !ok || line == "quit" || line == "exit" {
				return nil
			}
			a.command(ctx, line)
		case now := <-tick.C:
			if a.connection == nil && !a.connecting && !now.Before(a.retryAt) {
				a.beginConnect(ctx)
			}
			if a.connection != nil && !a.pairingRefresh.IsZero() && !now.Before(a.pairingRefresh) {
				slog.Info("Refreshing expiring pairing code", "stage", "pairing")
				a.disconnect(true)
			}
			if a.call != nil && !a.call.deadline.IsZero() && !now.Before(a.call.deadline) {
				if a.call.phase == "connecting" {
					a.reportStatus()
					slog.Error("Post-answer connection timeout; inspect ICE and DTLS evidence separately", "stage", "connection")
					a.endCall(true, sigclient.HangupReasonConnectTimeout)
				} else {
					a.endCall(true, "ring timeout")
				}
			}
		}
	}
}

func (a *application) beginConnect(ctx context.Context) {
	a.connecting = true
	a.session++
	state := a.store.Snapshot()
	slog.Info("Connecting to Digits", "stage", "signaling", "session", a.session, "number", state.Number)
	go func() {
		connection, err := a.connect(state)
		// Unbuffered ownership handoff: a late dial result cannot strand an
		// open socket in a queue after shutdown.
		select {
		case a.results <- connectionResult{connection, err}:
		case <-ctx.Done():
			if connection != nil {
				_ = connection.Close()
			}
		}
	}()
}

func (a *application) scheduleRetry(immediate bool) {
	if immediate {
		a.retryAt = time.Now()
		return
	}
	if !a.connectedAt.IsZero() && time.Since(a.connectedAt) >= 45*time.Second {
		a.backoff = 0
		a.retryAt = time.Now()
		return
	}
	if a.backoff == 0 {
		a.backoff = 6 * time.Second
	} else {
		a.backoff *= 2
	}
	if a.backoff > 60*time.Second {
		a.backoff = 60 * time.Second
	}
	a.retryAt = time.Now().Add(a.backoff)
	slog.Info("Reconnect scheduled", "stage", "signaling", "delay", a.backoff)
}

func (a *application) disconnect(immediate bool) {
	if a.call != nil {
		a.pendingHangup = true
	}
	a.endCall(false, "signaling lost or reconnect requested")
	connection := a.connection
	a.connection = nil
	a.servers = nil
	a.pairingRefresh = time.Time{}
	if connection != nil {
		_ = connection.Close()
	}
	a.scheduleRetry(immediate)
	a.connectedAt = time.Time{}
}

func (a *application) send(message *sigclient.Message) error {
	if a.connection == nil {
		return errors.New("signaling offline")
	}
	if err := a.connection.Send(message); err != nil {
		slog.Error("Signaling write failed", "stage", "signaling", "type", message.Type, "error", err)
		return err
	}
	if message.Type == sigclient.TypeSDP || message.Type == sigclient.TypeAnswer || message.Type == sigclient.TypeICE {
		if err := a.trace("local", message); err != nil {
			return err
		}
	}
	return nil
}

func (a *application) receive(ctx context.Context, message *sigclient.Message) error {
	if message.ConfID != "" {
		slog.Info("Ignoring unsupported conference message", "type", message.Type)
		return nil
	}
	switch message.Type {
	case sigclient.TypePairingCode:
		if a.store.Snapshot().DeviceToken != "" {
			return nil
		}
		slog.Info("PAIRING CODE — enter it in Digits 'Pair a new handset'", "stage", "pairing", "code", message.PairingCode, "ttl_seconds", message.PairingCodeTTL)
		if message.PairingCodeTTL > 0 {
			ttl := time.Duration(message.PairingCodeTTL) * time.Second
			margin := time.Minute
			if margin > ttl/5 {
				margin = ttl / 5
			}
			a.pairingRefresh = time.Now().Add(ttl - margin)
		}
		return nil
	case sigclient.TypePaired:
		if err := a.store.SavePairing(message.Number, message.DeviceToken); err != nil {
			return fmt.Errorf("pairing persistence failed; automatic reconnect stopped: %w", err)
		}
		slog.Info("Pairing committed locally; reconnecting with original token", "stage", "pairing", "number", message.Number)
		a.disconnect(true)
		return nil
	case sigclient.TypeLineRenumber:
		if message.Number == a.store.Snapshot().Number {
			return nil
		}
		if err := a.store.SaveNumber(message.Number); err != nil {
			return fmt.Errorf("renumber persistence failed; automatic reconnect stopped: %w", err)
		}
		slog.Info("Authoritative number saved; reconnecting", "stage", "pairing", "number", message.Number)
		a.disconnect(true)
		return nil
	case sigclient.TypeICEServers:
		a.servers = a.usableServers(message.Servers)
		return nil
	case sigclient.TypeError:
		slog.Error("Server error", "stage", "signaling", "error", message.Error)
		if a.call != nil {
			a.endCall(true, "server error")
		}
		return nil
	case sigclient.TypeRing:
		if a.store.Snapshot().DeviceToken == "" {
			return nil
		}
		if !validDeviceNumber(message.From) || message.From == a.store.Snapshot().Number {
			return nil
		}
		if a.call != nil {
			slog.Warn("Ignoring another ring while a single peer is active", "from", message.From)
			return nil // Ordinary server hangup is line-wide, not selected by To.
		}
		a.callID++
		a.call = &callSession{id: a.callID, remote: message.From, phase: "ringing", deadline: time.Now().Add(ringTimeout)}
		slog.Info("Incoming call: enter answer or hangup", "from", message.From)
		return nil
	}
	call := a.call
	if call == nil {
		return nil
	}
	if message.From != call.remote && !(message.Type == sigclient.TypeHangup && message.From == "") {
		return nil
	}
	switch message.Type {
	case sigclient.TypeSDP, sigclient.TypeAnswer:
		if message.SDP == "" || len(message.SDP) > maxSDP {
			a.failCall("SDP", errors.New("missing/oversized SDP"))
			return nil
		}
		if err := a.trace("remote", message); err != nil {
			return err
		}
		if message.Type == sigclient.TypeSDP {
			if call.caller || call.phase != "ringing" || call.offer != "" {
				a.failCall("SDP", errors.New("unexpected or repeated offer"))
				return nil
			}
			call.offer = message.SDP
			slog.Info("Incoming SDP retained until manual answer", "stage", "SDP", "bytes", len(call.offer))
			if call.answerRequested {
				a.answer(ctx)
			}
		} else {
			if !call.caller || call.phase != "calling" || !call.localSDP || call.peer == nil {
				a.failCall("SDP", errors.New("unexpected answer"))
				return nil
			}
			if err := call.peer.SetAnswer(message.SDP); err != nil {
				a.failCall("SDP", err)
				return nil
			}
			call.remoteSDP = true
			call.phase = "connecting"
			call.deadline = time.Now().Add(connectTimeout)
			a.drainRemoteICE()
		}
	case sigclient.TypeICE:
		if len(message.Candidate) > maxCandidate || (message.Candidate != "" && !strings.HasPrefix(message.Candidate, "candidate:")) {
			a.failCall("ICE", errors.New("expected bounded bare candidate string"))
			return nil
		}
		if err := a.trace("remote", message); err != nil {
			return err
		}
		if call.remoteSDP && call.peer != nil {
			if err := call.peer.AddICE(message.Candidate); err != nil {
				a.failCall("ICE", err)
			}
		} else {
			if len(call.remoteICE) == maxCandidates {
				a.failCall("ICE", errors.New("remote candidate buffer full"))
				return nil
			}
			call.remoteICE = append(call.remoteICE, message.Candidate)
		}
	case sigclient.TypeHangup, sigclient.TypeBusy:
		slog.Info("Remote call ended", "type", message.Type, "reason", message.Reason)
		a.endCall(false, message.Type)
	case sigclient.TypeICERestart:
		a.failCall("ICE", errors.New("ICE restart is outside this interoperability client"))
	default:
		slog.Debug("Ignoring message", "type", message.Type)
	}
	return nil
}

func (a *application) usableServers(servers []sigclient.ICEServer) []sigclient.ICEServer {
	var result []sigclient.ICEServer
	for _, server := range servers {
		var urls []string
		for _, raw := range server.URLs {
			if !a.options.hostOnly && strings.HasPrefix(raw, "stun:") {
				urls = append(urls, raw)
			} else {
				slog.Info("ICE server skipped for LAN milestone", "stage", "ICE", "host_only", a.options.hostOnly, "scheme", strings.SplitN(raw, ":", 2)[0])
			}
		}
		if len(urls) > 0 {
			result = append(result, sigclient.ICEServer{URLs: urls})
		}
	}
	slog.Info("ICE configuration cached for next call", "stage", "ICE", "stun_server_entries", len(result))
	return result
}

func (a *application) command(ctx context.Context, line string) {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return
	}
	if fields[0] == "call" && len(fields) == 2 {
		if a.connection == nil || a.store.Snapshot().DeviceToken == "" {
			slog.Warn("Call requires online paired registration")
			return
		}
		if a.call != nil || !validDeviceNumber(fields[1]) || fields[1] == a.store.Snapshot().Number {
			slog.Warn("Call requires idle state and a different numeric destination")
			return
		}
		a.callID++
		a.call = &callSession{id: a.callID, remote: fields[1], phase: "calling", caller: true, deadline: time.Now().Add(ringTimeout)}
		if err := a.send(&sigclient.Message{Type: sigclient.TypeCall, To: fields[1]}); err != nil {
			a.disconnect(false)
			return
		}
		if err := a.createPeer(ctx); err != nil {
			a.failCall("peer", err)
			return
		}
		offer, err := a.call.peer.Offer()
		if err != nil {
			a.failCall("SDP", err)
			return
		}
		if len(offer) > maxSDP {
			a.failCall("SDP", errors.New("local SDP exceeds ESP32 limit"))
			return
		}
		if err := a.send(&sigclient.Message{Type: sigclient.TypeSDP, To: a.call.remote, SDP: offer}); err != nil {
			a.disconnect(false)
			return
		}
		a.call.localSDP = true
		a.drainLocalICE()
		slog.Info("Outgoing call; offer sent before local ICE", "to", fields[1])
		return
	}
	if len(fields) != 1 {
		printHelp()
		return
	}
	switch fields[0] {
	case "answer":
		a.answer(ctx)
	case "hangup":
		a.endCall(true, "local hangup")
	case "status":
		a.reportStatus()
	case "reconnect":
		if a.connection != nil {
			a.endCall(true, "manual reconnect")
			a.disconnect(true)
		} else if !a.connecting {
			a.retryAt = time.Now()
		}
	case "help":
		printHelp()
	default:
		printHelp()
	}
}

func (a *application) createPeer(ctx context.Context) error {
	call := a.call
	session, id := a.session, call.id
	peer, err := newCallPeer(ctx, a.servers, a.options.audio, func(event peerEvent) {
		value := callEvent{session: session, callID: id, event: event}
		select {
		case a.events <- value:
		default:
			select {
			case a.overflow <- value:
			default:
			}
		}
	})
	if err == nil {
		call.peer = peer
	}
	return err
}

func (a *application) answer(ctx context.Context) {
	call := a.call
	if call == nil || call.caller || call.phase != "ringing" {
		slog.Warn("No incoming call to answer")
		return
	}
	call.answerRequested = true
	if call.offer == "" {
		slog.Info("Answer requested; waiting for caller SDP")
		return
	}
	if err := a.createPeer(ctx); err != nil {
		a.failCall("peer", err)
		return
	}
	answer, err := call.peer.AcceptOffer(call.offer)
	if err != nil {
		a.failCall("SDP", err)
		return
	}
	call.offer = ""
	call.remoteSDP = true
	call.phase = "connecting"
	call.deadline = time.Now().Add(connectTimeout)
	if !a.drainRemoteICE() {
		return
	}
	if len(answer) > maxSDP {
		a.failCall("SDP", errors.New("local answer exceeds ESP32 limit"))
		return
	}
	if err := a.send(&sigclient.Message{Type: sigclient.TypeAnswer, To: call.remote, SDP: answer}); err != nil {
		a.disconnect(false)
		return
	}
	call.localSDP = true
	a.drainLocalICE()
	slog.Info("Manual answer sent before local ICE; awaiting ICE/DTLS", "to", call.remote)
}

func (a *application) drainRemoteICE() bool {
	call := a.call
	for _, candidate := range call.remoteICE {
		if err := call.peer.AddICE(candidate); err != nil {
			a.failCall("ICE", err)
			return false
		}
	}
	call.remoteICE = nil
	return true
}

func (a *application) drainLocalICE() {
	call := a.call
	if call == nil || !call.localSDP {
		return
	}
	for _, candidate := range call.localICE {
		if err := a.send(&sigclient.Message{Type: sigclient.TypeICE, To: call.remote, Candidate: candidate}); err != nil {
			a.disconnect(false)
			return
		}
	}
	call.localICE = nil
}

func (a *application) peerEvent(value callEvent) {
	call := a.call
	if call == nil || value.session != a.session || value.callID != call.id {
		return
	}
	event := value.event
	switch event.Kind {
	case "candidate":
		if len(event.Text) > maxCandidate || !strings.HasPrefix(event.Text, "candidate:") {
			a.failCall("ICE", errors.New("invalid local bare candidate"))
			return
		}
		if len(call.localICE) == maxCandidates {
			a.failCall("ICE", errors.New("local candidate buffer full"))
			return
		}
		call.localICE = append(call.localICE, event.Text)
		a.drainLocalICE()
	case "state":
		if event.Text == "connected" {
			call.phase = "connected"
			call.deadline = time.Time{}
			call.peer.StartMedia()
			a.reportStatus()
		} else if event.Text == "failed" || event.Text == "closed" || event.Text == "disconnected" {
			a.reportStatus()
			a.failCall("connection", fmt.Errorf("Pion state %s; consult separate ICE/DTLS diagnostics", event.Text))
		}
	case "failure":
		stage := event.Text
		if stage == "" {
			stage = "media"
		}
		a.failCall(stage, event.Err)
	}
}

func (a *application) failCall(stage string, err error) {
	a.reportStatus()
	slog.Error("Call failed", "stage", stage, "error", err)
	a.endCall(true, sigclient.HangupReasonConnectTimeout)
}

func (a *application) endCall(notify bool, reason string) {
	call := a.call
	if call == nil {
		return
	}
	a.call = nil // Invalidate callbacks before closing Pion/read goroutines.
	if notify && a.connection != nil {
		wireReason := ""
		if reason == sigclient.HangupReasonConnectTimeout {
			wireReason = reason
		}
		if err := a.send(&sigclient.Message{Type: sigclient.TypeHangup, To: call.remote, Reason: wireReason}); err != nil {
			a.pendingHangup = true
		}
	}
	if call.peer != nil {
		snapshot := call.peer.Snapshot()
		slog.Info("Releasing peer", "peer", call.remote, "reason", reason, "rx_packets", snapshot.RXPackets, "tx_frames", snapshot.TXFrames)
		call.peer.Close()
	}
	slog.Info("Call idle", "reason", reason)
}

func (a *application) reportStatus() {
	state := a.store.Snapshot()
	phase := "idle"
	if a.call != nil {
		phase = a.call.phase
	}
	slog.Info("Client status", "signaling_online", a.connection != nil, "paired", state.DeviceToken != "", "number", state.Number, "call", phase)
	if a.call != nil && a.call.peer != nil {
		s := a.call.peer.Snapshot()
		slog.Info("Peer transport evidence", "stage", "ICE/DTLS/SRTP", "connection", s.Connection, "ice", s.ICE, "dtls", s.DTLS, "srtp", s.SRTP, "candidate_pair", s.CandidatePair)
		slog.Info("Peer media counters", "stage", "media", "codec", s.Codec, "clock_rate", s.ClockRate, "channels", s.Channels,
			"tx_frames", s.TXFrames, "rx_packets", s.RXPackets, "decoded_samples", s.DecodedSamples,
			"encode_errors", s.EncodeErrors, "decode_errors", s.DecodeErrors, "write_errors", s.WriteErrors)
	}
}

func prepareTraceDir(directory string) error {
	if directory == "" {
		return nil
	}
	if err := os.MkdirAll(directory, 0700); err != nil {
		return err
	}
	info, err := os.Lstat(directory)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode().Perm() != 0700 {
		return fmt.Errorf("trace directory must be a private 0700 directory")
	}
	return nil
}

func (a *application) trace(direction string, message *sigclient.Message) error {
	if a.options.traceDir == "" {
		return nil
	}
	a.traceSequence++
	name := fmt.Sprintf("%d-session%d-call%d-%04d-%s-%s", time.Now().UnixNano(), a.session, a.callID, a.traceSequence, direction, message.Type)
	var data []byte
	if message.Type == sigclient.TypeICE {
		data = []byte(message.Candidate + "\n")
		name += ".candidate"
	} else {
		data = []byte(message.SDP)
		name += ".sdp"
	}
	if err := os.WriteFile(filepath.Join(a.options.traceDir, name), data, 0600); err != nil {
		return fmt.Errorf("write protocol trace: %w", err)
	}
	return nil
}
