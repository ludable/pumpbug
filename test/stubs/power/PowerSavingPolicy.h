// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

unsigned long millis();
extern unsigned powerSavingShutdownRequests;
extern unsigned powerSavingExplicitShutdownRequests;
extern unsigned powerSavingConditionalShutdownRequests;
extern bool powerSavingShutdownSucceeds;

namespace power {

class PowerSavingPolicy {
 public:
  PowerSavingPolicy() : _lastActivityMs(millis()) {}

  void setScreenDimmingTimeoutSeconds(long seconds = 60) {
    _screenDimTimeoutMs = seconds * 1000;
  }
  void disableScreenDimming() { _screenDimTimeoutMs = -1; }
  bool isScreenDimmed() const { return _screenDimmed; }

  void setAutoPowerOffTimeoutSeconds(long, bool = true) {}
  void disableAutoPowerOff() {}
  void setPreSleepCallback(void (*)()) {}
  bool shutdown() {
    ++powerSavingShutdownRequests;
    ++powerSavingExplicitShutdownRequests;
    return powerSavingShutdownSucceeds;
  }
  bool shutdownIfOnBattery() {
    ++powerSavingShutdownRequests;
    ++powerSavingConditionalShutdownRequests;
    return powerSavingShutdownSucceeds;
  }

  void registerActivity() {
    _lastActivityMs = millis();
    _screenDimmed = false;
  }

  void update() {
    if (_screenDimTimeoutMs >= 0 &&
        millis() - _lastActivityMs >
            static_cast<unsigned long>(_screenDimTimeoutMs)) {
      _screenDimmed = true;
    }
  }

 private:
  long _screenDimTimeoutMs = 60 * 1000;
  unsigned long _lastActivityMs = 0;
  bool _screenDimmed = false;
};

}  // namespace power
