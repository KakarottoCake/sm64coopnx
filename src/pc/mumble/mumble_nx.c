#ifdef __SWITCH__

#include "mumble.h"

// Mumble positional-audio integration talks to the desktop Mumble app over
// shared memory (mumble.c, excluded for NX - no sys/mman.h, and no Mumble
// client to talk to on a Switch anyway). No-op stand-in so the rest of the
// engine, which calls these unconditionally, still links.

void mumble_init(void) {
}

void mumble_update(void) {
}

#endif // __SWITCH__
