// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace ble {

// Schedules initialization attempts for a required Bluetooth service. A
// failed attempt waits before retrying so a persistent allocation or
// controller failure cannot consume the main loop.
class BleStartupRetryPolicy {
 public:
  bool shouldAttempt(bool required, bool started, uint32_t nowMs) {
    if (!required || started) {
      _attempted = false;
      return false;
    }
    if (_attempted && nowMs - _lastAttemptMs < kRetryIntervalMs) return false;
    _attempted = true;
    _lastAttemptMs = nowMs;
    return true;
  }

 private:
  static constexpr uint32_t kRetryIntervalMs = 5000;

  uint32_t _lastAttemptMs = 0;
  bool _attempted = false;
};

}  // namespace ble
