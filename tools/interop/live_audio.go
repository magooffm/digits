package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"math"
	"os/exec"
	"sync"
	"sync/atomic"
	"time"
)

// Live PC audio for two-way calls (--audio live). The default PipeWire or
// PulseAudio microphone feeds the Opus source and decoded remote audio plays
// on the default output. parec/pacat run as child processes only while a call
// has media: capture starts on the first source frame after Connected and
// playback on the first decoded packet. Use a headset; there is no echo
// cancellation on either end.

const (
	liveFrameBytes = wavFrameSamples * 2
	// Frames held between parec and the 20 ms Opus ticker. Older frames are
	// dropped so clock drift cannot grow into delay (3 frames = 60 ms).
	liveCaptureQueue = 3
	// Decoded packets waiting for pacat. A full queue drops new packets rather
	// than blocking the RTP read loop.
	livePlaybackQueue = 10
	liveStopTimeout   = time.Second
)

var (
	liveCaptureCommand = []string{"parec", "--raw", "--format=s16le", "--rate=48000",
		"--channels=1", "--latency-msec=20", "--client-name=digits-pion", "--stream-name=Digits call microphone"}
	livePlaybackCommand = []string{"pacat", "--playback", "--raw", "--format=s16le", "--rate=48000",
		"--channels=1", "--latency-msec=60", "--client-name=digits-pion", "--stream-name=Digits call"}
)

func checkLiveAudio() error {
	for _, command := range [][]string{liveCaptureCommand, livePlaybackCommand} {
		if _, err := exec.LookPath(command[0]); err != nil {
			return fmt.Errorf("--audio live needs %s (PulseAudio utilities, provided by pulseaudio-utils): %w", command[0], err)
		}
	}
	return nil
}

// waitOrKill reaps the child, killing it if it has not exited in time.
func waitOrKill(cmd *exec.Cmd) {
	exited := make(chan struct{})
	go func() {
		_ = cmd.Wait()
		close(exited)
	}()
	select {
	case <-exited:
	case <-time.After(liveStopTimeout):
		_ = cmd.Process.Kill()
		<-exited
	}
}

type liveCapture struct {
	once      sync.Once
	startErr  error
	cmd       *exec.Cmd
	frames    chan []int16
	done      chan struct{}
	underruns atomic.Uint64
	drops     atomic.Uint64
}

func (c *liveCapture) start() {
	cmd := exec.Command(liveCaptureCommand[0], liveCaptureCommand[1:]...)
	stdout, err := cmd.StdoutPipe()
	if err == nil {
		err = cmd.Start()
	}
	if err != nil {
		c.startErr = fmt.Errorf("live microphone: %w", err)
		return
	}
	c.cmd = cmd
	c.frames = make(chan []int16, liveCaptureQueue)
	c.done = make(chan struct{})
	slog.Info("Live microphone started", "command", liveCaptureCommand[0], "queue_frames", liveCaptureQueue)
	go c.read(stdout)
}

func (c *liveCapture) read(stdout io.Reader) {
	defer close(c.done)
	raw := make([]byte, liveFrameBytes)
	for {
		if _, err := io.ReadFull(stdout, raw); err != nil {
			return
		}
		frame := make([]int16, wavFrameSamples)
		for i := range frame {
			frame[i] = int16(binary.LittleEndian.Uint16(raw[2*i:]))
		}
		for {
			select {
			case c.frames <- frame:
			default:
				select {
				case <-c.frames:
					c.drops.Add(1)
				default:
				}
				continue
			}
			break
		}
	}
}

// Fill never blocks the media ticker: no captured frame yet means silence.
func (c *liveCapture) Fill(frame []int16) error {
	c.once.Do(c.start)
	if c.startErr != nil {
		return c.startErr
	}
	select {
	case captured := <-c.frames:
		copy(frame, captured)
	default:
		clear(frame)
		c.underruns.Add(1)
	}
	return nil
}

func (c *liveCapture) Close() error {
	c.once.Do(func() {}) // A never-started capture stays stopped.
	if c.cmd == nil {
		return nil
	}
	// Kill first: the reader then sees EOF. Wait only after reads finished,
	// as os/exec requires for StdoutPipe.
	_ = c.cmd.Process.Kill()
	<-c.done
	_ = c.cmd.Wait()
	slog.Info("Live microphone stopped", "underrun_frames", c.underruns.Load(), "dropped_frames", c.drops.Load())
	return nil
}

type livePlayback struct {
	once     sync.Once
	startErr error
	cmd      *exec.Cmd
	stdin    io.WriteCloser
	frames   chan []int16
	done     chan struct{}
	drops    atomic.Uint64
	errors   atomic.Uint64
}

func (p *livePlayback) start() {
	cmd := exec.Command(livePlaybackCommand[0], livePlaybackCommand[1:]...)
	stdin, err := cmd.StdinPipe()
	if err == nil {
		err = cmd.Start()
	}
	if err != nil {
		p.startErr = err
		slog.Error("Live speaker unavailable; call continues without playback", "error", err)
		return
	}
	p.cmd, p.stdin = cmd, stdin
	p.frames = make(chan []int16, livePlaybackQueue)
	p.done = make(chan struct{})
	slog.Info("Live speaker started", "command", livePlaybackCommand[0], "queue_packets", livePlaybackQueue)
	go p.write()
}

func (p *livePlayback) write() {
	defer close(p.done)
	failed := false
	for frame := range p.frames {
		if failed {
			continue // Drain so Submit never blocks after a playback failure.
		}
		raw := make([]byte, 2*len(frame))
		for i, sample := range frame {
			binary.LittleEndian.PutUint16(raw[2*i:], uint16(sample))
		}
		if _, err := p.stdin.Write(raw); err != nil {
			p.errors.Add(1)
			slog.Error("Live speaker write failed; playback stopped for this call", "error", err)
			failed = true
		}
	}
}

// Submit copies decoded PCM; it is called only from the single RTP reader.
func (p *livePlayback) Submit(pcm []int16) {
	p.once.Do(p.start)
	if p.startErr != nil || len(pcm) == 0 {
		return
	}
	select {
	case p.frames <- append([]int16(nil), pcm...):
	default:
		p.drops.Add(1)
	}
}

// Close runs after the RTP reader has stopped, so no Submit can race it.
func (p *livePlayback) Close() {
	p.once.Do(func() {})
	if p.cmd == nil {
		return
	}
	close(p.frames)
	<-p.done
	// pacat plays its short remaining buffer and exits after stdin closes.
	_ = p.stdin.Close()
	waitOrKill(p.cmd)
	slog.Info("Live speaker stopped", "dropped_packets", p.drops.Load(), "write_errors", p.errors.Load())
}

// Incoming-call ring for live mode: 440+480 Hz, 2 s on / 4 s off, the same
// tone and cadence as the ESP32 ringer. Plays until answer or call end.
const (
	ringOnSamples     = 2 * wavRate
	ringPeriodSamples = 6 * wavRate
	ringComponentPeak = 2048
)

func renderRing(frame []byte, start uint64) {
	for i := 0; i < len(frame)/2; i++ {
		n := start + uint64(i)
		var sample int16
		if n%ringPeriodSamples < ringOnSamples {
			t := float64(n) / wavRate
			sample = int16(ringComponentPeak * (math.Sin(2*math.Pi*440*t) + math.Sin(2*math.Pi*480*t)))
		}
		binary.LittleEndian.PutUint16(frame[2*i:], uint16(sample))
	}
}

type liveRinger struct {
	cmd      *exec.Cmd
	stdin    io.WriteCloser
	ringtone int
	stop     chan struct{}
	done     chan struct{}
}

// startLiveRinger returns nil when no player can start; ringing is a
// convenience and never blocks the call.
func startLiveRinger(ringtone int) *liveRinger {
	cmd := exec.Command(livePlaybackCommand[0], livePlaybackCommand[1:]...)
	stdin, err := cmd.StdinPipe()
	if err == nil {
		err = cmd.Start()
	}
	if err != nil {
		slog.Warn("Incoming ring unavailable", "error", err)
		return nil
	}
	r := &liveRinger{cmd: cmd, stdin: stdin, ringtone: ringtone, stop: make(chan struct{}), done: make(chan struct{})}
	go r.write()
	return r
}

func (r *liveRinger) write() {
	defer close(r.done)
	frame := make([]byte, liveFrameBytes)
	for n := uint64(0); ; n += wavFrameSamples {
		select {
		case <-r.stop:
			return
		default:
		}
		renderRingtoneCycle(r.ringtone, frame, n)
		// pacat's short buffer paces this loop at real time.
		if _, err := r.stdin.Write(frame); err != nil {
			return
		}
	}
}

// Stop silences at once: the player is killed rather than drained.
func (r *liveRinger) Stop() {
	if r == nil {
		return
	}
	close(r.stop)
	_ = r.cmd.Process.Kill()
	<-r.done
	_ = r.cmd.Wait()
}
