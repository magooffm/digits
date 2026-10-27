// Host tests for silence_policy.h: the most recent change wins.
#include "silence_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    silence_policy_t p = {0};
    assert(!silence_policy_effective(&p));
    silence_policy_local(&p, true);               // Button: silent.
    assert(silence_policy_effective(&p) && p.overridden);
    silence_policy_server(&p, false);             // Same server value again: kept.
    assert(silence_policy_effective(&p));
    silence_policy_server(&p, true);              // Web/quiet hours change: server wins.
    assert(silence_policy_effective(&p) && !p.overridden);
    silence_policy_local(&p, false);              // Button: ring during quiet hours.
    assert(!silence_policy_effective(&p) && p.overridden);
    silence_policy_server(&p, true);
    assert(!silence_policy_effective(&p));
    silence_policy_server(&p, false);             // Quiet hours end.
    assert(!silence_policy_effective(&p) && !p.overridden);
    silence_policy_server(&p, true);              // Next quiet hours start.
    assert(silence_policy_effective(&p));
    silence_policy_local(&p, true);               // Same as the server: no override.
    assert(!p.overridden);
    silence_policy_server(&p, false);
    assert(!silence_policy_effective(&p));
    puts("silence_policy tests passed");
    return 0;
}
