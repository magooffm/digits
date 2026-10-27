#pragma once

// Local silence (the planned silence button) against the server's effective
// silent flag (web toggle or quiet hours): the most recent change wins. A
// local choice holds until the server value changes; choosing what the
// server already says needs no override. Pure, header only:
// tests/silence_policy_test.c runs on the host.

#include <stdbool.h>

typedef struct {
    bool server;     // Last effective flag from the server.
    bool overridden; // A local choice is newer than the server's value.
    bool local;
} silence_policy_t;

static inline void silence_policy_server(silence_policy_t *p, bool server)
{
    if (server != p->server) p->overridden = false; // Repeats keep the override.
    p->server = server;
}

static inline void silence_policy_local(silence_policy_t *p, bool silent)
{
    p->local = silent;
    p->overridden = silent != p->server;
}

static inline bool silence_policy_effective(const silence_policy_t *p)
{
    return p->overridden ? p->local : p->server;
}
