// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power/PowerSavingPolicy.h"

#include <M5Unified.h>

#include "power/PowerEventLog.h"
#include "util/i2c_lock.h"
#include "util/power.h"

namespace power {

PowerSavingPolicy::PowerSavingPolicy(PowerEventLog& eventLog)
    : _eventLog(eventLog), _lastActivityMs(millis()) {}

void PowerSavingPolicy::registerActivity() {
  _lastActivityMs = millis();
  if (_screenDimmed) {
    M5_LOGD("Display dimming OFF");
    M5.Display.setBrightness(_lastBrightness);
    _screenDimmed = false;
  }
}

void PowerSavingPolicy::update() {
  if (millis() - _lastActivityMs >
      static_cast<unsigned long>(_autoPowerOffTimeout)) {
    if (!shutdownIfOnBattery()) {
      M5_LOGD("On external power, extending last activity.");
      _lastActivityMs = millis();
    }
    return;
  }
  if (!_screenDimmed && millis() - _lastActivityMs >
                            static_cast<unsigned long>(_screenDimTimeout)) {
    M5_LOGD("Display dimming ON");
    _lastBrightness = M5.Display.getBrightness();
    M5.Display.setBrightness(1);
    _screenDimmed = true;
  }
}

bool PowerSavingPolicy::shutdownIfOnBattery() {
  // VIN is the reliable indication that shutdown must be suppressed. The PM1
  // charging flag turns false when a full battery remains plugged in and has
  // also produced false charging readings during shared-bus contention.
  if (getBatteryStatus().hasExternalPower) return false;

  return performShutdown();
}

bool PowerSavingPolicy::shutdown() { return performShutdown(); }

bool PowerSavingPolicy::performShutdown() {
  logBatteryStatus();
  recordSleepEvent(_eventLog);
  if (_preSleepCallback) _preSleepCallback();
  if (_wakeOnMotion) {
    M5_LOGD("Shutting down with wake-up on motion enabled.");
    // PM1 asserts its LDO power hold before the later motion-wake setup steps
    // can fail. Clear that partial configuration before the fallback power
    // off so the PMIC is left in a known state.
    if (enableWakeUpOnMotionAndShutdown()) return true;
    M5_LOGE("Motion-wake setup failed; unwinding PM1 and powering off.");
    disableWakeUpOnMotion();
  } else {
    M5_LOGD("Shutting down.");
  }
  {
    I2cLock lock;
    M5.Power.powerOff();
  }
  return true;
}

}  // namespace power
