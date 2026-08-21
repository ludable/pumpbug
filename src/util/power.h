// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace power {

class PowerEventLog;

// Wakes the StickS3 PM1 I2C interface before M5.begin() probes it. This must
// run before M5.begin(); normal PM1 access uses the initialized shared bus and
// the I2C lock instead.
void preparePM1ForBoot();

enum class ChargingState : int8_t {
  Unknown = -1,
  NotCharging = 0,
  Charging = 1,
};

struct BatteryStatus {
  int percent;    // Estimated usable charge, or -1 if the reading failed
  int voltageMv;  // Battery terminal voltage, or -1 if the reading failed
  bool hasExternalPower;
  ChargingState charging;
};

struct PowerButtonSample {
  bool wasPressed = false;
  bool isPressed = false;
};

inline constexpr uint32_t kPowerButtonDoubleClickWindowMs = 500;

// Reads the related PM1 battery and charging values without allowing another
// task to use the shared I2C bus between them.
BatteryStatus getBatteryStatus();

void logBatteryStatus();

// Records wake and sleep events with battery and wall-clock state. Wake events
// also include the ESP32 reset reason.
void recordWakeEvent(PowerEventLog& eventLog);
void recordSleepEvent(PowerEventLog& eventLog);

// Backfill this boot's Wake after SNTP makes UTC available. No-op if the Wake
// already had an RTC timestamp or no current-boot Wake is pending.
void backfillWakeTimestamp(PowerEventLog& eventLog);

bool enableWakeUpOnMotionAndShutdown();
bool disableWakeUpOnMotion();

// Gives firmware the PM1 press flag while leaving PM1's double-click power-off
// behavior unchanged. M5Unified does not expose the StickS3 PM1 through
// M5.BtnPWR.
void configurePowerButton();
bool pollPowerButton(PowerButtonSample& sample);

bool setStatusLedEnabled(bool enabled);

// Controls the StickS3 external 5 V output through the PM1. The output supplies
// Grove, Hat2 EXT_5V, and IR; it does not supply the built-in peripherals.
bool setExtPowerEnabled(bool enabled);

}  // namespace power
