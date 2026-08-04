// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace scale_time {

// Sentinel for "this weight sample did not carry a scale timer".
inline constexpr uint32_t UNKNOWN_MS = 0xFFFFFFFFu;

// Acaia timer payloads are [minutes, seconds, deciseconds]. We store the
// decoded value in milliseconds for future compatibility and because the
// rest of the firmware's timing model is millisecond-based, but the effective
// scale-timer resolution is 100 ms.
inline constexpr uint32_t ACAIA_TIMER_RESOLUTION_MS = 100;

inline bool isKnown(uint32_t timerMs) { return timerMs != UNKNOWN_MS; }

}  // namespace scale_time
