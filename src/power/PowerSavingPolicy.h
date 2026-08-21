// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace power {

class PowerEventLog;

// Applies the display and shutdown policy after the device has been idle.
class PowerSavingPolicy {
 public:
  explicit PowerSavingPolicy(PowerEventLog& eventLog);

  void setScreenDimmingTimeoutSeconds(long seconds = 60) {
    _screenDimTimeout = seconds * 1000;
  }
  void disableScreenDimming() { _screenDimTimeout = -1; }
  bool isScreenDimmed() const { return _screenDimmed; }

  void setAutoPowerOffTimeoutSeconds(long seconds = 5 * 60,
                                     bool wakeOnMotion = true) {
    _autoPowerOffTimeout = seconds * 1000;
    _wakeOnMotion = wakeOnMotion;
  }
  void disableAutoPowerOff() { _autoPowerOffTimeout = -1; }

  void registerActivity();
  void update();

  // Runs the product shutdown sequence in response to an explicit user
  // request. PM1 keeps the StickS3 off with VIN present; this behavior is
  // verified on hardware rather than inferred from its register interface.
  bool shutdown();
  // Runs the product shutdown sequence when the device is on battery power.
  // Motion wake, persistent event logging, and the network notification are
  // shared by idle and low-battery shutdowns.
  bool shutdownIfOnBattery();

  // Gives network clients an opportunity to observe imminent sleep. The
  // callback must complete within a few hundred milliseconds.
  void setPreSleepCallback(void (*callback)()) { _preSleepCallback = callback; }

 private:
  PowerEventLog& _eventLog;
  long _screenDimTimeout = 60 * 1000;
  long _autoPowerOffTimeout = 5 * 60 * 1000;
  bool _wakeOnMotion = true;
  unsigned long _lastActivityMs;
  unsigned short _lastBrightness = 0;
  bool _screenDimmed = false;
  void (*_preSleepCallback)() = nullptr;

  bool performShutdown();
};

}  // namespace power
