#pragma once

// Local ringtone selection: one selector (rotary switch or fader) per call
// button picks how calls from that button's number ring on this phone. Every
// position is a ringtone. Pure logic, no ESP-IDF dependency;
// tests/ring_select_test.c runs it on the host.

#include <stdbool.h>
#include <stdint.h>

#define RING_SELECT_MAX_POSITIONS 12
#define RING_SELECT_MIN_POSITIONS 2

typedef struct {
    unsigned positions;                       // 2..12.
    uint8_t ringtone[RING_SELECT_MAX_POSITIONS]; // By 0-based position.
} ring_select_map_t;

// Positions take the available ringtones in order (built-ins first, then
// clips); unfilled positions get the standard ring (0).
void ring_select_default_map(ring_select_map_t *map, unsigned positions,
                             const unsigned *available, unsigned count);

// Repairs a stored map: clamps positions and replaces the old "caller's
// choice" marker (0xFF) with the standard ring. Returns false if anything
// changed.
bool ring_select_sanitize(ring_select_map_t *map);

// The ringtone to play. offered is the caller's choice or -1 for none;
// valid() reports which ringtones this phone has. A local ringtone for the
// position (RINGTONE_FIRST_LOCAL + position, from the SD card) wins over the
// map. A selected ringtone that is missing, or no selector for this caller
// (position < 0), falls back to the caller's choice, then to the standard
// ring.
unsigned ring_select_resolve(const ring_select_map_t *map, int position, int offered,
                             bool (*valid)(unsigned ringtone));

// Rotary switch on a resistor ladder or fader: raw ADC 0..full_scale spread
// evenly over the positions. A reading must move 20% of a step past the
// boundary before the position changes, so a switch between detents or a
// fader resting on a border cannot flicker. previous < 0 means unknown.
unsigned ring_select_position(unsigned raw, unsigned full_scale, unsigned positions, int previous);
