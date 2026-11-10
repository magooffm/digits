// Host tests for button_ladder_map.h: each level from the build guide, with
// 3 % either way, decodes to its buttons.
#include "button_ladder_map.h"
#include <assert.h>
#include <stdio.h>

#define A BUTTON_LADDER_A
#define B BUTTON_LADDER_B
#define S BUTTON_LADDER_SPEAKER

int main(void)
{
    static const struct { int mv; unsigned buttons; } cases[] = {
        {3300, 0}, {460, A | B | S}, {550, A | B}, {620, A | S}, {770, A}, {900, B | S}, {1250, B}, {1650, S},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        for (int percent = -3; percent <= 3; percent += 3) {
            int mv = cases[i].mv + cases[i].mv * percent / 100;
            if (button_ladder_decode(mv) != cases[i].buttons) {
                printf("%d mV: got 0x%x, want 0x%x\n", mv, button_ladder_decode(mv), cases[i].buttons);
                return 1;
            }
        }
    assert(button_ladder_decode(0) == 0);   // Shorted input.
    assert(button_ladder_decode(3100) == 0); // A lower supply.
    puts("button_ladder tests passed");
    return 0;
}
