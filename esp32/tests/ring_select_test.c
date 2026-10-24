// Host test for local ringtone selection: default map, repair, resolving
// against the caller's choice, and ADC position reading with hysteresis.
// Run by esp32/tests/run_host_tests.sh.
#include "ring_select.h"
#include <assert.h>
#include <stdio.h>

static bool have(unsigned ringtone)
{
    // This phone: built-ins 0-4 and clips 12 and 37.
    return ringtone <= 4 || ringtone == 12 || ringtone == 37;
}

static void test_default_map(void)
{
    const unsigned available[] = {0, 1, 2, 3, 4, 12, 37};
    ring_select_map_t map;
    ring_select_default_map(&map, 12, available, 7);
    assert(map.positions == 12);
    for (unsigned p = 0; p < 7; ++p) assert(map.ringtone[p] == available[p]);
    for (unsigned p = 7; p < 11; ++p) assert(map.ringtone[p] == 0); // Unfilled: standard.
    assert(map.ringtone[11] == RING_SELECT_CALLER);
    // Fewer positions (switch end stop): still ends with the caller's choice.
    ring_select_default_map(&map, 4, available, 7);
    assert(map.positions == 4 && map.ringtone[2] == 2 && map.ringtone[3] == RING_SELECT_CALLER);
    ring_select_default_map(&map, 1, available, 7); // Clamped to 2.
    assert(map.positions == 2 && map.ringtone[0] == 0 && map.ringtone[1] == RING_SELECT_CALLER);
    ring_select_default_map(&map, 40, available, 7); // Clamped to 12.
    assert(map.positions == 12);
}

static void test_sanitize(void)
{
    ring_select_map_t map = {.positions = 5, .ringtone = {1, RING_SELECT_CALLER, 3, 4, 2}};
    assert(!ring_select_sanitize(&map));
    assert(map.ringtone[1] == 0 && map.ringtone[4] == RING_SELECT_CALLER);
    assert(ring_select_sanitize(&map)); // Now clean.
    map.positions = 0;
    assert(!ring_select_sanitize(&map) && map.positions == 2 && map.ringtone[1] == RING_SELECT_CALLER);
}

static void test_resolve(void)
{
    ring_select_map_t map = {.positions = 6, .ringtone = {0, 2, 12, 99, 3, RING_SELECT_CALLER}};
    assert(ring_select_resolve(&map, 1, 4, have) == 2);    // Local choice wins.
    assert(ring_select_resolve(&map, 2, -1, have) == 12);  // Clip on this phone.
    assert(ring_select_resolve(&map, 3, 4, have) == 4);    // Missing clip: caller's choice.
    assert(ring_select_resolve(&map, 3, -1, have) == 0);   // ... or standard.
    assert(ring_select_resolve(&map, 5, 37, have) == 37);  // Last position: caller decides.
    assert(ring_select_resolve(&map, 5, 50, have) == 0);   // Caller's clip missing here.
    assert(ring_select_resolve(&map, 5, -1, have) == 0);   // Caller chose nothing.
    assert(ring_select_resolve(&map, -1, 3, have) == 3);   // Caller not on a button.
    assert(ring_select_resolve(&map, 9, 3, have) == 3);    // Out of range position.
}

static void test_position(void)
{
    const unsigned full = 4095, positions = 12;
    // Exact taps of an 11-resistor ladder read as their positions.
    for (unsigned p = 0; p < positions; ++p)
        assert(ring_select_position(p * full / (positions - 1), full, positions, -1) == p);
    assert(ring_select_position(5000, full, positions, -1) == 11); // Clamped.
    // Hysteresis: from position 3, the boundary to 4 sits at 3.5 steps; it
    // takes 3.7 steps to move, and coming back needs 3.3.
    unsigned step = full / (positions - 1);
    assert(ring_select_position(3 * step + step * 55 / 100, full, positions, 3) == 3);
    assert(ring_select_position(3 * step + step * 75 / 100, full, positions, 3) == 4);
    assert(ring_select_position(3 * step + step * 45 / 100, full, positions, 4) == 4);
    assert(ring_select_position(3 * step + step * 25 / 100, full, positions, 4) == 3);
    // A big jump moves immediately.
    assert(ring_select_position(9 * step, full, positions, 2) == 9);
    // Unknown previous: nearest position.
    assert(ring_select_position(3 * step + step * 55 / 100, full, positions, -1) == 4);
    assert(ring_select_position(100, 0, positions, 3) == 0);
}

int main(void)
{
    test_default_map();
    test_sanitize();
    test_resolve();
    test_position();
    puts("ring_select_test: default map, repair, resolve and ADC hysteresis passed");
    return 0;
}
