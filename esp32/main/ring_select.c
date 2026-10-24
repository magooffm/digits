#include "ring_select.h"

#define HYSTERESIS_PERCENT 20

static unsigned clamp_positions(unsigned positions)
{
    if (positions < RING_SELECT_MIN_POSITIONS) return RING_SELECT_MIN_POSITIONS;
    if (positions > RING_SELECT_MAX_POSITIONS) return RING_SELECT_MAX_POSITIONS;
    return positions;
}

void ring_select_default_map(ring_select_map_t *map, unsigned positions,
                             const unsigned *available, unsigned count)
{
    map->positions = clamp_positions(positions);
    for (unsigned p = 0; p < RING_SELECT_MAX_POSITIONS; ++p)
        map->ringtone[p] = p < count && available[p] < RING_SELECT_CALLER ? (uint8_t)available[p] : 0;
    map->ringtone[map->positions - 1] = RING_SELECT_CALLER;
}

bool ring_select_sanitize(ring_select_map_t *map)
{
    bool clean = true;
    unsigned positions = clamp_positions(map->positions);
    if (positions != map->positions) {
        map->positions = positions;
        clean = false;
    }
    for (unsigned p = 0; p + 1 < positions; ++p)
        if (map->ringtone[p] == RING_SELECT_CALLER) {
            map->ringtone[p] = 0;
            clean = false;
        }
    if (map->ringtone[positions - 1] != RING_SELECT_CALLER) {
        map->ringtone[positions - 1] = RING_SELECT_CALLER;
        clean = false;
    }
    return clean;
}

unsigned ring_select_resolve(const ring_select_map_t *map, int position, int offered,
                             bool (*valid)(unsigned ringtone))
{
    unsigned fallback = offered >= 0 && valid((unsigned)offered) ? (unsigned)offered : 0;
    if (position < 0 || (unsigned)position >= map->positions) return fallback;
    uint8_t chosen = map->ringtone[position];
    if (chosen == RING_SELECT_CALLER || !valid(chosen)) return fallback;
    return chosen;
}

unsigned ring_select_position(unsigned raw, unsigned full_scale, unsigned positions, int previous)
{
    positions = clamp_positions(positions);
    if (!full_scale) return 0;
    if (raw > full_scale) raw = full_scale;
    // Scaled by 100 to keep the hysteresis in integer steps.
    unsigned long long scaled = (unsigned long long)raw * (positions - 1) * 100 / full_scale;
    unsigned nearest = (unsigned)((scaled + 50) / 100);
    if (previous < 0 || (unsigned)previous >= positions || (unsigned)previous == nearest) return nearest;
    // Stay put unless the reading is clearly past the midpoint toward nearest.
    unsigned long long here = (unsigned long long)previous * 100;
    unsigned long long distance = scaled > here ? scaled - here : here - scaled;
    return distance >= 50 + HYSTERESIS_PERCENT ? nearest : (unsigned)previous;
}
