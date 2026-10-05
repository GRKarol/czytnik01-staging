#pragma once

#include <Arduino.h>

// Watches PWR from its own task, so the reader can always be switched off,
// even with the main loop stuck (a long book index, a hung driver).
//
// - PWR held 10 s: the power is cut whatever the firmware is doing, the
//   reader's "battery out and back in". On battery the board goes dark when
//   PWR is let go; on USB power it restarts.
// - PWR held 2 s while the main loop has not run for 3 s: the same, since
//   the normal power-off in the main loop can't happen then.
namespace PowerGuard {

void begin();

// Called by the main loop on every pass.
void mainLoopAlive(uint32_t nowMs);

// The normal power-off is running (it blocks the main loop on purpose):
// only the 10 s hold acts until the reader is off.
void setShuttingDown(bool shuttingDown);

}  // namespace PowerGuard
