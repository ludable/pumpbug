// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "power/LowBatteryShutdownPolicy.h"
#include "power/PowerSavingPolicy.h"
#include "power/ScaleRadioStateMachine.h"

class BleScaleService;

namespace power {

// Applies the device-wide power policy from activity and resource requirements.
class PowerManager {
 public:
  struct PerformanceStatus {
    uint32_t actualCpuMhz = 0;
    uint32_t loopDelayMs = 0;
    bool adaptivePerformanceEnabled = false;
    bool fullPerformanceRequired = false;
  };

  explicit PowerManager(BleScaleService& scaleService);

  // Reports one activity event and permits one retry of a failed radio wake.
  void notifyActivity();
  // Extends the idle timers without permitting a radio-wake retry.
  void keepAwake();
  // Reports whether the foreground workload needs full CPU and loop speed.
  void setForegroundFullPerformanceRequired(bool required);
  // Reports an application-level requirement independent of foreground work.
  void setApplicationFullPerformanceRequired(bool required);
  void update();

  void enableAdaptivePerformance();
  void enableStatusLedPowerSaving();
  void enableLowBatteryShutdown();
  uint32_t loopDelayMs() const;
  PerformanceStatus performanceStatus() const;

  // Selects how the scale's Bluetooth controller is managed. Fixed policies
  // hold it running or stopped; the demand policy follows scale use and display
  // state.
  //
  // External control can begin only while the controller is running and no
  // transition or service suspension is active. That transfer lasts until
  // reboot. An invalid transfer leaves the existing policy unchanged.
  [[nodiscard]] bool setScaleRadioPolicy(ScaleRadioPolicy policy);

  // Returns controller state and policy progress from the same observation.
  ScaleRadioStatus scaleRadioStatus() const { return _radioStatus; }

  bool isScreenDimmed() const;
  bool isStatusLedEnabled() const;
  // Disables automatic display dimming, shutdown, CPU scaling, loop pacing, and
  // status-LED power saving. It restores full CPU speed and turns the status
  // LED on before automatic control stops. The Bluetooth controller continues
  // to follow the selected radio policy.
  void disableIdlePowerSaving();
  // Prevents power-button presses from changing measurement conditions.
  // Automatic idle and low-battery policy remain active.
  void disablePowerButtonSleep();
  void setScreenDimmingTimeoutSeconds(long seconds = 60);
  void setPreSleepCallback(void (*callback)());

 private:
  struct PerformanceTarget {
    uint16_t cpuMhz;
    uint32_t loopDelayMs;
  };

  using RadioDecision = ScaleRadioStateMachine::Decision;
  using RadioEvent = ScaleRadioStateMachine::Event;

  // Tracks a controller restart until the scale becomes ready.
  struct WakeTiming {
    uint32_t startedMs = 0;
    bool awaitingScale = false;
  };

  BleScaleService& _scaleService;
  PowerSavingPolicy _savingPolicy;
  LowBatteryShutdownPolicy _lowBatteryShutdown;
  ScaleRadioStateMachine _scaleRadio;

  static constexpr uint32_t kIdleLoopDelayMs = 10;
  static constexpr uint32_t kBatteryPollIntervalMs = 5000;
  static constexpr uint32_t kPowerButtonPollIntervalMs = 200;
  static constexpr uint16_t kReducedCpuMhz = 80;
  static constexpr uint16_t kFullCpuMhz = 240;
  bool _adaptivePerformanceEnabled = true;
  bool _foregroundFullPerformanceRequired = false;
  bool _applicationFullPerformanceRequired = false;
  uint16_t _appliedCpuMhz = 0;
  uint16_t _failedCpuTargetMhz = 0;
  bool _statusLedPowerSavingEnabled = true;
  bool _lowBatteryShutdownEnabled = true;
  bool _lowBatteryDisplayModeEnabled = false;
  bool _statusLedEnabled = true;
  bool _batteryPollStarted = false;
  uint32_t _lastBatteryPollMs = 0;
  bool _powerButtonPollStarted = false;
  bool _powerButtonReadFailed = false;
  bool _powerButtonSleepEnabled = true;
  bool _powerButtonDown = false;
  bool _powerButtonPressPending = false;
  uint32_t _lastPowerButtonPollMs = 0;
  uint32_t _powerButtonReleasedMs = 0;

  void applyCpuPolicy();
  bool applyStatusLedState(bool enabled);
  void applyLowBatteryDisplayMode(bool enabled);
  PerformanceTarget performanceTarget() const;
  bool fullPerformanceRequired() const;
  void retryCpuPolicy();
  void disableAdaptivePerformance();
  void disableStatusLedPowerSaving();
  void disableLowBatteryShutdown();
  void updateBatteryPolicies(uint32_t nowMs);
  bool updatePowerButton(uint32_t nowMs);
  ScaleRadioStateMachine::Inputs radioInputs(uint32_t nowMs) const;
  void applyRadioDecision(RadioDecision decision, uint32_t nowMs);
  void applyRadioSuspension();
  void refreshRadioStatus(uint32_t nowMs);
  ScaleRadioStatus _radioStatus;
  bool _appliedServiceSuspended = false;
  WakeTiming _wakeTiming;
  const char* radioContext() const;
  void reportRadioEvent(const RadioEvent& event, uint32_t nowMs);
  void reportScaleReadyAfterWake(uint32_t nowMs);
};

extern PowerManager powerManager;

}  // namespace power
