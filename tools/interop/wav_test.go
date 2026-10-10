package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"sync"
	"testing"
	"time"
)

func writeTestWAV(t *testing.T, samples []int16) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "input.wav")
	data := append([]byte{}, wavHeader(uint32(len(samples)*2))...)
	for _, sample := range samples {
		data = binary.LittleEndian.AppendUint16(data, uint16(sample))
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestWAVInputLoopsAcrossShortFrameBoundary(t *testing.T) {
	samples := []int16{-32768, -1, 0, 12345, 32767}
	path := writeTestWAV(t, samples)
	for call := 0; call < 2; call++ {
		source, err := newPCMSource("wav", path)
		if err != nil {
			t.Fatal(err)
		}
		for frameIndex := 0; frameIndex < 3; frameIndex++ {
			frame := make([]int16, wavFrameSamples)
			if err := source.Fill(frame); err != nil {
				t.Fatal(err)
			}
			for i, sample := range frame {
				if want := samples[(frameIndex*wavFrameSamples+i)%len(samples)]; sample != want {
					t.Fatalf("call %d frame %d sample %d: got %d want %d", call, frameIndex, i, sample, want)
				}
			}
		}
		if err := source.Close(); err != nil {
			t.Fatal(err)
		}
	}
}

func TestWAVInputAcceptsPaddedMetadata(t *testing.T) {
	path := writeTestWAV(t, []int16{42, -42})
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	// Unknown odd-sized chunks include one padding byte, before fmt/data.
	chunk := append([]byte("JUNK"), 3, 0, 0, 0, 'a', 'b', 'c', 0)
	data = append(append(append([]byte{}, data[:12]...), chunk...), data[12:]...)
	binary.LittleEndian.PutUint32(data[4:], uint32(len(data)-8))
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	wav, err := openWAVInput(path)
	if err != nil {
		t.Fatal(err)
	}
	defer wav.file.Close()
	frame := make([]int16, 4)
	if err := wav.Fill(frame); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(frame, []int16{42, -42, 42, -42}) {
		t.Fatalf("metadata changed PCM: %v", frame)
	}
}

func TestWAVInputRejectsUnsupportedOrTruncated(t *testing.T) {
	validPath := writeTestWAV(t, []int16{1, 2, 3})
	valid, err := os.ReadFile(validPath)
	if err != nil {
		t.Fatal(err)
	}
	tests := map[string]func([]byte) []byte{
		"stereo":      func(d []byte) []byte { binary.LittleEndian.PutUint16(d[22:], 2); return d },
		"sample-rate": func(d []byte) []byte { binary.LittleEndian.PutUint32(d[24:], 44100); return d },
		"float":       func(d []byte) []byte { binary.LittleEndian.PutUint16(d[20:], 3); return d },
		"bits":        func(d []byte) []byte { binary.LittleEndian.PutUint16(d[34:], 24); return d },
		"byte-rate":   func(d []byte) []byte { binary.LittleEndian.PutUint32(d[28:], 1); return d },
		"alignment":   func(d []byte) []byte { binary.LittleEndian.PutUint16(d[32:], 1); return d },
		"truncated":   func(d []byte) []byte { return d[:len(d)-2] },
		"huge-chunk":  func(d []byte) []byte { binary.LittleEndian.PutUint32(d[40:], math.MaxUint32); return d },
		"odd-data":    func(d []byte) []byte { binary.LittleEndian.PutUint32(d[40:], 5); return d },
		"empty-data": func(d []byte) []byte {
			d = d[:44]
			binary.LittleEndian.PutUint32(d[4:], 36)
			binary.LittleEndian.PutUint32(d[40:], 0)
			return d
		},
		"missing-format": func(d []byte) []byte { copy(d[12:], "JUNK"); return d },
		"missing-data":   func(d []byte) []byte { copy(d[36:], "JUNK"); return d },
		"short-format":   func(d []byte) []byte { binary.LittleEndian.PutUint32(d[16:], 2); return d },
		"duplicate-format": func(d []byte) []byte {
			d = append(d, d[12:36]...)
			binary.LittleEndian.PutUint32(d[4:], uint32(len(d)-8))
			return d
		},
		"duplicate-data": func(d []byte) []byte {
			d = append(d, d[36:]...)
			binary.LittleEndian.PutUint32(d[4:], uint32(len(d)-8))
			return d
		},
	}
	for name, mutate := range tests {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "bad.wav")
			if err := os.WriteFile(path, mutate(append([]byte{}, valid...)), 0600); err != nil {
				t.Fatal(err)
			}
			if input, err := openWAVInput(path); err == nil {
				input.file.Close()
				t.Fatal("invalid input accepted")
			}
		})
	}
	for _, args := range [][2]string{{"wav", ""}, {"tone", validPath}, {"silence", validPath}, {"microphone", ""}} {
		if err := validateAudio(args[0], args[1]); err == nil {
			t.Fatalf("bad audio flags accepted: %v", args)
		}
	}
}

type testWAVOutput struct {
	*os.File
	block, entered     chan struct{}
	once               sync.Once
	failData, failSync bool
}

func (f *testWAVOutput) Write(data []byte) (int, error) {
	position, err := f.File.Seek(0, 1)
	if err != nil {
		return 0, err
	}
	if position >= 44 {
		if f.block != nil {
			f.once.Do(func() { close(f.entered) })
			<-f.block
		}
		if f.failData {
			return 0, errors.New("simulated disk full")
		}
	}
	return f.File.Write(data)
}

func (f *testWAVOutput) Sync() error {
	if f.failSync {
		return errors.New("simulated sync failure")
	}
	return f.File.Sync()
}

func TestWAVCaptureDoesNotBlockReceiverAndDrainsOnClose(t *testing.T) {
	f, err := os.CreateTemp(t.TempDir(), "capture-*.wav")
	if err != nil {
		t.Fatal(err)
	}
	output := &testWAVOutput{File: f, block: make(chan struct{}), entered: make(chan struct{})}
	r, err := startWAVRecorder(output, f.Name(), 2)
	if err != nil {
		t.Fatal(err)
	}
	pcm := []int16{-32768, -1, 12345, 32767}
	r.Submit(pcm)
	select {
	case <-output.entered:
	case <-time.After(time.Second):
		t.Fatal("writer did not start")
	}
	pcm[0] = 99 // The upstream decoder reuses its buffer; the saved copy must not change.
	r.Submit([]int16{42, -42})
	submitted := make(chan struct{})
	go func() { r.Submit([]int16{7, 8, 9}); close(submitted) }()
	select {
	case <-submitted:
	case <-time.After(time.Second):
		t.Fatal("slow disk blocked receiver")
	}
	if s := r.Snapshot(); s.DroppedPackets != 1 || s.DroppedSamples != 3 {
		t.Fatalf("missing overflow counters: %+v", s)
	}
	closed := make(chan struct{})
	go func() { r.Close(); close(closed) }()
	select {
	case <-closed:
		t.Fatal("Close did not drain pending writes")
	case <-time.After(20 * time.Millisecond):
	}
	close(output.block)
	select {
	case <-closed:
	case <-time.After(time.Second):
		t.Fatal("recorder did not finalize")
	}
	r.Close()
	s := r.Snapshot()
	if !s.Finalized || s.Errors != 0 || s.Samples != 6 {
		t.Fatalf("bad finalized counters: %+v", s)
	}
	if s.Peak != 32768 || s.RMS < 19000 || s.RMS > 20000 {
		t.Fatalf("saved PCM metrics incorrect: %+v", s)
	}
	data, err := os.ReadFile(f.Name())
	if err != nil {
		t.Fatal(err)
	}
	if len(data) != 56 || binary.LittleEndian.Uint32(data[4:]) != 48 || binary.LittleEndian.Uint32(data[40:]) != 12 {
		t.Fatalf("bad finalized WAV lengths: %v", data)
	}
	input, err := openWAVInput(f.Name())
	if err != nil {
		t.Fatal(err)
	}
	defer input.file.Close()
	frame := make([]int16, 6)
	if err := input.Fill(frame); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(frame, []int16{-32768, -1, 12345, 32767, 42, -42}) {
		t.Fatalf("capture copied wrong PCM: %v", frame)
	}
}

func TestWAVCaptureDiskFailuresAreObservable(t *testing.T) {
	for _, mode := range []string{"write", "sync"} {
		t.Run(mode, func(t *testing.T) {
			f, err := os.CreateTemp(t.TempDir(), "capture-*.wav")
			if err != nil {
				t.Fatal(err)
			}
			output := &testWAVOutput{File: f, failData: mode == "write", failSync: mode == "sync"}
			r, err := startWAVRecorder(output, f.Name(), 2)
			if err != nil {
				t.Fatal(err)
			}
			r.Submit([]int16{1, 2, 3})
			r.Close()
			s := r.Snapshot()
			if s.Errors != 1 {
				t.Fatalf("disk failure not reported: %+v", s)
			}
			if mode == "write" && (s.Samples != 0 || s.DroppedSamples != 3 || !s.Finalized) {
				t.Fatalf("bad write failure cleanup: %+v", s)
			}
			if mode == "sync" && (s.Samples != 3 || s.Finalized) {
				t.Fatalf("bad sync failure cleanup: %+v", s)
			}
		})
	}
}

func TestWAVCapturePrivateUniqueFiles(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "private")
	first, err := newWAVRecorder(directory)
	if err != nil {
		t.Fatal(err)
	}
	first.Close()
	second, err := newWAVRecorder(directory)
	if err != nil {
		t.Fatal(err)
	}
	second.Close()
	if first.path == second.path {
		t.Fatal("per-call recording overwrote a previous file")
	}
	info, err := os.Stat(first.path)
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("recording not private: %v %v", info, err)
	}
	if err := os.Chmod(directory, 0755); err != nil {
		t.Fatal(err)
	}
	if _, err := newWAVRecorder(directory); err == nil {
		t.Fatal("nonprivate output directory accepted")
	}
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(t.TempDir(), "symlink")
	if err := os.Symlink(directory, link); err != nil {
		t.Fatal(err)
	}
	if _, err := newWAVRecorder(link); err == nil {
		t.Fatal("symlink output directory accepted")
	}
	data, err := os.ReadFile(first.path)
	if err != nil || !bytes.Equal(data, wavHeader(0)) {
		t.Fatalf("empty call did not finalize valid WAV: %v", err)
	}
}
