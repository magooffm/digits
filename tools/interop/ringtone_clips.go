package main

import (
	"encoding/binary"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"sync"
)

// User ringtone clips, numbers 10-99, read from the same folder the ESP32
// firmware packs (esp32/ringtones/NN-Name.ext). Decoded on first use with
// ffmpeg, trimmed, cut at 30 s with a fade-out and normalized to the ringer
// level like esp32/tools/pack_ringtones.py, but kept at 48 kHz.
const (
	ringtoneFirstClip = 10
	ringtoneLastClip  = 99
	clipMaxSeconds    = 30
	clipFadeSeconds   = 1
	clipSilence       = 0.01
)

var clipNamePattern = regexp.MustCompile(`^(\d{2})[-_ ](.+)\.[A-Za-z0-9]+$`)

type ringtoneClip struct {
	ID   int
	Name string
	Path string
}

type clipLibrary struct {
	mu      sync.Mutex
	dir     string
	clips   map[int]ringtoneClip
	decoded map[int][]int16
}

var clips = &clipLibrary{clips: map[int]ringtoneClip{}, decoded: map[int][]int16{}}

// defaultRingtoneDir is esp32/ringtones relative to bin/digits-pion in a
// full checkout; falls back to the working directory's ../../esp32/ringtones.
func defaultRingtoneDir() string {
	if executable, err := os.Executable(); err == nil {
		return filepath.Join(filepath.Dir(executable), "..", "..", "..", "esp32", "ringtones")
	}
	return filepath.Join("..", "..", "esp32", "ringtones")
}

// load scans dir; a missing folder simply means no clips.
func (l *clipLibrary) load(dir string) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.dir, l.clips, l.decoded = dir, map[int]ringtoneClip{}, map[int][]int16{}
	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		match := clipNamePattern.FindStringSubmatch(entry.Name())
		if entry.IsDir() || match == nil {
			continue
		}
		id, _ := strconv.Atoi(match[1])
		if id < ringtoneFirstClip || id > ringtoneLastClip {
			continue
		}
		if existing, dup := l.clips[id]; dup {
			return fmt.Errorf("ringtone %d used twice: %s and %s", id, existing.Path, entry.Name())
		}
		l.clips[id] = ringtoneClip{ID: id, Name: match[2], Path: filepath.Join(dir, entry.Name())}
	}
	return nil
}

func (l *clipLibrary) list() []ringtoneClip {
	l.mu.Lock()
	defer l.mu.Unlock()
	out := make([]ringtoneClip, 0, len(l.clips))
	for _, clip := range l.clips {
		out = append(out, clip)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// samples returns the decoded clip, or nil when it is missing or unreadable.
func (l *clipLibrary) samples(id int) []int16 {
	l.mu.Lock()
	defer l.mu.Unlock()
	if pcm, ok := l.decoded[id]; ok {
		return pcm
	}
	clip, ok := l.clips[id]
	if !ok {
		return nil
	}
	pcm, err := decodeClip(clip.Path)
	if err != nil {
		slog.Warn("Ringtone clip unusable; ringing with the standard ring", "ringtone", id, "error", err)
	}
	l.decoded[id] = pcm // Cache failures too: one warning per clip.
	return pcm
}

func decodeClip(path string) ([]int16, error) {
	command := exec.Command("ffmpeg", "-v", "error", "-nostdin", "-i", path, "-t", strconv.Itoa(clipMaxSeconds+1),
		"-ac", "1", "-ar", strconv.Itoa(wavRate), "-f", "s16le", "-")
	raw, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("ffmpeg: %w", err)
	}
	pcm := make([]int16, len(raw)/2)
	for i := range pcm {
		pcm[i] = int16(binary.LittleEndian.Uint16(raw[2*i:]))
	}
	return prepareClip(pcm)
}

func prepareClip(pcm []int16) ([]int16, error) {
	peak := 0
	for _, s := range pcm {
		peak = max(peak, abs(int(s)))
	}
	if peak == 0 {
		return nil, fmt.Errorf("no audio")
	}
	threshold := float64(peak) * clipSilence
	first, last := 0, len(pcm)
	for first < last && float64(abs(int(pcm[first]))) <= threshold {
		first++
	}
	for last > first && float64(abs(int(pcm[last-1]))) <= threshold {
		last--
	}
	pcm = append([]int16(nil), pcm[first:last]...)
	if limit := clipMaxSeconds * wavRate; len(pcm) > limit {
		pcm = pcm[:limit]
		fade := clipFadeSeconds * wavRate
		for i := 0; i < fade; i++ {
			pcm[limit-fade+i] = int16(int(pcm[limit-fade+i]) * (fade - i) / fade)
		}
	}
	peak = 0
	for _, s := range pcm {
		peak = max(peak, abs(int(s)))
	}
	for i, s := range pcm {
		pcm[i] = int16(int(s) * ringtonePeak / peak)
	}
	return pcm, nil
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}
