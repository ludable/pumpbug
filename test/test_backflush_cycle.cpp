// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Drives BackflushCycle with a synthetic clock and pump detector, which is all
// it needs: nothing in the class touches hardware.
//
// The cases cluster around the two things that are easy to get wrong: which
// moment a period is measured to, and which reported changes are real enough
// to end one.

#include <cassert>
#include <cstdint>

#include "apps/backflush/BackflushCycle.h"

namespace {

using Phase = BackflushCycle::Phase;
using Instruction = BackflushCycle::Instruction;

constexpr uint32_t kTarget = BackflushCycle::PHASE_TARGET_MS;
constexpr uint32_t kAfterTargetConfirm =
    BackflushCycle::AFTER_TARGET_PUMP_CHANGE_CONFIRM_MS;
constexpr uint32_t kBeforeTargetConfirm =
    BackflushCycle::BEFORE_TARGET_PUMP_CHANGE_CONFIRM_MS;

// Presents a new pump state and holds it for the shorter confirmation window,
// the one that applies past a period's target, checking that it is not applied
// early.
void applyPumpChangeAtTarget(BackflushCycle& cycle, uint32_t firstObservedMs,
                             bool pumpDetected) {
  assert(!cycle.update(firstObservedMs, pumpDetected));
  assert(cycle.confirmingPumpChange());
  assert(cycle.update(firstObservedMs + kAfterTargetConfirm, pumpDetected));
}

// Runs an ideal session up to the start of the fifth pump-on period, with every
// earlier period ending exactly at its target. Returns when cycle 5 began.
uint32_t runToFifthPumpOn(BackflushCycle& cycle) {
  uint32_t now = 0;
  assert(cycle.update(now, true));
  for (size_t i = 0; i + 1 < BackflushCycle::CYCLE_COUNT; ++i) {
    now += kTarget;
    applyPumpChangeAtTarget(cycle, now, false);
    now += kTarget;
    applyPumpChangeAtTarget(cycle, now, true);
  }
  return now;
}

void normalCycleUsesDetectedTransitions() {
  BackflushCycle cycle;
  cycle.reset();

  assert(cycle.instruction(0) == Instruction::StartPump);
  assert(cycle.update(1'000, true));
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.instruction(10'999) == Instruction::PumpOn);
  assert(cycle.instruction(11'000) == Instruction::StopPump);

  applyPumpChangeAtTarget(cycle, 11'200, false);
  assert(cycle.phase() == Phase::PumpOff);
  // Waiting to confirm the pump stopped does not shift the measurement: the
  // period ends when the detector first reported it off.
  assert(cycle.results().onMs[0] == 10'200);
  assert(cycle.instruction(21'199) == Instruction::Wait);
  assert(cycle.instruction(21'200) == Instruction::StartPump);

  applyPumpChangeAtTarget(cycle, 21'500, true);
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.cycleIndex() == 1);
  assert(cycle.results().offMs[0] == 10'300);
}

void briefEarlyPumpChangeLeavesThePeriodIntact() {
  BackflushCycle cycle;
  cycle.reset();
  assert(cycle.update(0, true));

  assert(!cycle.update(2'000, false));
  assert(cycle.confirmingPumpChange());
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.displayPhase() == Phase::PumpOff);
  assert(cycle.displayInstruction(2'500) == Instruction::Wait);
  assert(cycle.displayPhaseElapsedMs(2'500) == 500);
  assert(!cycle.update(3'000, true));
  assert(!cycle.confirmingPumpChange());
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.displayPhase() == Phase::PumpOn);
  assert(cycle.displayPhaseElapsedMs(3'000) == 3'000);
  assert(cycle.phaseElapsedMs(4'000) == 4'000);
}

void aDropoutPastTheTargetLeavesThePhaseIntact() {
  BackflushCycle cycle;
  cycle.reset();
  assert(cycle.update(0, true));

  // On a weak signal, detection reports the pump off and then on again 0.8 s
  // later. The pump never stopped, so the first cycle has to still be in its
  // pump-on period rather than having begun a rest that never happened.
  assert(!cycle.update(10'100, false));
  assert(cycle.confirmingPumpChange());
  assert(!cycle.update(10'900, true));
  assert(!cycle.confirmingPumpChange());
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.cycleIndex() == 0);
  assert(cycle.phaseElapsedMs(12'000) == 12'000);

  // The real stop still belongs to the same period.
  applyPumpChangeAtTarget(cycle, 14'000, false);
  assert(cycle.results().onMs[0] == 14'000);
  assert(!cycle.offRecorded(0));
}

void sustainedEarlyPumpChangeUsesTheFirstObservedTime() {
  BackflushCycle cycle;
  cycle.reset();
  assert(cycle.update(1'000, true));

  assert(!cycle.update(7'000, false));
  assert(cycle.displayPhase() == Phase::PumpOff);
  assert(cycle.displayPhaseElapsedMs(8'500) == 1'500);
  assert(cycle.update(12'000, false));
  assert(cycle.phase() == Phase::PumpOff);
  assert(cycle.results().onMs[0] == 6'000);
  assert(cycle.phaseElapsedMs(12'000) == 5'000);

  assert(!cycle.update(14'000, true));
  assert(cycle.displayPhase() == Phase::PumpOn);
  assert(cycle.displayCycleIndex() == 1);
  assert(cycle.displayPhaseElapsedMs(15'500) == 1'500);
  assert(cycle.update(19'000, true));
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.cycleIndex() == 1);
  assert(cycle.results().offMs[0] == 7'000);
  assert(cycle.phaseElapsedMs(19'000) == 5'000);
}

void cuttingAPeriodShortNeedsTheLongerConfirmation() {
  BackflushCycle cycle;
  cycle.reset();
  assert(cycle.update(1'000, true));

  // The detector reports the pump off before the target, so that report must
  // last for the longer confirmation time. Reaching the target while waiting
  // does not shorten it.
  assert(!cycle.update(10'000, false));
  assert(!cycle.update(11'500, false));
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.update(10'000 + kBeforeTargetConfirm, false));
  assert(cycle.phase() == Phase::PumpOff);
  assert(cycle.results().onMs[0] == 9'000);
  assert(cycle.phaseElapsedMs(10'000 + kBeforeTargetConfirm) ==
         kBeforeTargetConfirm);
}

void latePumpChangesRemainVisibleInTheSummary() {
  BackflushCycle cycle;
  cycle.reset();
  assert(cycle.update(0, true));
  assert(cycle.instruction(12'000) == Instruction::StopPump);
  applyPumpChangeAtTarget(cycle, 12'000, false);
  assert(cycle.results().onMs[0] == 12'000);

  assert(cycle.instruction(24'500) == Instruction::StartPump);
  applyPumpChangeAtTarget(cycle, 24'500, true);
  assert(cycle.results().offMs[0] == 12'500);
}

void fifthPumpStopCompletesWithoutARest() {
  BackflushCycle cycle;
  cycle.reset();
  const uint32_t fifthStartedMs = runToFifthPumpOn(cycle);
  assert(cycle.phase() == Phase::PumpOn);
  assert(cycle.cycleIndex() == BackflushCycle::CYCLE_COUNT - 1);

  const uint32_t stoppedMs = fifthStartedMs + kTarget;
  assert(!cycle.update(stoppedMs, false));
  assert(cycle.confirmingPumpChange());
  assert(cycle.displayInstruction(stoppedMs) == Instruction::StopPump);
  assert(cycle.update(stoppedMs + kAfterTargetConfirm, false));
  assert(cycle.complete());
  assert(cycle.results().onMs[BackflushCycle::CYCLE_COUNT - 1] == kTarget);
  assert(!cycle.offRecorded(BackflushCycle::CYCLE_COUNT - 1));
  assert(cycle.results().offMs[BackflushCycle::CYCLE_COUNT - 1] == 0);
}

void briefDropoutDuringTheFifthOnPeriodDoesNotCompleteTheRun() {
  BackflushCycle cycle;
  cycle.reset();
  const uint32_t fifthStartedMs = runToFifthPumpOn(cycle);

  // The detector briefly reports the pump off, then recovers before the report
  // can end the final pump-on period.
  assert(!cycle.update(fifthStartedMs + 9'200, false));
  assert(cycle.confirmingPumpChange());
  assert(!cycle.update(fifthStartedMs + 9'800, true));
  assert(!cycle.confirmingPumpChange());
  assert(!cycle.complete());

  applyPumpChangeAtTarget(cycle, fifthStartedMs + 11'000, false);
  assert(cycle.complete());
}

void recordedMeasurementsTrackCompletedPeriods() {
  BackflushCycle cycle;
  cycle.reset();
  assert(!cycle.onRecorded(0));
  assert(!cycle.offRecorded(0));

  assert(cycle.update(0, true));
  assert(!cycle.onRecorded(0));

  applyPumpChangeAtTarget(cycle, kTarget, false);
  assert(cycle.onRecorded(0));
  assert(!cycle.offRecorded(0));
  assert(!cycle.onRecorded(1));

  // A newly reported pump state does not end the period until it persists for
  // the required confirmation time.
  assert(!cycle.update(2 * kTarget, true));
  assert(cycle.confirmingPumpChange());
  assert(!cycle.offRecorded(0));

  assert(cycle.update(2 * kTarget + kAfterTargetConfirm, true));
  assert(cycle.offRecorded(0));
  assert(!cycle.onRecorded(1));
  assert(!cycle.onRecorded(BackflushCycle::CYCLE_COUNT));
}

void everyRequiredPeriodIsRecordedOnceTheRunCompletes() {
  BackflushCycle cycle;
  cycle.reset();
  const uint32_t fifthStartedMs = runToFifthPumpOn(cycle);
  applyPumpChangeAtTarget(cycle, fifthStartedMs + kTarget, false);

  assert(cycle.complete());
  for (size_t i = 0; i < BackflushCycle::CYCLE_COUNT; ++i) {
    assert(cycle.onRecorded(i));
    assert(cycle.offRecorded(i) == (i + 1 < BackflushCycle::CYCLE_COUNT));
  }
}

void elapsedTimeSurvivesMillisWrap() {
  BackflushCycle cycle;
  cycle.reset();
  constexpr uint32_t start = UINT32_MAX - 2'000;
  assert(cycle.update(start, true));
  assert(cycle.phaseElapsedMs(start + 7'000) == 7'000);
  assert(cycle.instruction(start + 10'000) == Instruction::StopPump);
  applyPumpChangeAtTarget(cycle, start + 10'500, false);
  assert(cycle.results().onMs[0] == 10'500);
}

}  // namespace

int main() {
  normalCycleUsesDetectedTransitions();
  briefEarlyPumpChangeLeavesThePeriodIntact();
  aDropoutPastTheTargetLeavesThePhaseIntact();
  sustainedEarlyPumpChangeUsesTheFirstObservedTime();
  cuttingAPeriodShortNeedsTheLongerConfirmation();
  latePumpChangesRemainVisibleInTheSummary();
  fifthPumpStopCompletesWithoutARest();
  briefDropoutDuringTheFifthOnPeriodDoesNotCompleteTheRun();
  recordedMeasurementsTrackCompletedPeriods();
  everyRequiredPeriodIsRecordedOnceTheRunCompletes();
  elapsedTimeSurvivesMillisWrap();
  return 0;
}
