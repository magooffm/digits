#pragma once

// Decodes the button ladder on one ADS1115 input: a 33 kOhm pull-up to
// 3.3 V, each button to GND through its own resistor (ring A 10k, ring B
// 20k, speaker 33k). Every combination gives its own level; the limits
// are the midpoints between neighbours.

#define BUTTON_LADDER_A 0x1
#define BUTTON_LADDER_B 0x2
#define BUTTON_LADDER_SPEAKER 0x4

static inline unsigned button_ladder_decode(int mv)
{
    static const struct { int below_mv; unsigned buttons; } levels[] = {
        {505, BUTTON_LADDER_A | BUTTON_LADDER_B | BUTTON_LADDER_SPEAKER}, // 0.46 V
        {585, BUTTON_LADDER_A | BUTTON_LADDER_B},                         // 0.55 V
        {695, BUTTON_LADDER_A | BUTTON_LADDER_SPEAKER},                   // 0.62 V
        {835, BUTTON_LADDER_A},                                           // 0.77 V
        {1075, BUTTON_LADDER_B | BUTTON_LADDER_SPEAKER},                  // 0.90 V
        {1450, BUTTON_LADDER_B},                                          // 1.25 V
        {2475, BUTTON_LADDER_SPEAKER},                                    // 1.65 V
    };
    // Far below every level: a short or a missing pull-up, not a press.
    if (mv < 300) return 0;
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i)
        if (mv < levels[i].below_mv) return levels[i].buttons;
    return 0; // 3.3 V: nothing pressed.
}
