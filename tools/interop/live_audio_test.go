package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func withLiveCommands(t *testing.T, capture, playback []string) {
	t.Helper()
	oldCapture, oldPlayback := liveCaptureCommand, livePlaybackCommand
	liveCaptureCommand, livePlaybackCommand = capture, playback
	t.Cleanup(func() { liveCaptureCommand, livePlaybackCommand = oldCapture, oldPlayback })
}

func TestLiveAudioFlags(t *testing.T) {
	if err := validateAudio("live", ""); err != nil {
		t.Fatalf("live rejected: %v", err)
	}
	if err := validateAudio("live", "speech.wav"); err == nil {
		t.Fatal("live with --audio-file accepted")
	}
	withLiveCommands(t, []string{"digits-no-such-recorder"}, []string{"sh"})
	if _, err := newPCMSource("live", ""); err == nil {
		t.Fatal("missing capture command accepted")
	}
}

func TestLiveCaptureBoundedAndSilentWhenEmpty(t *testing.T) {
	// 20 frames of 'y\n' (sample 0x0a79), then stay alive until killed.
	withLiveCommands(t, []string{"sh", "-c", "yes | head -c 38400; exec sleep 30"}, []string{"sh"})
	source, err := newPCMSource("live", "")
	if err != nil {
		t.Fatal(err)
	}
	frame := make([]int16, wavFrameSamples)
	if err := source.Fill(frame); err != nil { // Starts capture; likely silence.
		t.Fatal(err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for source.live.drops.Load() < 20-liveCaptureQueue && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if got := source.live.drops.Load(); got < 20-liveCaptureQueue-1 {
		t.Fatalf("queue not bounded: drops=%d", got)
	}
	for i := 0; i < liveCaptureQueue; i++ {
		clear(frame)
		if err := source.Fill(frame); err != nil {
			t.Fatal(err)
		}
		if frame[0] != 0x0a79 || frame[wavFrameSamples-1] != 0x0a79 {
			t.Fatalf("frame %d: unexpected samples %#x", i, frame[0])
		}
	}
	underruns := source.live.underruns.Load()
	frame[0] = 1
	if err := source.Fill(frame); err != nil || frame[0] != 0 {
		t.Fatalf("empty queue must give silence: err=%v sample=%d", err, frame[0])
	}
	if source.live.underruns.Load() != underruns+1 {
		t.Fatal("underrun not counted")
	}
	start := time.Now()
	if err := source.Close(); err != nil {
		t.Fatal(err)
	}
	if time.Since(start) > 2*time.Second {
		t.Fatal("capture close did not stop the recorder promptly")
	}
}

func TestLiveCaptureCloseWithoutStart(t *testing.T) {
	withLiveCommands(t, []string{"sh"}, []string{"sh"})
	source, err := newPCMSource("live", "")
	if err != nil {
		t.Fatal(err)
	}
	if err := source.Close(); err != nil {
		t.Fatal(err)
	}
	if source.live.cmd != nil {
		t.Fatal("validation-only source started a recorder")
	}
}

func TestLivePlaybackWritesLittleEndianPCM(t *testing.T) {
	out := filepath.Join(t.TempDir(), "played.raw")
	withLiveCommands(t, []string{"sh"}, []string{"sh", "-c", `exec cat > "$0"`, out})
	player := &livePlayback{}
	first := []int16{1, -2, 0x1234}
	player.Submit(first)
	first[0] = 99 // Submit must copy caller memory.
	player.Submit([]int16{-32768, 32767})
	player.Submit(nil)
	player.Close()
	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	want := []int16{1, -2, 0x1234, -32768, 32767}
	if len(data) != 2*len(want) {
		t.Fatalf("wrote %d bytes, want %d", len(data), 2*len(want))
	}
	for i, sample := range want {
		if got := int16(binary.LittleEndian.Uint16(data[2*i:])); got != sample {
			t.Fatalf("sample %d = %d, want %d", i, got, sample)
		}
	}
	if player.drops.Load() != 0 || player.errors.Load() != 0 {
		t.Fatalf("drops=%d errors=%d", player.drops.Load(), player.errors.Load())
	}
}

func TestLivePlaybackFailureNeverBlocks(t *testing.T) {
	withLiveCommands(t, []string{"sh"}, []string{"sh", "-c", "exit 0"})
	player := &livePlayback{}
	frame := make([]int16, wavFrameSamples)
	done := make(chan struct{})
	go func() {
		for i := 0; i < 200; i++ {
			player.Submit(frame)
		}
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("Submit blocked after the player exited")
	}
	player.Close()
}

func TestLivePlaybackCloseWithoutStart(t *testing.T) {
	player := &livePlayback{}
	player.Close()
	if player.cmd != nil {
		t.Fatal("player started without audio")
	}
}

func TestNamedRecorderPrefix(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "audio")
	recorder, err := newNamedWAVRecorder(directory, "send")
	if err != nil {
		t.Fatal(err)
	}
	recorder.Submit(make([]int16, wavFrameSamples))
	recorder.Close()
	name := filepath.Base(recorder.path)
	if len(name) < 5 || name[:5] != "send-" || filepath.Ext(name) != ".wav" {
		t.Fatalf("unexpected file name %q", name)
	}
	if s := recorder.Snapshot(); !s.Finalized || s.Samples != wavFrameSamples {
		t.Fatalf("snapshot %+v", s)
	}
}

func TestRenderRingCadenceAndLevel(t *testing.T) {
	frame := make([]byte, liveFrameBytes)
	peak := 0
	for n := uint64(0); n < ringOnSamples; n += wavFrameSamples {
		renderRing(frame, n)
		for i := 0; i < wavFrameSamples; i++ {
			v := int(int16(binary.LittleEndian.Uint16(frame[2*i:])))
			if v < 0 {
				v = -v
			}
			if v > peak {
				peak = v
			}
		}
	}
	if peak < 3900 || peak > 2*ringComponentPeak {
		t.Fatalf("ring peak %d", peak)
	}
	renderRing(frame, ringOnSamples) // First frame of the 4 s pause.
	for i := range frame {
		if frame[i] != 0 {
			t.Fatal("pause is not silent")
		}
	}
	renderRing(frame, ringPeriodSamples) // Next cycle rings again.
	if binary.LittleEndian.Uint16(frame[2*100:]) == 0 {
		t.Fatal("second cycle silent")
	}
}

func TestLiveRingerStopsPromptly(t *testing.T) {
	out := filepath.Join(t.TempDir(), "ring.raw")
	// A slow consumer, like pacat's real-time buffer: Stop must not wait for it.
	withLiveCommands(t, []string{"sh"}, []string{"sh", "-c", `exec dd of="$0" bs=1920 2>/dev/null`, out})
	ringer := startLiveRinger(2)
	if ringer == nil {
		t.Fatal("ringer did not start")
	}
	time.Sleep(50 * time.Millisecond)
	start := time.Now()
	ringer.Stop()
	if time.Since(start) > time.Second {
		t.Fatal("Stop took too long")
	}
	info, err := os.Stat(out)
	if err != nil || info.Size() == 0 {
		t.Fatalf("ringer wrote nothing: %v", err)
	}
	var none *liveRinger
	none.Stop() // nil-safe for calls without live audio
}

func TestLiveRingerMissingPlayer(t *testing.T) {
	withLiveCommands(t, []string{"sh"}, []string{"digits-no-such-player"})
	if startLiveRinger(ringtoneStandard) != nil {
		t.Fatal("ringer reported running without a player")
	}
}
