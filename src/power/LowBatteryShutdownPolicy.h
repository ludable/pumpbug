// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace power {

// On-device testing found audio and replay unstable below this voltage.
inline constexpr int kLowBatteryConfirmationVoltageMv = 3450;

// Detects sustained battery voltage below the product's stable operating
// region. The policy is independent of the hardware shutdown path so its
// timing and recovery behavior can be verified on the host.
class LowBatteryShutdownPolicy {
 public:
  enum class Decision : uint8_t { None, Shutdown };

  Decision update(int voltageMv, bool hasExternalPower, uint32_t nowMs);
  void reset();
  bool confirmationActive() const { return _lowVoltageObserved; }

 private:
  static constexpr int kRecoveryVoltageMv = 3500;
  static constexpr uint32_t kConfirmationMs = 15000;

  bool _lowVoltageObserved = false;
  bool _shutdownRequested = false;
  uint32_t _lowVoltageStartedMs = 0;
};

}  // namespace power
