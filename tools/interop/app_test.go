package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	sigclient "github.com/justinlindh/digits/pi/digitsd/internal/signal"
)

// Controller unit tests keep all application access on this test goroutine.
// Only the fake transport's mailbox and the state store cross goroutines.
type testSignaling struct {
	mu        sync.Mutex
	inbox     chan *sigclient.Message
	done      chan struct{}
	sent      []*sigclient.Message
	sendErr   error
	closeOnce sync.Once
}

type testLogBuffer struct {
	mu     sync.Mutex
	buffer bytes.Buffer
}

func (b *testLogBuffer) Write(data []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buffer.Write(data)
}

func (b *testLogBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buffer.String()
}

func newTestSignaling() *testSignaling {
	return &testSignaling{inbox: make(chan *sigclient.Message, 16), done: make(chan struct{})}
}

func (s *testSignaling) Send(message *sigclient.Message) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sendErr != nil {
		return s.sendErr
	}
	data, err := message.Marshal()
	if err != nil {
		return err
	}
	copy, err := sigclient.ParseMessage(data)
	if err != nil {
		return err
	}
	s.sent = append(s.sent, copy)
	return nil
}
func (s *testSignaling) Inbox() <-chan *sigclient.Message { return s.inbox }
func (s *testSignaling) Done() <-chan struct{}            { return s.done }
func (s *testSignaling) Close() error {
	s.closeOnce.Do(func() { close(s.done) })
	return nil
}
func (s *testSignaling) messages() []*sigclient.Message {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]*sigclient.Message(nil), s.sent...)
}

func testApplication(t *testing.T, paired bool) (*application, *testSignaling, context.Context) {
	t.Helper()
	store := openTestState(t, testStatePath(t), "", "", "")
	if paired {
		if err := store.SavePairing("5001", strings.Repeat("ab", 32)); err != nil {
			t.Fatal(err)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	transport := newTestSignaling()
	a := newApplication(store, options{audio: "silence", hostOnly: true})
	a.connection, a.session = transport, 7
	t.Cleanup(func() { cancel(); a.endCall(false, "test cleanup"); _ = transport.Close() })
	return a, transport, ctx
}

func waitControllerCandidate(t *testing.T, a *application) {
	t.Helper()
	timer := time.NewTimer(3 * time.Second)
	defer timer.Stop()
	for {
		select {
		case value := <-a.events:
			a.peerEvent(value)
			if value.event.Kind == "candidate" {
				return
			}
		case <-timer.C:
			t.Fatal("Pion did not gather a local bare ICE candidate")
		}
	}
}

func TestControllerOutgoingCallSDPThenBareICE(t *testing.T) {
	a, transport, ctx := testApplication(t, true)
	a.command(ctx, "call 5002")
	if a.call == nil || !a.call.caller || a.call.phase != "calling" {
		t.Fatal("call was not started")
	}
	waitControllerCandidate(t, a)
	messages := transport.messages()
	if len(messages) < 3 || messages[0].Type != sigclient.TypeCall || messages[1].Type != sigclient.TypeSDP || messages[1].SDP == "" {
		t.Fatalf("call/offer ordering: %+v", messages)
	}
	for i, message := range messages[2:] {
		if message.Type != sigclient.TypeICE || message.To != "5002" || !strings.HasPrefix(message.Candidate, "candidate:") || strings.HasPrefix(message.Candidate, "{") {
			t.Fatalf("candidate %d is not the original bare wire representation: %+v", i, message)
		}
	}
	if a.call.phase == "connected" {
		t.Fatal("sending offer incorrectly marked media connected")
	}
	a.command(ctx, "hangup")
	messages = transport.messages()
	if a.call != nil || messages[len(messages)-1].Type != sigclient.TypeHangup {
		t.Fatal("hangup did not tear down call")
	}
}

func TestControllerManualAnswerBeforeOfferAndEarlyRemoteICE(t *testing.T) {
	a, transport, ctx := testApplication(t, true)
	if err := a.receive(ctx, &sigclient.Message{Type: sigclient.TypeRing, From: "5002"}); err != nil {
		t.Fatal(err)
	}
	a.command(ctx, "answer")
	if a.call == nil || !a.call.answerRequested || a.call.peer != nil || len(transport.messages()) != 0 {
		t.Fatal("answer must wait for offer")
	}
	candidate := "candidate:1 1 UDP 2130706431 127.0.0.1 50000 typ host"
	if err := a.receive(ctx, &sigclient.Message{Type: sigclient.TypeICE, From: "5002", Candidate: candidate}); err != nil {
		t.Fatal(err)
	}
	if len(a.call.remoteICE) != 1 {
		t.Fatal("early remote ICE was not buffered")
	}
	caller, err := newCallPeer(ctx, nil, "silence", func(peerEvent) {})
	if err != nil {
		t.Fatal(err)
	}
	defer caller.Close()
	offer, err := caller.Offer()
	if err != nil {
		t.Fatal(err)
	}
	if err := a.receive(ctx, &sigclient.Message{Type: sigclient.TypeSDP, From: "5002", SDP: offer}); err != nil {
		t.Fatal(err)
	}
	if a.call == nil || a.call.phase != "connecting" || !a.call.remoteSDP || !a.call.localSDP || len(a.call.remoteICE) != 0 {
		t.Fatal("manual answer did not apply offer and drain remote ICE")
	}
	waitControllerCandidate(t, a)
	messages := transport.messages()
	if len(messages) < 2 || messages[0].Type != sigclient.TypeAnswer || messages[0].SDP == "" {
		t.Fatal("callee must send answer, not an sdp answer envelope")
	}
	for _, message := range messages[1:] {
		if message.Type != sigclient.TypeICE || !strings.HasPrefix(message.Candidate, "candidate:") {
			t.Fatal("local ICE was not gated behind answer")
		}
	}
	remaining := time.Until(a.call.deadline)
	if remaining <= 0 || remaining > connectTimeout {
		t.Fatal("post-answer deadline does not match upstream 10 seconds")
	}
}

func TestControllerUnpairedCallsAndRingsRejected(t *testing.T) {
	a, transport, ctx := testApplication(t, false)
	a.command(ctx, "call 5002")
	if err := a.receive(ctx, &sigclient.Message{Type: sigclient.TypeRing, From: "5002"}); err != nil {
		t.Fatal(err)
	}
	if a.call != nil || len(transport.messages()) != 0 {
		t.Fatal("unpaired device participated in a call")
	}
}

func TestControllerForeignAndConferenceTrafficCannotHangUpActiveLine(t *testing.T) {
	a, transport, ctx := testApplication(t, true)
	a.call = &callSession{id: 3, remote: "5002", phase: "ringing"}
	call := a.call
	for _, message := range []*sigclient.Message{
		{Type: sigclient.TypeRing, From: "5003"},
		{Type: sigclient.TypeSDP, From: "5003", SDP: "foreign offer"},
		{Type: sigclient.TypeICE, From: "5003", Candidate: "not a candidate"},
		{Type: sigclient.TypeHangup, From: "5003"},
		{Type: sigclient.TypeBusy, From: "5003"},
		{Type: sigclient.TypeHangup, From: "5002", ConfID: "conference-id"},
		{Type: sigclient.TypeSDP, From: "5002", ConfID: "conference-id", SDP: strings.Repeat("x", maxSDP+1)},
	} {
		if err := a.receive(ctx, message); err != nil {
			t.Fatal(err)
		}
	}
	if a.call != call || len(transport.messages()) != 0 {
		t.Fatal("foreign/conference traffic ended a call or sent line-wide hangup")
	}
	if err := a.receive(ctx, &sigclient.Message{Type: sigclient.TypeHangup, From: "5002"}); err != nil {
		t.Fatal(err)
	}
	if a.call != nil || len(transport.messages()) != 0 {
		t.Fatal("remote hangup should release locally without echo")
	}
}

func TestControllerGenerationRejectsStalePeerCallbacks(t *testing.T) {
	a, transport, _ := testApplication(t, true)
	a.call = &callSession{id: 12, remote: "5002", phase: "calling", localSDP: true}
	for _, value := range []callEvent{
		{session: a.session - 1, callID: 12, event: peerEvent{Kind: "state", Text: "connected"}},
		{session: a.session, callID: 11, event: peerEvent{Kind: "failure", Err: errors.New("old peer")}},
		{session: a.session - 1, callID: 12, event: peerEvent{Kind: "candidate", Text: "invalid old candidate"}},
	} {
		a.peerEvent(value)
	}
	if a.call == nil || a.call.phase != "calling" || len(transport.messages()) != 0 {
		t.Fatal("stale callback mutated current call")
	}
	a.peerEvent(callEvent{session: a.session, callID: 12, event: peerEvent{Kind: "candidate", Text: "candidate:1 1 UDP 1 127.0.0.1 50000 typ host"}})
	if got := transport.messages(); len(got) != 1 || got[0].Type != sigclient.TypeICE {
		t.Fatal("current callback was not accepted")
	}
}

func TestControllerSDPAndCandidateBoundsAbortOnlyCurrentCall(t *testing.T) {
	for name, message := range map[string]*sigclient.Message{
		"oversize SDP": {Type: sigclient.TypeSDP, From: "5002", SDP: strings.Repeat("x", maxSDP+1)},
		"empty SDP":    {Type: sigclient.TypeSDP, From: "5002"},
		"JSON ICE":     {Type: sigclient.TypeICE, From: "5002", Candidate: `{"candidate":"candidate:1"}`},
		"oversize ICE": {Type: sigclient.TypeICE, From: "5002", Candidate: "candidate:" + strings.Repeat("x", maxCandidate)},
	} {
		t.Run(name, func(t *testing.T) {
			a, transport, ctx := testApplication(t, true)
			a.call = &callSession{id: 1, remote: "5002", phase: "ringing"}
			if err := a.receive(ctx, message); err != nil {
				t.Fatal(err)
			}
			got := transport.messages()
			if a.call != nil || len(got) != 1 || got[0].Type != sigclient.TypeHangup || got[0].To != "5002" {
				t.Fatal("bad payload did not safely end affected call")
			}
		})
	}
}

func TestControllerRemoteCandidateBufferBounded(t *testing.T) {
	a, transport, ctx := testApplication(t, true)
	a.call = &callSession{id: 1, remote: "5002", phase: "ringing"}
	for i := 0; i < maxCandidates; i++ {
		if err := a.receive(ctx, &sigclient.Message{Type: sigclient.TypeICE, From: "5002", Candidate: ""}); err != nil {
			t.Fatal(err)
		}
	}
	if a.call == nil || len(a.call.remoteICE) != maxCandidates {
		t.Fatal("bounded early ICE bank did not retain end markers")
	}
	_ = a.receive(ctx, &sigclient.Message{Type: sigclient.TypeICE, From: "5002", Candidate: ""})
	if a.call != nil || len(transport.messages()) != 1 {
		t.Fatal("overflow did not abort call")
	}
}

func TestControllerWriteFailureDisconnectsAndClearsCall(t *testing.T) {
	a, transport, ctx := testApplication(t, true)
	transport.sendErr = errors.New("broken transport")
	a.command(ctx, "call 5002")
	if a.connection != nil || a.call != nil || !a.pendingHangup || a.backoff != 6*time.Second {
		t.Fatal("write failure did not release call and schedule reconnect cleanup")
	}
	select {
	case <-transport.done:
	default:
		t.Fatal("failed transport was not closed")
	}
}

func TestControllerReconnectBackoffIsBoundedAndResetAfterStableSession(t *testing.T) {
	a, _, _ := testApplication(t, true)
	for _, want := range []time.Duration{6, 12, 24, 48, 60, 60} {
		a.scheduleRetry(false)
		if a.backoff != want*time.Second || time.Until(a.retryAt) <= 0 {
			t.Fatalf("backoff=%v, want %vs", a.backoff, want)
		}
	}
	a.scheduleRetry(true)
	if time.Until(a.retryAt) > time.Millisecond {
		t.Fatal("credential reconnect was delayed")
	}
	a.connectedAt = time.Now().Add(-time.Minute)
	a.scheduleRetry(false)
	if a.backoff != 0 || time.Until(a.retryAt) > time.Millisecond {
		t.Fatal("stable connection did not reset retry sequence")
	}
}

func TestControllerCredentialPersistenceFailureStopsRun(t *testing.T) {
	store := openTestState(t, testStatePath(t), "", "", "")
	original := store.Snapshot()
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	transport := newTestSignaling()
	transport.inbox <- &sigclient.Message{Type: sigclient.TypePaired, Number: "5001", DeviceToken: strings.Repeat("ab", 32)}
	a := newApplication(store, options{audio: "silence"})
	dials := 0
	a.connect = func(deviceState) (signalingConnection, error) { dials++; return transport, nil }
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	err := a.run(ctx, make(chan string))
	if err == nil || !strings.Contains(err.Error(), "automatic reconnect stopped") || dials != 1 {
		t.Fatalf("credential failure did not stop retry: %v, dials=%d", err, dials)
	}
	if store.Snapshot() != original {
		t.Fatal("failed pairing changed in-memory identity")
	}
}

// Exercise the original signal.Client over actual WebSockets, including its
// control-frame PONG and the first JSON on every credential reconnect. The
// server deliberately sends no successful registration acknowledgement.
func TestOriginalWebSocketPairPersistReconnectRenumberAndPong(t *testing.T) {
	store := openTestState(t, testStatePath(t), "", "", "")
	initial := store.Snapshot()
	token := strings.Repeat("aB", 32)
	var logs testLogBuffer
	previousLogger := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&logs, nil)))
	defer slog.SetDefault(previousLogger)
	errorsChannel := make(chan error, 16)
	registrations := make(chan *sigclient.Message, 3)
	requests := make(chan int, 2)
	pongs := make(chan string, 1)
	var connectionMu sync.Mutex
	connections := 0
	upgrader := websocket.Upgrader{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ws, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			errorsChannel <- err
			return
		}
		defer ws.Close()
		_ = ws.SetReadDeadline(time.Now().Add(5 * time.Second))
		_, data, err := ws.ReadMessage()
		if err != nil {
			errorsChannel <- err
			return
		}
		reg, err := sigclient.ParseMessage(data)
		if err != nil || reg.Type != sigclient.TypeRegister {
			errorsChannel <- fmt.Errorf("first JSON was not register: %s", data)
			return
		}
		connectionMu.Lock()
		connections++
		index := connections
		connectionMu.Unlock()
		if reg.HardwareID != initial.HardwareID {
			errorsChannel <- errors.New("reconnect changed hardware identity")
		}
		if index > 1 {
			saved, err := os.ReadFile(store.path)
			var state deviceState
			if err != nil || json.Unmarshal(saved, &state) != nil || state.Number != reg.Number || state.DeviceToken != reg.DeviceToken {
				errorsChannel <- errors.New("new register preceded committed credentials")
			}
		}
		registrations <- reg
		if index == 1 {
			ws.SetPongHandler(func(payload string) error { pongs <- payload; return nil })
			if err := ws.WriteJSON(&sigclient.Message{Type: sigclient.TypePairingCode, PairingCode: "001234", PairingCodeTTL: 60}); err != nil {
				errorsChannel <- err
				return
			}
			if err := ws.WriteControl(websocket.PingMessage, []byte("probe-001"), time.Now().Add(time.Second)); err != nil {
				errorsChannel <- err
				return
			}
			readFinished := make(chan struct{})
			go func() { defer close(readFinished); _, _, _ = ws.ReadMessage() }()
			select {
			case payload := <-pongs:
				if payload != "probe-001" {
					errorsChannel <- errors.New("PONG did not echo PING payload")
				}
			case <-time.After(3 * time.Second):
				errorsChannel <- errors.New("reused signal client did not send control-frame PONG")
				_ = ws.Close()
				<-readFinished
				return
			}
			if err := ws.WriteJSON(&sigclient.Message{Type: sigclient.TypePaired, Number: "5001", DeviceToken: token}); err != nil {
				errorsChannel <- err
			}
			<-readFinished
			return
		}
		_, data, err = ws.ReadMessage()
		if err != nil {
			errorsChannel <- err
			return
		}
		request, err := sigclient.ParseMessage(data)
		if err != nil || request.Type != sigclient.TypeRequestICE {
			errorsChannel <- fmt.Errorf("waiting for nonexistent register ACK or unexpected post-register message: %s", data)
			return
		}
		requests <- index
		if index == 2 {
			if err := ws.WriteJSON(&sigclient.Message{Type: sigclient.TypeLineRenumber, Number: "5009"}); err != nil {
				errorsChannel <- err
			}
		}
		_, _, _ = ws.ReadMessage() // Client deliberately closes after renumber or quit.
	}))
	defer server.Close()
	a := newApplication(store, options{server: "ws" + strings.TrimPrefix(server.URL, "http") + "/ws", audio: "silence", hostOnly: true})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	commands := make(chan string, 1)
	finished := make(chan error, 1)
	go func() { finished <- a.run(ctx, commands) }()
	stopped := false
	defer func() {
		cancel()
		if !stopped {
			<-finished
		}
	}()
	for _, expected := range []struct{ number, token string }{{"unpaired", ""}, {"5001", token}, {"5009", token}} {
		select {
		case reg := <-registrations:
			if reg.Number != expected.number || reg.DeviceToken != expected.token {
				t.Fatalf("register credentials=%+v, want number=%s", reg, expected.number)
			}
		case err := <-errorsChannel:
			t.Fatal(err)
		case <-ctx.Done():
			t.Fatal("timed out waiting for credential register")
		}
	}
	for _, expected := range []int{2, 3} {
		select {
		case got := <-requests:
			if got != expected {
				t.Fatalf("ICE request from session %d, want %d", got, expected)
			}
		case <-ctx.Done():
			t.Fatal("client waited for register ACK")
		}
	}
	commands <- "quit"
	if err := <-finished; err != nil {
		t.Fatal(err)
	}
	stopped = true
	if !strings.Contains(logs.String(), "code=001234") || !strings.Contains(logs.String(), "ttl_seconds=60") {
		t.Fatal("pairing code leading zeroes or TTL were lost")
	}
	if strings.Contains(logs.String(), token) {
		t.Fatal("logs disclosed bearer token")
	}
	if state := store.Snapshot(); state.Number != "5009" || state.DeviceToken != token || state.HardwareID != initial.HardwareID {
		t.Fatal("pairing and renumber changed identity/token")
	}
	for {
		select {
		case err := <-errorsChannel:
			t.Error(err)
		default:
			return
		}
	}
}

func TestServerURLValidation(t *testing.T) {
	for _, url := range []string{"ws://localhost:8080/ws", "wss://digits.example/ws", "ws://192.168.1.2/ws"} {
		if err := validateServer(url); err != nil {
			t.Fatalf("valid URL %q rejected: %v", url, err)
		}
	}
	for _, url := range []string{"", "http://localhost/ws", "ws://localhost/", "ws://user:secret@localhost/ws", "ws://localhost/ws?token=secret", "ws://localhost/ws#fragment"} {
		if validateServer(url) == nil {
			t.Fatalf("invalid URL %q accepted", url)
		}
	}
}

func TestTraceFilesPreserveRawSDPAndBareCandidates(t *testing.T) {
	a, _, _ := testApplication(t, true)
	a.options.traceDir = filepath.Join(t.TempDir(), "private-trace")
	if err := prepareTraceDir(a.options.traceDir); err != nil {
		t.Fatal(err)
	}
	sdp := "v=0\r\na=ice-ufrag:example\r\n"
	candidate := "candidate:1 1 UDP 1 127.0.0.1 50000 typ host"
	if err := a.trace("local", &sigclient.Message{Type: sigclient.TypeSDP, SDP: sdp}); err != nil {
		t.Fatal(err)
	}
	if err := a.trace("remote", &sigclient.Message{Type: sigclient.TypeICE, Candidate: candidate}); err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(a.options.traceDir)
	if err != nil || len(entries) != 2 {
		t.Fatal("missing trace artifacts")
	}
	for _, entry := range entries {
		name := filepath.Join(a.options.traceDir, entry.Name())
		info, err := os.Stat(name)
		if err != nil || info.Mode().Perm() != 0600 {
			t.Fatal("trace is not private")
		}
		data, err := os.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		want := sdp
		if strings.HasSuffix(name, ".candidate") {
			want = candidate + "\n"
		}
		if string(data) != want {
			t.Fatal("wire payload was transformed in trace")
		}
	}
}
