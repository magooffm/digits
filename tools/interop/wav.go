package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"
)

const (
	wavRate         = 48000
	wavFrameSamples = 960
	// Opus can decode up to 120 ms. Buffers and queue memory stay bounded even
	// if a future upstream decoder supports those longer packets.
	wavMaxPacketSamples = 6 * wavFrameSamples
	wavQueuePackets     = 50
	wavMaxDataBytes     = (uint64(math.MaxUint32) - 36) &^ 1
)

type pcmSource struct {
	kind   string
	sample uint64
	wav    *wavInput
	live   *liveCapture
}

func validateAudio(audio, file string) error {
	if audio != "tone" && audio != "silence" && audio != "wav" && audio != "live" {
		return fmt.Errorf("audio must be tone, silence, wav or live")
	}
	if (audio == "wav") != (file != "") {
		return fmt.Errorf("--audio wav requires --audio-file; --audio-file is only valid with --audio wav")
	}
	return nil
}

func newPCMSource(audio, file string) (*pcmSource, error) {
	if err := validateAudio(audio, file); err != nil {
		return nil, err
	}
	s := &pcmSource{kind: audio}
	if audio == "live" {
		if err := checkLiveAudio(); err != nil {
			return nil, err
		}
		s.live = &liveCapture{}
	}
	if audio == "wav" {
		var err error
		s.wav, err = openWAVInput(file)
		if err != nil {
			return nil, err
		}
	}
	return s, nil
}

func (s *pcmSource) Fill(frame []int16) error {
	if s.live != nil {
		return s.live.Fill(frame)
	}
	if s.wav != nil {
		return s.wav.Fill(frame)
	}
	for i := range frame {
		frame[i] = 0
		if s.kind == "tone" {
			frame[i] = int16(2048 * math.Sin(2*math.Pi*440*float64(s.sample+uint64(i))/wavRate))
		}
	}
	s.sample += uint64(len(frame))
	return nil
}

func (s *pcmSource) Close() error {
	if s.live != nil {
		return s.live.Close()
	}
	if s.wav != nil {
		return s.wav.file.Close()
	}
	return nil
}

type wavInput struct {
	file                            *os.File
	dataStart, dataBytes, remaining int64
	buffer                          [wavFrameSamples * 2]byte
}

// Parse bounded chunk headers rather than loading the recording into memory.
// Only PCM16/48 kHz/mono is accepted; no conversion or compressed WAV codec.
func openWAVInput(path string) (_ *wavInput, err error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open WAV input: %w", err)
	}
	defer func() {
		if err != nil {
			_ = f.Close()
		}
	}()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("WAV input must be a regular file")
	}
	var header [12]byte
	if _, err = io.ReadFull(f, header[:]); err != nil {
		return nil, fmt.Errorf("WAV header: %w", err)
	}
	end := int64(binary.LittleEndian.Uint32(header[4:8])) + 8
	if string(header[:4]) != "RIFF" || string(header[8:]) != "WAVE" || end != info.Size() {
		return nil, fmt.Errorf("WAV must have a complete RIFF/WAVE container")
	}
	w := &wavInput{file: f}
	var haveFormat, haveData bool
	for offset := int64(12); offset < end; {
		if end-offset < 8 {
			return nil, fmt.Errorf("truncated WAV chunk header")
		}
		var chunk [8]byte
		if _, err = f.ReadAt(chunk[:], offset); err != nil {
			return nil, fmt.Errorf("WAV chunk: %w", err)
		}
		size := int64(binary.LittleEndian.Uint32(chunk[4:]))
		start := offset + 8
		next := start + size + size%2
		if next > end {
			return nil, fmt.Errorf("WAV chunk exceeds container")
		}
		switch string(chunk[:4]) {
		case "fmt ":
			if haveFormat || size < 16 {
				return nil, fmt.Errorf("invalid or duplicate WAV format")
			}
			var format [16]byte
			if _, err = f.ReadAt(format[:], start); err != nil {
				return nil, err
			}
			if binary.LittleEndian.Uint16(format[0:2]) != 1 || binary.LittleEndian.Uint16(format[2:4]) != 1 ||
				binary.LittleEndian.Uint32(format[4:8]) != wavRate || binary.LittleEndian.Uint32(format[8:12]) != wavRate*2 ||
				binary.LittleEndian.Uint16(format[12:14]) != 2 || binary.LittleEndian.Uint16(format[14:16]) != 16 {
				return nil, fmt.Errorf("WAV input must be uncompressed 48 kHz mono signed PCM16")
			}
			haveFormat = true
		case "data":
			if haveData || size == 0 || size%2 != 0 {
				return nil, fmt.Errorf("WAV requires one nonempty, sample-aligned data chunk")
			}
			w.dataStart, w.dataBytes, w.remaining = start, size, size
			haveData = true
		}
		offset = next
	}
	if !haveFormat || !haveData {
		return nil, fmt.Errorf("WAV requires fmt and data chunks")
	}
	if _, err = f.Seek(w.dataStart, io.SeekStart); err != nil {
		return nil, err
	}
	return w, nil
}

// Fill wraps exact source samples across frame boundaries, including recordings
// shorter than one frame. It restarts at the first PCM sample for every call.
func (w *wavInput) Fill(frame []int16) error {
	if len(frame) > wavFrameSamples {
		return fmt.Errorf("WAV frame exceeds 20 ms")
	}
	for filled := 0; filled < len(frame); {
		if w.remaining == 0 {
			if _, err := w.file.Seek(w.dataStart, io.SeekStart); err != nil {
				return err
			}
			w.remaining = w.dataBytes
		}
		count := min(len(frame)-filled, int(w.remaining/2))
		if _, err := io.ReadFull(w.file, w.buffer[:count*2]); err != nil {
			return fmt.Errorf("read WAV PCM: %w", err)
		}
		for i := 0; i < count; i++ {
			frame[filled+i] = int16(binary.LittleEndian.Uint16(w.buffer[i*2:]))
		}
		filled += count
		w.remaining -= int64(count * 2)
	}
	return nil
}

func wavHeader(dataBytes uint32) []byte {
	h := make([]byte, 44)
	copy(h, "RIFF")
	binary.LittleEndian.PutUint32(h[4:], 36+dataBytes)
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1)
	binary.LittleEndian.PutUint16(h[22:], 1)
	binary.LittleEndian.PutUint32(h[24:], wavRate)
	binary.LittleEndian.PutUint32(h[28:], wavRate*2)
	binary.LittleEndian.PutUint16(h[32:], 2)
	binary.LittleEndian.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], dataBytes)
	return h
}

func prepareRecordingDir(directory string) error {
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
		return fmt.Errorf("record directory must be a private 0700 directory, not a symlink")
	}
	return nil
}

type wavOutput interface {
	io.WriteSeeker
	Truncate(int64) error
	Sync() error
	Close() error
}

type recordingSnapshot struct {
	Path                                            string
	Samples, DroppedPackets, DroppedSamples, Errors uint64
	Finalized                                       bool
	Peak                                            uint32
	RMS                                             float64
}

type wavRecorder struct {
	path                                            string
	output                                          wavOutput
	queue                                           chan []int16
	pool                                            chan []int16
	done                                            chan struct{}
	mu                                              sync.RWMutex
	closed                                          bool
	closeOnce                                       sync.Once
	failed, finalized                               atomic.Bool
	samples, droppedPackets, droppedSamples, errors atomic.Uint64
	sumSquares                                      atomic.Uint64
	peak                                            atomic.Uint32
}

func newWAVRecorder(directory string) (*wavRecorder, error) {
	return newNamedWAVRecorder(directory, "receive")
}

// newNamedWAVRecorder creates PREFIX-<UTC time>-<random>.wav with 0600 mode.
func newNamedWAVRecorder(directory, prefix string) (*wavRecorder, error) {
	if err := prepareRecordingDir(directory); err != nil {
		return nil, err
	}
	f, err := os.CreateTemp(directory, prefix+"-"+time.Now().UTC().Format("20060102T150405Z")+"-*.wav")
	if err != nil {
		return nil, err
	}
	info, err := f.Stat()
	if err == nil && info.Mode().Perm() != 0600 {
		err = fmt.Errorf("recording file must have 0600 permissions")
	}
	if err != nil {
		_ = f.Close()
		_ = os.Remove(f.Name())
		return nil, err
	}
	r, err := startWAVRecorder(f, filepath.Clean(f.Name()), wavQueuePackets)
	if err != nil {
		_ = f.Close()
		_ = os.Remove(f.Name())
		return nil, err
	}
	return r, nil
}

func startWAVRecorder(output wavOutput, path string, capacity int) (*wavRecorder, error) {
	n, err := output.Write(wavHeader(0))
	if n != 44 && err == nil {
		err = io.ErrShortWrite
	}
	if err != nil {
		return nil, fmt.Errorf("write WAV header: %w", err)
	}
	r := &wavRecorder{path: path, output: output, queue: make(chan []int16, capacity),
		pool: make(chan []int16, capacity), done: make(chan struct{})}
	for i := 0; i < capacity; i++ {
		r.pool <- make([]int16, wavMaxPacketSamples)
	}
	go r.run()
	return r, nil
}

func (r *wavRecorder) drop(samples int) {
	if samples <= 0 {
		return
	}
	r.droppedPackets.Add(1)
	r.droppedSamples.Add(uint64(samples))
}

// Submit copies the upstream decoder's reused PCM into a fixed buffer pool.
// A slow disk never blocks RTP reception; queue exhaustion drops whole packets.
func (r *wavRecorder) Submit(pcm []int16) {
	if len(pcm) == 0 {
		return
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	if r.closed || r.failed.Load() || len(pcm) > wavMaxPacketSamples {
		r.drop(len(pcm))
		return
	}
	select {
	case block := <-r.pool:
		copy(block, pcm)
		select {
		case r.queue <- block[:len(pcm)]:
		default:
			r.pool <- block
			r.drop(len(pcm))
		}
	default:
		r.drop(len(pcm))
	}
}

func (r *wavRecorder) recordError(err error) {
	r.errors.Add(1)
	if !r.failed.Swap(true) {
		slog.Error("Receive WAV capture failed; call media continues", "error", err, "path", r.path)
	}
}

func (r *wavRecorder) run() {
	defer close(r.done)
	var dataBytes uint64
	buffer := make([]byte, wavMaxPacketSamples*2)
	for pcm := range r.queue {
		if r.failed.Load() {
			r.drop(len(pcm))
		} else if dataBytes+uint64(len(pcm)*2) > wavMaxDataBytes {
			r.recordError(fmt.Errorf("WAV reached the RIFF size limit"))
			r.drop(len(pcm))
		} else {
			for i, sample := range pcm {
				binary.LittleEndian.PutUint16(buffer[i*2:], uint16(sample))
			}
			n, err := r.output.Write(buffer[:len(pcm)*2])
			// Preserve only complete samples if a disk error causes a short write.
			dataBytes += uint64(n &^ 1)
			var sum uint64
			var peak uint32
			for _, sample := range pcm[:n/2] {
				value := int64(sample)
				sum += uint64(value * value)
				if value < 0 {
					value = -value
				}
				peak = max(peak, uint32(value))
			}
			r.sumSquares.Add(sum)
			if peak > r.peak.Load() {
				r.peak.Store(peak)
			}
			r.samples.Add(uint64(n / 2))
			if n != len(pcm)*2 && err == nil {
				err = io.ErrShortWrite
			}
			if err != nil {
				r.recordError(err)
				r.drop(len(pcm) - n/2)
			}
		}
		r.pool <- pcm[:wavMaxPacketSamples]
	}
	// Truncation removes any incomplete sample left by a failed disk write.
	var err error
	if err = r.output.Truncate(44 + int64(dataBytes)); err == nil {
		_, err = r.output.Seek(0, io.SeekStart)
	}
	if err == nil {
		var n int
		n, err = r.output.Write(wavHeader(uint32(dataBytes)))
		if n != 44 && err == nil {
			err = io.ErrShortWrite
		}
	}
	if err == nil {
		err = r.output.Sync()
	}
	closeErr := r.output.Close()
	if err == nil {
		err = closeErr
	}
	if err != nil {
		r.recordError(fmt.Errorf("finalize WAV: %w", err))
	} else {
		r.finalized.Store(true)
	}
}

func (r *wavRecorder) Snapshot() recordingSnapshot {
	s := recordingSnapshot{Path: r.path, Samples: r.samples.Load(), DroppedPackets: r.droppedPackets.Load(),
		DroppedSamples: r.droppedSamples.Load(), Errors: r.errors.Load(), Finalized: r.finalized.Load(), Peak: r.peak.Load()}
	if s.Samples > 0 {
		s.RMS = math.Sqrt(float64(r.sumSquares.Load()) / float64(s.Samples))
	}
	return s
}

func (r *wavRecorder) Close() {
	r.closeOnce.Do(func() {
		r.mu.Lock()
		r.closed = true
		close(r.queue)
		r.mu.Unlock()
		<-r.done
		s := r.Snapshot()
		slog.Info("Receive WAV closed", "path", s.Path, "recorded_samples", s.Samples,
			"recording_dropped_packets", s.DroppedPackets, "recording_dropped_samples", s.DroppedSamples,
			"recording_errors", s.Errors, "finalized", s.Finalized, "pcm_peak", s.Peak, "pcm_rms", s.RMS)
	})
}
