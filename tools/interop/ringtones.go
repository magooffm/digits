package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"strconv"
	"strings"
)

// Caller-selectable ringtones, mirroring esp32/main/ringtone_songs.h (index
// order is the wire contract). TestRingtoneTableMatchesFirmware fails if the
// two tables drift apart. The choice travels in the offer as a session-level
// SDP attribute that the unchanged server relays and other clients ignore.
const ringtoneSDPAttribute = "a=x-digits-ringtone:"

type ringtoneSong struct {
	Name  string
	BPM   int
	Notes string
}

var ringtoneSongs = []ringtoneSong{
	{"Standard ring", 0, ""},
	{"Fur Elise (Beethoven)", 100, "E6/16 D#6/16 E6/16 D#6/16 E6/16 B5/16 D6/16 C6/16 A5/8 R/16 C5/16 E5/16 A5/16 " +
		"B5/8 R/16 E5/16 G#5/16 B5/16 C6/8 R/16 E5/16 E6/16 D#6/16 E6/16 D#6/16 E6/16 " +
		"B5/16 D6/16 C6/16 A5/4"},
	{"Ode to Joy (Beethoven)", 150, "E5/4 E5/4 F5/4 G5/4 G5/4 F5/4 E5/4 D5/4 C5/4 C5/4 D5/4 E5/4 E5/4. D5/8 D5/2"},
	{"Westminster chimes", 150, "G#5/4 F#5/4 E5/4 B4/2 R/8 E5/4 G#5/4 F#5/4 B4/2 R/8 E5/4 F#5/4 G#5/4 E5/2"},
	{"Eine kleine Nachtmusik (Mozart)", 130, "G5/4 R/8 D5/8 G5/4 R/8 D5/8 G5/8 D5/8 G5/8 B5/8 D6/2 C6/4 R/8 A5/8 C6/4 R/8 A5/8 " +
		"C6/8 A5/8 F#5/8 A5/8 D5/2"},
}

const (
	ringtoneStandard    = 0
	ringtonePauseFrames = 2 * wavRate
	ringtonePeak        = 4096
	ringtoneDecayTau    = 0.4 // seconds
	ringtoneAttack      = 96  // frames (2 ms)
	ringtoneRelease     = 384 // frames (8 ms)
)

type ringtoneNote struct {
	start, length int
	hz            float64 // 0 for a rest
}

type parsedRingtone struct {
	notes  []ringtoneNote
	frames int
}

var parsedRingtones = mustParseRingtones()

func mustParseRingtones() []parsedRingtone {
	out := make([]parsedRingtone, len(ringtoneSongs))
	for i, song := range ringtoneSongs {
		parsed, err := parseRingtone(song)
		if err != nil {
			panic(fmt.Sprintf("ringtone %d: %v", i, err))
		}
		out[i] = parsed
	}
	return out
}

func parsePitch(text string) (float64, error) {
	semitones := map[byte]int{'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
	if len(text) < 2 {
		return 0, fmt.Errorf("bad pitch %q", text)
	}
	note, ok := semitones[text[0]]
	if !ok {
		return 0, fmt.Errorf("bad pitch %q", text)
	}
	rest := text[1:]
	if rest[0] == '#' {
		note++
		rest = rest[1:]
	} else if rest[0] == 'b' {
		note--
		rest = rest[1:]
	}
	if len(rest) != 1 || rest[0] < '0' || rest[0] > '8' {
		return 0, fmt.Errorf("bad octave in %q", text)
	}
	midi := 12*(int(rest[0]-'0')+1) + note
	return 440 * math.Pow(2, float64(midi-69)/12), nil
}

func parseRingtone(song ringtoneSong) (parsedRingtone, error) {
	var parsed parsedRingtone
	if song.Notes == "" {
		if song.BPM != 0 {
			return parsed, fmt.Errorf("standard ring has no tempo")
		}
		return parsed, nil
	}
	for _, token := range strings.Fields(song.Notes) {
		pitch, value, ok := strings.Cut(token, "/")
		if !ok || pitch == "" {
			return parsed, fmt.Errorf("bad note %q", token)
		}
		dotted := strings.HasSuffix(value, ".")
		denominator, err := strconv.Atoi(strings.TrimSuffix(value, "."))
		if err != nil || (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16) {
			return parsed, fmt.Errorf("bad length in %q", token)
		}
		seconds := 60.0 / float64(song.BPM) * 4 / float64(denominator)
		if dotted {
			seconds *= 1.5
		}
		note := ringtoneNote{start: parsed.frames, length: int(math.Round(seconds * wavRate))}
		if pitch != "R" {
			if note.hz, err = parsePitch(pitch); err != nil {
				return parsed, err
			}
		}
		parsed.notes = append(parsed.notes, note)
		parsed.frames += note.length
	}
	return parsed, nil
}

// validRingtone accepts the built-in numbers and the whole clip range: a
// caller may choose a clip that only the callee has; a callee without it
// rings with the standard ring.
func validRingtone(id int) bool {
	return (id >= 0 && id < len(ringtoneSongs)) || (id >= ringtoneFirstClip && id <= ringtoneLastClip)
}

func ringtoneName(id int) string {
	if id >= 0 && id < len(ringtoneSongs) {
		return ringtoneSongs[id].Name
	}
	for _, clip := range clips.list() {
		if clip.ID == id {
			return clip.Name + " (clip)"
		}
	}
	return "clip not on this computer"
}

// renderRingtoneCycle fills mono little-endian PCM16 for absolute frame
// index start of an endless ring: the standard 2 s on / 4 s off tone, or a
// melody followed by a 2 s pause.
func renderRingtoneCycle(id int, frame []byte, start uint64) {
	if id >= ringtoneFirstClip {
		if pcm := clips.samples(id); len(pcm) > 0 {
			renderClipCycle(pcm, frame, start)
			return
		}
	}
	if id < 0 || id >= len(ringtoneSongs) || id == ringtoneStandard {
		renderRing(frame, start)
		return
	}
	song := parsedRingtones[id]
	period := uint64(song.frames + ringtonePauseFrames)
	for i := 0; i < len(frame)/2; i++ {
		position := int((start + uint64(i)) % period)
		var sample int16
		if position < song.frames {
			sample = melodySample(song, position)
		}
		binary.LittleEndian.PutUint16(frame[2*i:], uint16(sample))
	}
}

func renderClipCycle(pcm []int16, frame []byte, start uint64) {
	period := uint64(len(pcm) + ringtonePauseFrames)
	for i := 0; i < len(frame)/2; i++ {
		var sample int16
		if position := (start + uint64(i)) % period; position < uint64(len(pcm)) {
			sample = pcm[position]
		}
		binary.LittleEndian.PutUint16(frame[2*i:], uint16(sample))
	}
}

func melodySample(song parsedRingtone, position int) int16 {
	for _, note := range song.notes {
		if position >= note.start+note.length {
			continue
		}
		if note.hz == 0 {
			return 0
		}
		offset := position - note.start
		envelope := math.Exp(-float64(offset) / (ringtoneDecayTau * wavRate))
		if offset < ringtoneAttack {
			envelope *= float64(offset) / ringtoneAttack
		}
		if left := note.length - offset; left < ringtoneRelease {
			envelope *= float64(left) / ringtoneRelease
		}
		phase := 2 * math.Pi * note.hz * float64(offset) / wavRate
		wave := math.Sin(phase) + 0.4*math.Sin(2*phase) + 0.15*math.Sin(3*phase)
		return int16(ringtonePeak * envelope * wave / 1.55)
	}
	return 0
}

// addRingtoneToOffer inserts the attribute before the first media section.
func addRingtoneToOffer(sdp string, id int) string {
	line := ringtoneSDPAttribute + strconv.Itoa(id) + "\r\n"
	if at := strings.Index(sdp, "\nm="); at >= 0 {
		return sdp[:at+1] + line + sdp[at+1:]
	}
	return sdp + line
}

// takeRingtoneFromOffer removes every attribute line and returns the first
// valid ringtone, or -1 when none was offered.
func takeRingtoneFromOffer(sdp string) (int, string) {
	ringtone := -1
	var kept strings.Builder
	for _, line := range strings.SplitAfter(sdp, "\n") {
		if !strings.HasPrefix(line, ringtoneSDPAttribute) {
			kept.WriteString(line)
			continue
		}
		value := strings.TrimRight(strings.TrimPrefix(line, ringtoneSDPAttribute), "\r\n")
		digits := value != "" && len(value) <= 3 && strings.Trim(value, "0123456789") == ""
		if id, err := strconv.Atoi(value); digits && err == nil && ringtone < 0 && validRingtone(id) {
			ringtone = id
		}
	}
	return ringtone, kept.String()
}
