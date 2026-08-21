// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power/PowerManager.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp32-hal-cpu.h>

#include <cstddef>

#include "ble/BleScaleService.h"
#include "diagnostics/RuntimeEventLog.h"
#include "power/SpeakerCodec.h"
#include "util/power.h"

namespace power {

namespace {
using Action = ScaleRadioStateMachine::Action;
using EventKind = ScaleRadioStateMachine::EventKind;
using FailureArea = ScaleRadioStateMachine::FailureArea;
using ServiceState = ScaleServiceState;

constexpr diagnostics::BleFailureCode kFailureCodes[] = {
    diagnostics::BleFailureCode::ScaleServiceStop,
    diagnostics::BleFailureCode::ScaleControllerStop,
    diagnostics::BleFailureCode::ScaleControllerStart,
    diagnostics::BleFailureCode::ScaleControllerUnusable,
};

constexpr std::size_t kFailureCodeCount =
    sizeof(kFailureCodes) / sizeof(kFailureCodes[0]);
static_assert(kFailureCodeCount ==
              static_cast<std::size_t>(FailureArea::Count));

ServiceState serviceState(const BleScaleService::Snapshot& scale) {
  switch (scale.state) {
    case BleScaleService::State::OFF:
      return ServiceState::Off;
    case BleScaleService::State::SCANNING:
    case BleScaleService::State::RECONNECTING:
      return ServiceState::Disconnected;
    case BleScaleService::State::CONNECTING:
      return ServiceState::Connecting;
    case BleScaleService::State::READY:
      return ServiceState::Ready;
  }
  return ServiceState::Off;
}
}  // namespace

PowerManager::PowerManager(BleScaleService& scaleService)
    : _scaleService(scaleService), _savingPolicy(_eventLog) {}

void PowerManager::notifyActivity() {
  _savingPolicy.registerActivity();
  _scaleRadio.notifyActivity();
  retryCpuPolicy();
}

void PowerManager::keepAwake() {
  _savingPolicy.registerActivity();
  applyCpuPolicy();
}

void PowerManager::setForegroundFullPerformanceRequired(bool required) {
  if (_foregroundFullPerformanceRequired == required) return;
  _foregroundFullPerformanceRequired = required;
  retryCpuPolicy();
}

void PowerManager::setApplicationFullPerformanceRequired(bool required) {
  if (_applicationFullPerformanceRequired == required) return;
  _applicationFullPerformanceRequired = required;
  retryCpuPolicy();
}

void PowerManager::enableAdaptivePerformance() {
  _adaptivePerformanceEnabled = true;
  retryCpuPolicy();
}

void PowerManager::enableStatusLedPowerSaving() {
  _statusLedPowerSavingEnabled = true;
  _batteryPollStarted = false;
}

void PowerManager::enableLowBatteryShutdown() {
  _lowBatteryShutdownEnabled = true;
  _lowBatteryShutdown.reset();
  _batteryPollStarted = false;
}

bool PowerManager::setScaleRadioPolicy(ScaleRadioPolicy policy) {
  const uint32_t nowMs = millis();
  if (policy == ScaleRadioPolicy::ExternalControl &&
      _scaleRadio.policy() != ScaleRadioPolicy::ExternalControl &&
      radioInputs(nowMs).controller != ble::ControllerState::Running) {
    M5_LOGE(
        "Scale Bluetooth controller: external-control handover refused; the "
        "controller is not running");
    return false;
  }
  if (!_scaleRadio.setPolicy(policy)) {
    M5_LOGE("Scale Bluetooth controller: radio policy change refused");
    return false;
  }
  applyRadioSuspension();
  // The status has to answer for the new policy immediately, not from the next
  // loop: a phase that selects a policy and asks whether it has settled in the
  // same breath must be told no.
  refreshRadioStatus(nowMs);
  return true;
}

void PowerManager::refreshRadioStatus(uint32_t nowMs) {
  const ScaleRadioStateMachine::Inputs inputs = radioInputs(nowMs);
  ScaleRadioStatus status;
  status.policy = _scaleRadio.policy();
  status.controller = inputs.controller;
  status.shouldRun =
      radioTarget(status.policy, ScaleRadioStateMachine::conditionsOf(inputs));
  status.failed = _scaleRadio.hasFailed();
  const bool controllerAtTarget =
      status.shouldRun ? status.controller == ble::ControllerState::Running
                       : status.controller == ble::ControllerState::Stopped;
  status.settled =
      !status.failed && !_scaleRadio.inTransition() && controllerAtTarget;
  _radioStatus = status;
}

uint32_t PowerManager::loopDelayMs() const {
  return performanceTarget().loopDelayMs;
}

PowerManager::PerformanceStatus PowerManager::performanceStatus() const {
  const PerformanceTarget target = performanceTarget();
  return {getCpuFrequencyMhz(), target.loopDelayMs, _adaptivePerformanceEnabled,
          fullPerformanceRequired()};
}

PowerManager::PerformanceTarget PowerManager::performanceTarget() const {
  const bool fullPerformance = fullPerformanceRequired();
  const uint32_t loopDelayMs =
      _adaptivePerformanceEnabled && !fullPerformance ? kIdleLoopDelayMs : 0;
  const uint16_t cpuMhz = _adaptivePerformanceEnabled &&
                                  _savingPolicy.isScreenDimmed() &&
                                  !fullPerformance
                              ? kReducedCpuMhz
                              : kFullCpuMhz;
  return {cpuMhz, loopDelayMs};
}

bool PowerManager::fullPerformanceRequired() const {
  return _foregroundFullPerformanceRequired ||
         _applicationFullPerformanceRequired;
}

void PowerManager::applyCpuPolicy() {
  if (!_adaptivePerformanceEnabled) return;
  const uint16_t targetMhz = performanceTarget().cpuMhz;
  if (_appliedCpuMhz == targetMhz) return;
  if (_failedCpuTargetMhz == targetMhz) return;
  if (!setCpuFrequencyMhz(targetMhz)) {
    M5_LOGE("Power manager: failed to set CPU frequency to %u MHz",
            static_cast<unsigned>(targetMhz));
    _failedCpuTargetMhz = targetMhz;
    return;
  }
  _appliedCpuMhz = targetMhz;
  _failedCpuTargetMhz = 0;
  M5_LOGI("Power manager: CPU frequency set to %u MHz",
          static_cast<unsigned>(targetMhz));
}

void PowerManager::retryCpuPolicy() {
  _failedCpuTargetMhz = 0;
  applyCpuPolicy();
}

void PowerManager::disableAdaptivePerformance() {
  _adaptivePerformanceEnabled = false;
  _failedCpuTargetMhz = 0;
  if (!setCpuFrequencyMhz(kFullCpuMhz)) {
    M5_LOGE("Power manager: failed to restore CPU frequency to %u MHz",
            static_cast<unsigned>(kFullCpuMhz));
  }
  // Direct frequency changes are allowed while automatic control is disabled,
  // so the cached value may differ from the hardware.
  _appliedCpuMhz = 0;
}

bool PowerManager::applyStatusLedState(bool enabled) {
  if (_statusLedEnabled == enabled) return true;
  if (!setStatusLedEnabled(enabled)) {
    M5_LOGE("Power manager: failed to set PM1 status LED %s",
            enabled ? "on" : "off");
    return false;
  }
  _statusLedEnabled = enabled;
  M5_LOGI("Power manager: PM1 status LED %s", enabled ? "on" : "off");
  return true;
}

void PowerManager::applyLowBatteryDisplayMode(bool enabled) {
  if (_lowBatteryDisplayModeEnabled == enabled) return;
  _lowBatteryDisplayModeEnabled = enabled;
  if (enabled)
    M5.Display.powerSaveOn();
  else
    M5.Display.powerSaveOff();
}

void PowerManager::disableStatusLedPowerSaving() {
  _statusLedPowerSavingEnabled = false;
  applyStatusLedState(true);
}

void PowerManager::updateBatteryPolicies(uint32_t nowMs) {
  if (!_statusLedPowerSavingEnabled && !_lowBatteryShutdownEnabled) return;
  if (_batteryPollStarted &&
      nowMs - _lastBatteryPollMs < kBatteryPollIntervalMs) {
    return;
  }
  _batteryPollStarted = true;
  _lastBatteryPollMs = nowMs;

  const BatteryStatus battery = getBatteryStatus();
  if (_statusLedPowerSavingEnabled)
    applyStatusLedState(battery.hasExternalPower);

  if (!_lowBatteryShutdownEnabled) return;

  const LowBatteryShutdownPolicy::Decision lowBatteryDecision =
      _lowBatteryShutdown.update(battery.voltageMv, battery.hasExternalPower,
                                 nowMs);
  applyLowBatteryDisplayMode(_lowBatteryShutdown.confirmationActive());
  if (lowBatteryDecision == LowBatteryShutdownPolicy::Decision::Shutdown) {
    M5_LOGW("Power manager: sustained low battery at %d mV; shutting down",
            battery.voltageMv);
    if (!_savingPolicy.shutdownIfOnBattery()) _lowBatteryShutdown.reset();
  }
}

bool PowerManager::updatePowerButton(uint32_t nowMs) {
  if (!_powerButtonSleepEnabled) return false;

  if (!_powerButtonPollStarted ||
      nowMs - _lastPowerButtonPollMs >= kPowerButtonPollIntervalMs) {
    _powerButtonPollStarted = true;
    _lastPowerButtonPollMs = nowMs;

    PowerButtonSample sample;
    if (!pollPowerButton(sample)) {
      if (!_powerButtonReadFailed)
        M5_LOGE("Power button: failed to read button state");
      _powerButtonReadFailed = true;
    } else {
      _powerButtonReadFailed = false;
      if (sample.wasPressed || sample.isPressed) {
        _powerButtonDown = true;
        // PM1 retains control while the button is held and while a second
        // click may still turn the device off.
        _powerButtonPressPending = false;
      }
      if (_powerButtonDown && !sample.isPressed) {
        _powerButtonDown = false;
        _powerButtonPressPending = true;
        _powerButtonReleasedMs = nowMs;
      }
    }
  }

  if (!_powerButtonPressPending ||
      nowMs - _powerButtonReleasedMs <
          kPowerButtonDoubleClickWindowMs + kPowerButtonPollIntervalMs) {
    return false;
  }

  _powerButtonPressPending = false;
  M5_LOGI("Power button: sleep requested");
  return _savingPolicy.shutdown();
}

ScaleRadioStateMachine::Inputs PowerManager::radioInputs(uint32_t nowMs) const {
  const BleScaleService::DemandSnapshot demand = _scaleService.demandSnapshot();
  ScaleRadioStateMachine::Inputs inputs;
  inputs.service = serviceState(_scaleService.snapshot());
  inputs.controller = _scaleService.controllerState();
  inputs.nowMs = nowMs;
  inputs.screenDimmed = _savingPolicy.isScreenDimmed();
  inputs.connectWanted = demand.connectWanted;
  inputs.diagnosticScanWanted = demand.diagnosticScanWanted;
  return inputs;
}

// The state machine reports controller transitions. The selected policy
// explains why they happened: Demand responds to service demand and display
// state, while a forced policy holds a measurement condition.
const char* PowerManager::radioContext() const {
  return _scaleRadio.policy() == ScaleRadioPolicy::Demand ? "demand policy"
                                                          : "forced phase";
}

void PowerManager::reportRadioEvent(const RadioEvent& event, uint32_t nowMs) {
  switch (event.kind) {
    case EventKind::None:
      return;
    case EventKind::StopStarted:
      M5_LOGI("Scale Bluetooth controller (%s): releasing", radioContext());
      // A wake that never finished is not worth timing to its replacement.
      _wakeTiming = {};
      return;
    case EventKind::ControllerStopped:
      M5_LOGI("Scale Bluetooth controller (%s): stopped", radioContext());
      return;
    case EventKind::WakeRequested:
      M5_LOGI("Scale Bluetooth controller (%s): wanted again", radioContext());
      _wakeTiming.startedMs = nowMs;
      return;
    case EventKind::ControllerRestarted:
      M5_LOGI("Scale Bluetooth controller (%s): restarted in %u ms",
              radioContext(),
              static_cast<unsigned>(nowMs - _wakeTiming.startedMs));
      // Only a wake driven by Connect demand has a scale still to arrive.
      _wakeTiming.awaitingScale = _scaleService.demandSnapshot().connectWanted;
      return;
    case EventKind::Failure:
      break;
  }

  const std::size_t index = static_cast<std::size_t>(event.failureArea);
  if (index >= kFailureCodeCount) {
    M5_LOGE("Scale power: invalid failure area");
    return;
  }
  M5_LOGE("Scale power: %s", event.reason ? event.reason : "unknown failure");
  runtimeEventLog.pushNetFailure(diagnostics::NetSource::Ble,
                                 static_cast<uint16_t>(kFailureCodes[index]),
                                 event.reason);
}

void PowerManager::applyRadioDecision(RadioDecision decision, uint32_t nowMs) {
  // A valid chain terminates before this guard. The limit prevents a malformed
  // transition from monopolizing the main loop.
  constexpr unsigned kDecisionCycleLimit = 4;
  for (unsigned step = 0; step < kDecisionCycleLimit; ++step) {
    reportRadioEvent(decision.event, nowMs);
    switch (decision.action) {
      case Action::None:
        return;
      case Action::StopController: {
        const bool accepted = _scaleService.requestControllerStop();
        decision = _scaleRadio.resolve(Action::StopController, accepted,
                                       radioInputs(nowMs));
        break;
      }
      case Action::StartController: {
        const bool accepted = _scaleService.requestControllerStart();
        decision = _scaleRadio.resolve(Action::StartController, accepted,
                                       radioInputs(nowMs));
        break;
      }
    }
  }
  M5_LOGE("Scale power: decision chain exceeded bound");
}

// Closes out the wake timing when the scale the wake was for actually arrives.
// The state machine has no interest in this; it only affects what is logged.
void PowerManager::reportScaleReadyAfterWake(uint32_t nowMs) {
  if (!_wakeTiming.awaitingScale) return;
  if (serviceState(_scaleService.snapshot()) != ScaleServiceState::Ready)
    return;
  M5_LOGI("Scale Bluetooth controller: scale ready after %u ms",
          static_cast<unsigned>(nowMs - _wakeTiming.startedMs));
  _wakeTiming.awaitingScale = false;
}

void PowerManager::applyRadioSuspension() {
  const bool suspended = _scaleRadio.wantsServiceSuspended();
  if (suspended == _appliedServiceSuspended) return;
  _appliedServiceSuspended = suspended;
  _scaleService.setPowerSuspended(suspended);
}

void PowerManager::update() {
  const uint32_t nowMs = millis();
  // An accepted shutdown command should not start more policy work while PM1
  // is removing power.
  if (updatePowerButton(nowMs)) return;
  _savingPolicy.update();
  applyCpuPolicy();
  updateBatteryPolicies(nowMs);
  speakerCodec.tick(nowMs);
  if (!_scaleService.isStarted()) return;
  applyRadioDecision(_scaleRadio.update(radioInputs(nowMs)), nowMs);
  // Suspension is derived from the machine's state rather than driven by paired
  // events, so it cannot be left set by a decision that was never applied.
  applyRadioSuspension();
  refreshRadioStatus(nowMs);
  reportScaleReadyAfterWake(nowMs);
}

bool PowerManager::isScreenDimmed() const {
  return _savingPolicy.isScreenDimmed();
}

bool PowerManager::isStatusLedEnabled() const { return _statusLedEnabled; }

void PowerManager::disableIdlePowerSaving() {
  _savingPolicy.disableScreenDimming();
  _savingPolicy.disableAutoPowerOff();
  disableAdaptivePerformance();
  disableStatusLedPowerSaving();
  disableLowBatteryShutdown();
}

void PowerManager::disablePowerButtonSleep() {
  _powerButtonSleepEnabled = false;
  _powerButtonDown = false;
  _powerButtonPressPending = false;
}

void PowerManager::setScreenDimmingTimeoutSeconds(long seconds) {
  _savingPolicy.setScreenDimmingTimeoutSeconds(seconds);
}

void PowerManager::setPreSleepCallback(void (*callback)()) {
  _savingPolicy.setPreSleepCallback(callback);
}

void PowerManager::disableLowBatteryShutdown() {
  _lowBatteryShutdownEnabled = false;
  _lowBatteryShutdown.reset();
  applyLowBatteryDisplayMode(false);
}

}  // namespace power
