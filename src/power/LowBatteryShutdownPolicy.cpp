// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power/LowBatteryShutdownPolicy.h"

namespace power {

LowBatteryShutdownPolicy::Decision LowBatteryShutdownPolicy::update(
    int voltageMv, bool hasExternalPower, uint32_t nowMs) {
  if (hasExternalPower) {
    reset();
    return Decision::None;
  }
  // A missing sample provides no evidence that the battery recovered. Keep an
  // existing confirmation interval so intermittent I2C failures cannot defer
  // shutdown indefinitely.
  if (voltageMv <= 0) return Decision::None;
  if (_shutdownRequested) return Decision::None;

  if (!_lowVoltageObserved) {
    if (voltageMv > kLowBatteryConfirmationVoltageMv) return Decision::None;
    _lowVoltageObserved = true;
    _lowVoltageStartedMs = nowMs;
    return Decision::None;
  }

  if (voltageMv >= kRecoveryVoltageMv) {
    reset();
    return Decision::None;
  }
  if (nowMs - _lowVoltageStartedMs < kConfirmationMs) return Decision::None;

  _shutdownRequested = true;
  return Decision::Shutdown;
}

void LowBatteryShutdownPolicy::reset() {
  _lowVoltageObserved = false;
  _shutdownRequested = false;
  _lowVoltageStartedMs = 0;
}

}  // namespace power
