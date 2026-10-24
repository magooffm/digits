#pragma once

// Caller-selectable ringtones. Index 0 is the standard ring; the others are
// public-domain melodies. tools/interop parses this file, so keep one song per
// RINGTONE_SONG line in exactly this form:
//   RINGTONE_SONG("Name", quarter-notes per minute, "NOTE/LEN ...")
// NOTE is a pitch like C5, F#4 or Bb4 (octave 4 holds A4 = 440 Hz), or R for a
// rest. LEN is the note value: 1 whole, 2 half, 4 quarter, 8 eighth, 16
// sixteenth; a trailing dot adds half (4. = dotted quarter). Order is the wire
// contract: a caller sends the index, so append new songs, never reorder.

#define RINGTONE_SONGS(RINGTONE_SONG)                                                          \
    RINGTONE_SONG("Standard ring", 0, "")                                                      \
    RINGTONE_SONG("Fur Elise (Beethoven)", 100,                                                \
        "E6/16 D#6/16 E6/16 D#6/16 E6/16 B5/16 D6/16 C6/16 A5/8 R/16 C5/16 E5/16 A5/16 "       \
        "B5/8 R/16 E5/16 G#5/16 B5/16 C6/8 R/16 E5/16 E6/16 D#6/16 E6/16 D#6/16 E6/16 "        \
        "B5/16 D6/16 C6/16 A5/4")                                                              \
    RINGTONE_SONG("Ode to Joy (Beethoven)", 150,                                               \
        "E5/4 E5/4 F5/4 G5/4 G5/4 F5/4 E5/4 D5/4 C5/4 C5/4 D5/4 E5/4 E5/4. D5/8 D5/2")         \
    RINGTONE_SONG("Westminster chimes", 150,                                                   \
        "G#5/4 F#5/4 E5/4 B4/2 R/8 E5/4 G#5/4 F#5/4 B4/2 R/8 E5/4 F#5/4 G#5/4 E5/2")          \
    RINGTONE_SONG("Eine kleine Nachtmusik (Mozart)", 130,                                      \
        "G5/4 R/8 D5/8 G5/4 R/8 D5/8 G5/8 D5/8 G5/8 B5/8 D6/2 C6/4 R/8 A5/8 C6/4 R/8 A5/8 "    \
        "C6/8 A5/8 F#5/8 A5/8 D5/2")
