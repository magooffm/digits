package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The firmware header is the source of truth for names, tempos, notes and
// order (the wire index).
func TestRingtoneTableMatchesFirmware(t *testing.T) {
	data, err := os.ReadFile("../../esp32/main/ringtone_songs.h")
	if err != nil {
		t.Fatal(err)
	}
	header := regexp.MustCompile(`(?s)#define RINGTONE_SONGS\(RINGTONE_SONG\)(.*)`).FindSubmatch(data)
	if header == nil {
		t.Fatal("RINGTONE_SONGS not found")
	}
	entry := regexp.MustCompile(`(?s)RINGTONE_SONG\("([^"]*)",\s*(\d+),((?:\s*\\?\s*"[^"]*")+)\s*\\?\s*\)`)
	piece := regexp.MustCompile(`"([^"]*)"`)
	matches := entry.FindAllSubmatch(header[1], -1)
	if len(matches) != len(ringtoneSongs) {
		t.Fatalf("firmware has %d songs, helper %d", len(matches), len(ringtoneSongs))
	}
	for i, m := range matches {
		bpm, _ := strconv.Atoi(string(m[2]))
		var notes strings.Builder
		for _, p := range piece.FindAllSubmatch(m[3], -1) {
			notes.Write(p[1])
		}
		want := ringtoneSong{Name: string(m[1]), BPM: bpm, Notes: notes.String()}
		if ringtoneSongs[i] != want {
			t.Fatalf("song %d differs:\nhelper   %+v\nfirmware %+v", i, ringtoneSongs[i], want)
		}
	}
}

func TestRingtoneMelodiesRender(t *testing.T) {
	for id := 1; id < len(ringtoneSongs); id++ {
		song := parsedRingtones[id]
		if song.frames < 2*wavRate || song.frames > 8*wavRate {
			t.Fatalf("ringtone %d length %d frames", id, song.frames)
		}
		frame := make([]byte, 2*(song.frames+ringtonePauseFrames))
		renderRingtoneCycle(id, frame, 0)
		peak := 0
		for i := 0; i < len(frame)/2; i++ {
			v := int(int16(binary.LittleEndian.Uint16(frame[2*i:])))
			if i >= song.frames && v != 0 {
				t.Fatalf("ringtone %d: pause not silent at %d", id, i)
			}
			if v < 0 {
				v = -v
			}
			if v > peak {
				peak = v
			}
		}
		if peak < 1500 || peak > ringtonePeak {
			t.Fatalf("ringtone %d peak %d", id, peak)
		}
	}
	// The standard ring keeps the existing tone.
	a, b := make([]byte, 1920), make([]byte, 1920)
	renderRingtoneCycle(ringtoneStandard, a, 48000)
	renderRing(b, 48000)
	if string(a) != string(b) {
		t.Fatal("standard ring changed")
	}
}

func TestRingtoneOfferAttribute(t *testing.T) {
	offer := "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n"
	tagged := addRingtoneToOffer(offer, 3)
	if !strings.Contains(tagged, "t=0 0\r\na=x-digits-ringtone:3\r\nm=audio") {
		t.Fatalf("attribute not before media: %q", tagged)
	}
	id, stripped := takeRingtoneFromOffer(tagged)
	if id != 3 || stripped != offer {
		t.Fatalf("got %d %q", id, stripped)
	}
	for _, bad := range []string{"100", "", "2x", "-1", "+1", "0005", "7"} {
		id, stripped := takeRingtoneFromOffer(strings.Replace(offer, "m=", ringtoneSDPAttribute+bad+"\r\nm=", 1))
		if id != -1 || stripped != offer {
			t.Fatalf("value %q: got %d %q", bad, id, stripped)
		}
	}
	if id, same := takeRingtoneFromOffer(offer); id != -1 || same != offer {
		t.Fatal("plain offer changed")
	}
}

func TestClipLibraryAndCycle(t *testing.T) {
	dir := t.TempDir()
	for _, name := range []string{"12-Chime.wav", "50_Song.mp3", "README.md", "7-Too low.mp3", "notes.txt"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("x"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	t.Cleanup(func() { _ = clips.load(t.TempDir()) })
	if err := clips.load(dir); err != nil {
		t.Fatal(err)
	}
	list := clips.list()
	if len(list) != 2 || list[0].ID != 12 || list[0].Name != "Chime" || list[1].ID != 50 {
		t.Fatalf("clips %+v", list)
	}
	if err := os.WriteFile(filepath.Join(dir, "12 Other.wav"), nil, 0600); err != nil {
		t.Fatal(err)
	}
	if err := clips.load(dir); err == nil {
		t.Fatal("duplicate number accepted")
	}
	if err := clips.load(filepath.Join(dir, "missing")); err != nil || len(clips.list()) != 0 {
		t.Fatal("missing folder must mean no clips")
	}
	if !validRingtone(37) || validRingtone(100) || validRingtone(7) {
		t.Fatal("clip range validation")
	}

	// Trimming, normalization and the 30 s cut with fade.
	pcm := make([]int16, 31*wavRate+2000)
	for i := 1000; i < len(pcm)-1000; i++ {
		pcm[i] = int16(8000 * ((i/50)%2*2 - 1))
	}
	prepared, err := prepareClip(pcm)
	if err != nil || len(prepared) != clipMaxSeconds*wavRate {
		t.Fatalf("prepared %d samples: %v", len(prepared), err)
	}
	if prepared[0] == 0 || abs(int(prepared[100])) != ringtonePeak || abs(int(prepared[len(prepared)-1])) > 10 {
		t.Fatal("trim, level or fade wrong")
	}
	if _, err := prepareClip(make([]int16, 100)); err == nil {
		t.Fatal("silent clip accepted")
	}

	// A clip plays once, then the 2 s pause, then repeats.
	short := []int16{100, 200, 300}
	frame := make([]byte, 2*(len(short)+ringtonePauseFrames+2))
	renderClipCycle(short, frame, 0)
	sample := func(i int) int16 { return int16(binary.LittleEndian.Uint16(frame[2*i:])) }
	if sample(0) != 100 || sample(2) != 300 || sample(3) != 0 || sample(len(short)+ringtonePauseFrames) != 100 {
		t.Fatal("clip cycle wrong")
	}
	// Unknown clips fall back to the standard ring.
	a, b := make([]byte, 960), make([]byte, 960)
	renderRingtoneCycle(77, a, 0)
	renderRing(b, 0)
	if string(a) != string(b) {
		t.Fatal("missing clip must ring standard")
	}
}
