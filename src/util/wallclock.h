// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// Wall-clock source for the firmware. Layers two sources behind a single
// utcNow() accessor:
//
//   1. On-board RTC (BM8563 on M5Stick variants that ship with one) —
//      survives reboot via its backup capacitor. Seeded into the system
//      clock at boot.
//
//   2. SNTP — fired once per boot when STA first comes up. After a
//      successful sync, the system clock is also written back to the
//      RTC if one is present, so the next boot starts with a fresh
//      reference even before STA reconnects.
//
// Until one of those provides a plausible time (epoch ≥ 2025-01-01),
// utcNow() returns 0 — callers (ExtractionRecorder, log timestamps)
// treat that as "unknown" rather than a real datetime.
//
// Named `wallclock` (not `clock`) to avoid colliding with the C library's
// global clock() function in <time.h>.
namespace wallclock {

// Seed the system clock from the RTC if available. Idempotent; safe to
// call once during setup(). No-op when no RTC hardware is detected.
void initFromRtc();

// Drive the SNTP / RTC bookkeeping. Cheap; call once per main-loop
// iteration. Kicks off SNTP on the first STA-connected transition and
// writes the freshly synced time back to the RTC.
void update();

// Unix epoch seconds. Returns 0 when the wall clock isn't usable yet.
uint32_t utcNow();

// True iff utcNow() will return a non-zero value right now.
bool isSet();

}  // namespace wallclock
