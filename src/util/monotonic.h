// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// Successive calls with the same ts (and increasing lastTs) will return
// monotonically increasing timestamps. It keeps 0 unused so that it can be
// designed as a sentinel.
inline uint32_t ensureMonotonicTimestamp(uint32_t ts, uint32_t lastTs) {
  if ((int32_t)(ts - lastTs) <= 0) ts = lastTs + 1;
  if (ts == 0) ++ts;
  return ts;
}

// Returns ts unless it is in the future relative to nowMs (wrap-safe), in
// which case it returns nowMs. BLE sample timestamps can be nudged ahead of
// the host millis() clock by ensureMonotonicTimestamp, so callers that treat
// a sample time as "no later than now" must clamp it first.
inline uint32_t clampNotFuture(uint32_t ts, uint32_t nowMs) {
  return (int32_t)(ts - nowMs) > 0 ? nowMs : ts;
}

// Milliseconds elapsed from pastMs to nowMs, clamped to 0 when pastMs is in
// the future relative to nowMs (see clampNotFuture). Avoids the unsigned
// underflow to ~4.29e9 that a bare nowMs - pastMs produces for future stamps.
inline uint32_t elapsedSinceClamped(uint32_t nowMs, uint32_t pastMs) {
  return nowMs - clampNotFuture(pastMs, nowMs);
}