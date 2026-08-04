// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <M5Unified.h>

#include <cassert>
#include <cstring>

#include "ble/BleScaleService.h"
#include "diagnostics/RuntimeEventLog.h"
#include "power/PowerManager.h"
#include "util/power.h"

unsigned powerSavingShutdownRequests = 0;
unsigned powerSavingExplicitShutdownRequests = 0;
unsigned powerSavingConditionalShutdownRequests = 0;
bool powerSavingShutdownSucceeds = true;
TestM5 M5;

namespace {

unsigned long nowMs = 0;
unsigned wakeLogs = 0;
unsigned restartLogs = 0;
unsigned scaleReadyLogs = 0;
uint32_t cpuMhz = 240;
unsigned cpuRequests = 0;
unsigned cpuErrorLogs = 0;
bool acceptCpuChange = true;
power::BatteryStatus batteryStatus{-1, -1, false,
                                   power::ChargingState::Unknown};
unsigned batteryStatusReads = 0;
unsigned displayPowerSaveOnRequests = 0;
unsigned displayPowerSaveOffRequests = 0;
bool statusLedEnabled = true;
bool acceptStatusLedChange = true;
unsigned statusLedRequests = 0;
power::PowerButtonSample powerButtonSample;
bool powerButtonReadSucceeds = true;
unsigned powerButtonPolls = 0;
unsigned powerButtonReadErrorLogs = 0;

void resetObservations() {
  nowMs = 0;
  runtimeEventLog = {};
  wakeLogs = 0;
  restartLogs = 0;
  scaleReadyLogs = 0;
  cpuMhz = 240;
  cpuRequests = 0;
  cpuErrorLogs = 0;
  acceptCpuChange = true;
  batteryStatus = {-1, -1, false, power::ChargingState::Unknown};
  batteryStatusReads = 0;
  displayPowerSaveOnRequests = 0;
  displayPowerSaveOffRequests = 0;
  statusLedEnabled = true;
  acceptStatusLedChange = true;
  statusLedRequests = 0;
  powerSavingShutdownRequests = 0;
  powerSavingExplicitShutdownRequests = 0;
  powerSavingConditionalShutdownRequests = 0;
  powerSavingShutdownSucceeds = true;
  powerButtonSample = {};
  powerButtonReadSucceeds = true;
  powerButtonPolls = 0;
  powerButtonReadErrorLogs = 0;
}

void waitForControllerStop(power::PowerManager& manager,
                           BleScaleService& scale) {
  scale.enable();
  manager.update();

  manager.setScreenDimmingTimeoutSeconds(0);
  scale.state = BleScaleService::State::SCANNING;
  nowMs = 1;
  manager.update();
  assert(scale.powerSuspended);
  assert(scale.suspendRequests == 1);

  scale.state = BleScaleService::State::OFF;
  nowMs = 2;
  manager.update();
  assert(scale.stopRequests == 1);

  // The zero timeout above forced the dim. Restore a realistic one so that a
  // later activity event wakes the display rather than re-dimming instantly.
  manager.setScreenDimmingTimeoutSeconds(60);
}

void testWakeOnStopCompletionUsesTheDemandDecisionLoop() {
  resetObservations();
  BleScaleService scale;
  scale.startCompletesImmediately = true;
  power::PowerManager manager(scale);
  waitForControllerStop(manager, scale);

  manager.notifyActivity();
  scale.controller = ble::ControllerState::Stopped;
  nowMs = 3;
  manager.update();  // the stop lands
  nowMs = 4;
  manager.update();  // the radio is wanted again, so it is started

  assert(scale.startRequests == 1);
  assert(!scale.powerSuspended);
  assert(scale.resumeRequests == 1);
  assert(wakeLogs == 1);
  assert(restartLogs == 1);
  assert(runtimeEventLog.netWrites == 0);
}

void testDiagnosticScanDemandRestartsAStoppedController() {
  resetObservations();
  BleScaleService scale;
  scale.startCompletesImmediately = true;
  power::PowerManager manager(scale);
  waitForControllerStop(manager, scale);

  scale.disable();
  scale.diagnosticScanWanted = true;
  scale.controller = ble::ControllerState::Stopped;
  manager.notifyActivity();
  nowMs = 3;
  manager.update();  // the stop lands
  nowMs = 4;
  manager.update();  // the diagnostic lease starts it again

  assert(scale.startRequests == 1);
  assert(!scale.powerSuspended);
  assert(scale.resumeRequests == 1);
  assert(runtimeEventLog.netWrites == 0);
}

void testUnstartedServiceIsOutsideStandbyPolicy() {
  resetObservations();
  BleScaleService scale;
  scale.started = false;
  scale.controller = ble::ControllerState::Stopped;
  scale.enable();
  power::PowerManager manager(scale);

  manager.notifyActivity();
  manager.update();

  assert(scale.startRequests == 0);
  assert(runtimeEventLog.netWrites == 0);
}

void testRejectedWakeReportsOnceAndWaitsForNewActivity() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);
  waitForControllerStop(manager, scale);

  scale.controller = ble::ControllerState::Stopped;
  scale.acceptStart = false;
  manager.notifyActivity();
  nowMs = 3;
  manager.update();  // the stop lands
  nowMs = 4;
  manager.update();  // the wake is attempted and rejected

  assert(scale.startRequests == 1);
  assert(wakeLogs == 1);
  assert(!scale.powerSuspended);
  assert(scale.resumeRequests == 1);
  assert(runtimeEventLog.netWrites == 1);
  assert(runtimeEventLog.lastSource == diagnostics::NetSource::Ble);
  assert(
      runtimeEventLog.lastCode ==
      static_cast<uint16_t>(diagnostics::BleFailureCode::ScaleControllerStart));

  for (nowMs = 5; nowMs < 20; ++nowMs) manager.update();
  assert(scale.startRequests == 1);
  assert(runtimeEventLog.netWrites == 1);
}

void testLongestDecisionChainAppliesItsTerminalResume() {
  resetObservations();
  BleScaleService scale;
  scale.startCompletesImmediately = true;
  scale.dropDemandOnStart = true;
  power::PowerManager manager(scale);
  waitForControllerStop(manager, scale);

  scale.controller = ble::ControllerState::Stopped;
  scale.acceptStop = false;
  manager.notifyActivity();
  nowMs = 3;
  manager.update();  // the stop lands
  nowMs = 4;
  manager.update();  // the wake runs, and demand vanishes as it completes
  nowMs = 5;
  manager.update();  // nothing wants it, so the release begins
  nowMs = 6;
  manager.update();  // the service has let go, so the stop is requested

  assert(scale.startRequests == 1);
  assert(scale.stopRequests == 2);
  // Resumed once when the restart landed, and again when the rejected stop
  // abandoned the release that followed it.
  assert(scale.resumeRequests == 2);
  assert(!scale.powerSuspended);
  assert(runtimeEventLog.netWrites == 1);
}

// Wake timing belongs to PowerManager, not the state machine: it reports how
// long the controller took to come back and, when the wake was for a scale, how
// long the scale then took to arrive.
void testWakeTimingReportsTheScaleArrivalOnce() {
  resetObservations();
  BleScaleService scale;
  scale.startCompletesImmediately = true;
  power::PowerManager manager(scale);
  waitForControllerStop(manager, scale);

  manager.notifyActivity();
  scale.controller = ble::ControllerState::Stopped;
  nowMs = 3;
  manager.update();  // the stop lands
  nowMs = 4;
  manager.update();  // the radio is wanted again, so it is started
  assert(restartLogs == 1);
  assert(scaleReadyLogs == 0);

  scale.state = BleScaleService::State::READY;
  nowMs = 5;
  manager.update();
  assert(scaleReadyLogs == 1);

  for (nowMs = 6; nowMs < 12; ++nowMs) manager.update();
  assert(scaleReadyLogs == 1);
}

// Releasing the radio ends the wake it interrupted, so a scale that turns up
// later is not reported against a wake that is already over.
void testReleasingTheRadioEndsAnUnfinishedWake() {
  resetObservations();
  BleScaleService scale;
  scale.startCompletesImmediately = true;
  power::PowerManager manager(scale);
  waitForControllerStop(manager, scale);

  manager.notifyActivity();
  scale.controller = ble::ControllerState::Stopped;
  nowMs = 3;
  manager.update();  // the stop lands
  nowMs = 4;
  manager.update();  // the radio is wanted again, so it is started
  assert(restartLogs == 1);

  // The scale never arrives and the consumer gives up, so the radio goes again.
  scale.disable();
  manager.setScreenDimmingTimeoutSeconds(0);
  nowMs = 5;
  manager.update();  // nothing wants it: the release begins

  scale.state = BleScaleService::State::READY;
  for (nowMs = 6; nowMs < 12; ++nowMs) manager.update();
  assert(scaleReadyLogs == 0);
}

// A fixed-running policy keeps the radio up where demand policy would release
// it, then returns control to demand policy.
void testForcedRadioPolicyOverridesTheDemandPolicy() {
  resetObservations();
  BleScaleService scale;
  scale.stopCompletesImmediately = true;
  power::PowerManager manager(scale);
  scale.enable();
  scale.state = BleScaleService::State::SCANNING;

  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ForceRunning));
  manager.disableIdlePowerSaving();
  manager.setScreenDimmingTimeoutSeconds(0);
  for (nowMs = 1; nowMs < 10; ++nowMs) manager.update();
  assert(scale.stopRequests == 0);
  assert(scale.suspendRequests == 0);
  assert(!scale.powerSuspended);
  assert(manager.scaleRadioStatus().settled);

  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::Demand));
  nowMs = 10;
  manager.update();  // dimmed with no scale, so the service is asked to let go
  assert(scale.powerSuspended);

  scale.state = BleScaleService::State::OFF;
  nowMs = 11;
  manager.update();  // the service has let go, so the controller stops
  assert(scale.stopRequests == 1);
  assert(manager.scaleRadioStatus().settled);
}

// Settled means the controller reached what the policy asked for, not merely
// that the machine is between transitions. The answer has to be right at the
// moment the policy changes, before another update() runs, because a phase
// selects a policy and asks in the same breath.
void testRadioStatusAnswersForTheSelectedPolicyImmediately() {
  resetObservations();
  BleScaleService scale;
  scale.stopCompletesImmediately = true;
  power::PowerManager manager(scale);
  scale.enable();
  scale.state = BleScaleService::State::SCANNING;

  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ForceRunning));
  for (nowMs = 1; nowMs < 5; ++nowMs) manager.update();
  power::ScaleRadioStatus status = manager.scaleRadioStatus();
  assert(status.settled && status.shouldRun);
  assert(status.controller == ble::ControllerState::Running);

  // Asked to stop a running controller: not settled, with no update() between.
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ForceStopped));
  status = manager.scaleRadioStatus();
  assert(!status.settled);
  assert(!status.shouldRun);
  assert(status.controller == ble::ControllerState::Running);

  // Back to a policy the controller already satisfies, again with no update()
  // in between: settled becomes true because the observation, not the machine
  // state, decides it.
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ForceRunning));
  status = manager.scaleRadioStatus();
  assert(status.settled);
  assert(status.shouldRun);
}

// A stopped target is settled only when the controller explicitly reports
// Stopped. Starting, Stopping, and Failed are not interchangeable with it.
void testRadioStatusRequiresTheExactControllerState() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  scale.controller = ble::ControllerState::Starting;
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ForceStopped));
  power::ScaleRadioStatus status = manager.scaleRadioStatus();
  assert(status.controller == ble::ControllerState::Starting);
  assert(!status.settled);

  scale.controller = ble::ControllerState::Stopping;
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::Demand));
  status = manager.scaleRadioStatus();
  assert(status.controller == ble::ControllerState::Stopping);
  assert(!status.settled);

  scale.controller = ble::ControllerState::Failed;
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ForceStopped));
  status = manager.scaleRadioStatus();
  assert(status.controller == ble::ControllerState::Failed);
  assert(!status.settled);
}

// The stress diagnostic can take direct ownership only from the stable running
// state in which it starts. A transitional controller remains managed.
void testExternalControlRequiresAStableRunningController() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  scale.controller = ble::ControllerState::Starting;
  assert(
      !manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ExternalControl));
  assert(manager.scaleRadioStatus().policy == power::ScaleRadioPolicy::Demand);

  scale.controller = ble::ControllerState::Running;
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ExternalControl));
  assert(manager.scaleRadioStatus().policy ==
         power::ScaleRadioPolicy::ExternalControl);

  // Once ownership has passed, querying the same policy must not depend on the
  // controller state the stress diagnostic has subsequently produced.
  scale.controller = ble::ControllerState::Stopped;
  assert(manager.setScaleRadioPolicy(power::ScaleRadioPolicy::ExternalControl));
}

// Idle power-saving controls display and performance behavior. Radio behavior
// remains the responsibility of the selected ScaleRadioPolicy.
void testDisablingIdlePowerSavingDoesNotTouchTheRadio() {
  resetObservations();
  BleScaleService scale;
  scale.stopCompletesImmediately = true;
  power::PowerManager manager(scale);
  scale.enable();
  scale.state = BleScaleService::State::SCANNING;

  manager.disableIdlePowerSaving();
  for (nowMs = 1; nowMs < 10; ++nowMs) manager.update();
  // Demand is still selected and the display never dims, so the radio runs.
  assert(scale.stopRequests == 0);
  assert(!scale.powerSuspended);
}

void testAdaptivePerformanceSeparatesIdleAndForegroundWork() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  assert(manager.loopDelayMs() == 10);
  assert(cpuRequests == 0);
  auto status = manager.performanceStatus();
  assert(status.actualCpuMhz == 240);
  assert(status.loopDelayMs == 10);
  assert(status.adaptivePerformanceEnabled);
  assert(!status.fullPerformanceRequired);

  manager.setForegroundFullPerformanceRequired(true);
  assert(manager.loopDelayMs() == 0);
  assert(cpuMhz == 240);
  status = manager.performanceStatus();
  assert(status.loopDelayMs == 0);
  assert(status.fullPerformanceRequired);

  manager.setForegroundFullPerformanceRequired(false);
  manager.setScreenDimmingTimeoutSeconds(0);
  nowMs = 1;
  manager.update();
  assert(manager.loopDelayMs() == 10);
  assert(cpuMhz == 80);
  status = manager.performanceStatus();
  assert(status.actualCpuMhz == 80);
  assert(status.loopDelayMs == 10);
  assert(!status.fullPerformanceRequired);

  manager.setForegroundFullPerformanceRequired(true);
  assert(manager.loopDelayMs() == 0);
  assert(cpuMhz == 240);

  manager.setForegroundFullPerformanceRequired(false);
  assert(cpuMhz == 80);

  manager.notifyActivity();
  assert(manager.loopDelayMs() == 10);
  assert(cpuMhz == 240);
  assert(cpuRequests == 5);
}

void testApplicationPerformanceRequirementComposesWithForegroundWork() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  manager.setScreenDimmingTimeoutSeconds(0);
  nowMs = 1;
  manager.update();
  assert(cpuMhz == 80);
  assert(manager.loopDelayMs() == 10);

  manager.setApplicationFullPerformanceRequired(true);
  assert(cpuMhz == 240);
  assert(manager.loopDelayMs() == 0);

  manager.setForegroundFullPerformanceRequired(true);
  manager.setApplicationFullPerformanceRequired(false);
  assert(cpuMhz == 240);
  assert(manager.loopDelayMs() == 0);

  manager.setForegroundFullPerformanceRequired(false);
  assert(cpuMhz == 80);
  assert(manager.loopDelayMs() == 10);
}

void testDisablingIdlePowerSavingRestoresFullCpuSpeed() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  manager.setScreenDimmingTimeoutSeconds(0);
  nowMs = 1;
  manager.update();
  assert(cpuMhz == 80);

  manager.disableIdlePowerSaving();
  assert(manager.loopDelayMs() == 0);
  assert(cpuMhz == 240);
  assert(cpuRequests == 2);
  const auto status = manager.performanceStatus();
  assert(status.actualCpuMhz == 240);
  assert(status.loopDelayMs == 0);
  assert(!status.adaptivePerformanceEnabled);
}

void testRestoringAdaptivePerformanceReappliesTheCpuTarget() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  manager.update();
  assert(cpuMhz == 240);
  assert(cpuRequests == 1);

  manager.disableIdlePowerSaving();
  assert(cpuRequests == 2);

  cpuMhz = 80;  // A diagnostic may set the frequency outside PowerManager.
  assert(manager.performanceStatus().actualCpuMhz == 80);

  manager.enableAdaptivePerformance();
  assert(manager.loopDelayMs() == 10);
  assert(cpuMhz == 240);
  assert(cpuRequests == 3);
}

void testFailedCpuChangeWaitsForDiscreteActivityBeforeRetrying() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  acceptCpuChange = false;
  manager.update();
  assert(cpuRequests == 1);
  assert(cpuErrorLogs == 1);

  nowMs = 1;
  manager.update();
  manager.keepAwake();
  assert(cpuRequests == 1);
  assert(cpuErrorLogs == 1);

  acceptCpuChange = true;
  nowMs = 2;
  manager.update();
  assert(cpuRequests == 1);

  manager.notifyActivity();
  assert(cpuRequests == 2);
  assert(cpuErrorLogs == 1);

  nowMs = 3;
  manager.update();
  assert(cpuRequests == 2);
}

void testStatusLedTracksExternalPowerAtThePollingInterval() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  manager.update();
  assert(!statusLedEnabled);
  assert(!manager.isStatusLedEnabled());
  assert(statusLedRequests == 1);

  batteryStatus.hasExternalPower = true;
  nowMs = 4999;
  manager.update();
  assert(!statusLedEnabled);
  assert(statusLedRequests == 1);

  nowMs = 5000;
  manager.update();
  assert(statusLedEnabled);
  assert(manager.isStatusLedEnabled());
  assert(statusLedRequests == 2);
}

void testManualPowerControlKeepsStatusLedOnUntilAutomaticControlResumes() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  manager.update();
  assert(!statusLedEnabled);

  manager.disableIdlePowerSaving();
  assert(statusLedEnabled);
  assert(manager.isStatusLedEnabled());
  assert(statusLedRequests == 2);

  nowMs = 10000;
  manager.update();
  assert(statusLedEnabled);
  assert(statusLedRequests == 2);

  manager.enableStatusLedPowerSaving();
  manager.update();
  assert(!statusLedEnabled);
  assert(statusLedRequests == 3);
}

void testManualPowerControlStopsPeriodicBatteryReads() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  manager.disableIdlePowerSaving();
  for (nowMs = 0; nowMs <= 15000; nowMs += 5000) manager.update();

  assert(batteryStatusReads == 0);
}

void testLowBatteryShutdownStillRunsUnderManualPowerControl() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);
  manager.disableIdlePowerSaving();
  manager.enableLowBatteryShutdown();
  batteryStatus.voltageMv = 3440;

  for (nowMs = 0; nowMs <= 15000; nowMs += 5000) manager.update();

  assert(manager.isStatusLedEnabled());
  assert(powerSavingShutdownRequests == 1);
  assert(powerSavingExplicitShutdownRequests == 0);
  assert(powerSavingConditionalShutdownRequests == 1);
  assert(displayPowerSaveOnRequests == 1);
  assert(displayPowerSaveOffRequests == 0);
}

void testLowBatteryDisplayCueTracksConfirmation() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);
  batteryStatus.voltageMv = 3440;

  manager.update();
  assert(displayPowerSaveOnRequests == 1);
  assert(displayPowerSaveOffRequests == 0);

  batteryStatus.voltageMv = 3480;
  nowMs = 5000;
  manager.update();
  assert(displayPowerSaveOnRequests == 1);
  assert(displayPowerSaveOffRequests == 0);

  batteryStatus.voltageMv = 3500;
  nowMs = 10000;
  manager.update();
  assert(displayPowerSaveOnRequests == 1);
  assert(displayPowerSaveOffRequests == 1);
}

void testPowerButtonPressWaitsForDoubleClickWindowBeforeSleeping() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  powerButtonSample = {true, false};
  manager.update();
  assert(powerSavingShutdownRequests == 0);

  powerButtonSample = {};
  nowMs = power::kPowerButtonDoubleClickWindowMs + 199;
  manager.update();
  assert(powerSavingShutdownRequests == 0);

  nowMs = power::kPowerButtonDoubleClickWindowMs + 200;
  manager.update();
  assert(powerSavingShutdownRequests == 1);
}

void testPowerButtonPressShutsDownOnExternalPower() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  powerButtonSample = {true, false};
  manager.update();
  const unsigned cpuRequestsBeforeShutdown = cpuRequests;
  powerButtonSample = {};
  nowMs = power::kPowerButtonDoubleClickWindowMs + 200;
  manager.update();

  assert(powerSavingShutdownRequests == 1);
  assert(powerSavingExplicitShutdownRequests == 1);
  assert(powerSavingConditionalShutdownRequests == 0);
  assert(cpuRequests == cpuRequestsBeforeShutdown);
}

void testPowerButtonSecondPressRestartsTheDecisionWindow() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  powerButtonSample = {true, false};
  manager.update();
  nowMs = 600;
  powerButtonSample = {true, true};
  manager.update();
  nowMs = 800;
  powerButtonSample = {};
  manager.update();
  nowMs = 1499;
  manager.update();
  assert(powerSavingShutdownRequests == 0);
  nowMs = 1500;
  manager.update();
  assert(powerSavingShutdownRequests == 1);
}

void testPowerButtonHoldDoesNotArmSleepUntilRelease() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);

  powerButtonSample = {true, true};
  manager.update();
  powerButtonSample = {false, true};
  nowMs = 1200;
  manager.update();
  assert(powerSavingShutdownRequests == 0);

  powerButtonSample = {};
  nowMs = 1400;
  manager.update();
  nowMs = 2099;
  manager.update();
  assert(powerSavingShutdownRequests == 0);
  nowMs = 2100;
  manager.update();
  assert(powerSavingShutdownRequests == 1);
}

void testPowerButtonReadFailureLogsOnceUntilAReadSucceeds() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);
  powerButtonReadSucceeds = false;

  manager.update();
  nowMs = 200;
  manager.update();
  assert(powerButtonReadErrorLogs == 1);

  powerButtonReadSucceeds = true;
  nowMs = 400;
  manager.update();
  powerButtonReadSucceeds = false;
  nowMs = 600;
  manager.update();
  assert(powerButtonReadErrorLogs == 2);
}

void testPowerButtonSleepCanBeDisabledForMeasurements() {
  resetObservations();
  BleScaleService scale;
  power::PowerManager manager(scale);
  manager.disablePowerButtonSleep();
  powerButtonSample = {true, false};

  for (nowMs = 0; nowMs <= 1000; nowMs += 200) manager.update();

  assert(powerButtonPolls == 0);
  assert(powerSavingShutdownRequests == 0);
}

}  // namespace

diagnostics::RuntimeEventLog runtimeEventLog;

unsigned long millis() { return nowMs; }

bool setCpuFrequencyMhz(uint32_t mhz) {
  ++cpuRequests;
  if (!acceptCpuChange) return false;
  cpuMhz = mhz;
  return true;
}

uint32_t getCpuFrequencyMhz() { return cpuMhz; }

namespace power {

BatteryStatus getBatteryStatus() {
  ++batteryStatusReads;
  return batteryStatus;
}

bool setStatusLedEnabled(bool enabled) {
  ++statusLedRequests;
  if (!acceptStatusLedChange) return false;
  statusLedEnabled = enabled;
  return true;
}

bool pollPowerButton(PowerButtonSample& sample) {
  ++powerButtonPolls;
  sample = powerButtonSample;
  return powerButtonReadSucceeds;
}

// The real SpeakerCodec is linked in, so its codec-power calls land here. This
// test never primes it, so nothing is expected of them.
}  // namespace power

void testM5LogInfo(const char* format, ...) {
  if (std::strcmp(format, "Scale Bluetooth controller (%s): wanted again") ==
      0) {
    ++wakeLogs;
  } else if (std::strcmp(
                 format,
                 "Scale Bluetooth controller (%s): restarted in %u ms") == 0) {
    ++restartLogs;
  } else if (std::strcmp(
                 format,
                 "Scale Bluetooth controller: scale ready after %u ms") == 0) {
    ++scaleReadyLogs;
  }
}

void testM5LogWarning(const char*, ...) {}

void testDisplayPowerSaveOn() { ++displayPowerSaveOnRequests; }

void testDisplayPowerSaveOff() { ++displayPowerSaveOffRequests; }

void testM5LogError(const char* format, ...) {
  if (std::strcmp(format,
                  "Power manager: failed to set CPU frequency to %u MHz") ==
      0) {
    ++cpuErrorLogs;
  } else if (std::strcmp(format, "Power button: failed to read button state") ==
             0) {
    ++powerButtonReadErrorLogs;
  }
}

int main() {
  testWakeOnStopCompletionUsesTheDemandDecisionLoop();
  testRejectedWakeReportsOnceAndWaitsForNewActivity();
  testDiagnosticScanDemandRestartsAStoppedController();
  testUnstartedServiceIsOutsideStandbyPolicy();
  testLongestDecisionChainAppliesItsTerminalResume();
  testWakeTimingReportsTheScaleArrivalOnce();
  testReleasingTheRadioEndsAnUnfinishedWake();
  testForcedRadioPolicyOverridesTheDemandPolicy();
  testRadioStatusAnswersForTheSelectedPolicyImmediately();
  testRadioStatusRequiresTheExactControllerState();
  testExternalControlRequiresAStableRunningController();
  testDisablingIdlePowerSavingDoesNotTouchTheRadio();
  testAdaptivePerformanceSeparatesIdleAndForegroundWork();
  testApplicationPerformanceRequirementComposesWithForegroundWork();
  testDisablingIdlePowerSavingRestoresFullCpuSpeed();
  testRestoringAdaptivePerformanceReappliesTheCpuTarget();
  testFailedCpuChangeWaitsForDiscreteActivityBeforeRetrying();
  testStatusLedTracksExternalPowerAtThePollingInterval();
  testManualPowerControlKeepsStatusLedOnUntilAutomaticControlResumes();
  testManualPowerControlStopsPeriodicBatteryReads();
  testLowBatteryShutdownStillRunsUnderManualPowerControl();
  testLowBatteryDisplayCueTracksConfirmation();
  testPowerButtonPressWaitsForDoubleClickWindowBeforeSleeping();
  testPowerButtonPressShutsDownOnExternalPower();
  testPowerButtonSecondPressRestartsTheDecisionWindow();
  testPowerButtonHoldDoesNotArmSleepUntilRelease();
  testPowerButtonReadFailureLogsOnceUntilAReadSucceeds();
  testPowerButtonSleepCanBeDisabledForMeasurements();
  return 0;
}
