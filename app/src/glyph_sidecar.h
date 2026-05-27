#ifndef SC_GLYPH_SIDECAR_H
#define SC_GLYPH_SIDECAR_H

#include "common.h"

#include <stdbool.h>

// Spawn the external glyph viewer sidecar. `serial` may be NULL/"" to let
// adb auto-pick the only attached device. Non-essential: failures are logged
// but never abort scrcpy.
bool
sc_glyph_sidecar_spawn(const char *serial);

// Send SIGTERM to the sidecar (if any) and wait briefly. Safe to call even
// if spawn was never invoked or failed.
void
sc_glyph_sidecar_stop(void);

#endif
