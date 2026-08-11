// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "BackflushCycle.h"

void BackflushCycle::reset() { *this = BackflushCycle{}; }

void BackflushCycle::beginPhase(Phase phase, uint32_t startedMs) {
  _phase = phase;
  _phaseStartedMs = startedMs;
  clearPumpChange();
}

void BackflushCycle::rememberPumpChange(uint32_t nowMs) {
  if (_pumpChangePending) return;
  _pumpChangePending = true;
  _pumpChangeStartedMs = nowMs;
}

void BackflushCycle::clearPumpChange() {
  _pumpChangePending = false;
  _pumpChangeStartedMs = 0;
}

uint32_t BackflushCycle::phaseElapsedMs(uint32_t nowMs) const {
  switch (_phase) {
    case Phase::PumpOn:
    case Phase::PumpOff:
      return nowMs - _phaseStartedMs;
    case Phase::WaitingForPump:
    case Phase::Complete:
      return 0;
  }
  return 0;
}

uint32_t BackflushCycle::phaseRemainingMs(uint32_t nowMs) const {
  const uint32_t elapsed = phaseElapsedMs(nowMs);
  return elapsed >= PHASE_TARGET_MS ? 0 : PHASE_TARGET_MS - elapsed;
}

BackflushCycle::Phase BackflushCycle::displayPhase() const {
  if (!_pumpChangePending) return _phase;
  switch (_phase) {
    case Phase::PumpOn:
      // The fifth pump stop completes the routine rather than starting a rest.
      return _cycleIndex + 1 < CYCLE_COUNT ? Phase::PumpOff : Phase::PumpOn;
    case Phase::PumpOff:
      return Phase::PumpOn;
    case Phase::WaitingForPump:
    case Phase::Complete:
      return _phase;
  }
  return _phase;
}

uint32_t BackflushCycle::displayPhaseElapsedMs(uint32_t nowMs) const {
  return displayPhase() != _phase ? nowMs - _pumpChangeStartedMs
                                  : phaseElapsedMs(nowMs);
}

bool BackflushCycle::onRecorded(size_t cycle) const {
  if (cycle >= CYCLE_COUNT || cycle > _cycleIndex) return false;
  if (cycle < _cycleIndex) return true;
  switch (_phase) {
    case Phase::PumpOff:
    case Phase::Complete:
      return true;
    case Phase::WaitingForPump:
    case Phase::PumpOn:
      return false;
  }
  return false;
}

bool BackflushCycle::offRecorded(size_t cycle) const {
  return cycle < CYCLE_COUNT && cycle < _cycleIndex;
}

BackflushCycle::Instruction BackflushCycle::instruction(uint32_t nowMs) const {
  switch (_phase) {
    case Phase::WaitingForPump:
      return Instruction::StartPump;
    case Phase::PumpOn:
      return phaseElapsedMs(nowMs) >= PHASE_TARGET_MS ? Instruction::StopPump
                                                      : Instruction::PumpOn;
    case Phase::PumpOff:
      return phaseElapsedMs(nowMs) >= PHASE_TARGET_MS ? Instruction::StartPump
                                                      : Instruction::Wait;
    case Phase::Complete:
      return Instruction::Complete;
  }
  return Instruction::Complete;
}

BackflushCycle::Instruction BackflushCycle::displayInstruction(
    uint32_t nowMs) const {
  if (!_pumpChangePending) return instruction(nowMs);
  if (_phase == Phase::PumpOn && _cycleIndex + 1 == CYCLE_COUNT)
    return Instruction::StopPump;
  switch (displayPhase()) {
    case Phase::PumpOn:
      return Instruction::PumpOn;
    case Phase::PumpOff:
      return Instruction::Wait;
    case Phase::WaitingForPump:
    case Phase::Complete:
      return instruction(nowMs);
  }
  return instruction(nowMs);
}

// Decides whether a pending pump change has persisted long enough to end the
// running period. Which window applies is settled when the change is first
// detected, so one that arrived before the target keeps the longer wait even if
// the target passes while it is still being confirmed.
bool BackflushCycle::shouldApplyPumpChange(uint32_t nowMs) const {
  if (!_pumpChangePending) return false;
  const bool beganAfterTarget =
      _pumpChangeStartedMs - _phaseStartedMs >= PHASE_TARGET_MS;
  const uint32_t confirmMs = beganAfterTarget
                                 ? AFTER_TARGET_PUMP_CHANGE_CONFIRM_MS
                                 : BEFORE_TARGET_PUMP_CHANGE_CONFIRM_MS;
  return nowMs - _pumpChangeStartedMs >= confirmMs;
}

bool BackflushCycle::update(uint32_t nowMs, bool pumpDetected) {
  switch (_phase) {
    case Phase::WaitingForPump:
      // Nothing is timed until the operator runs the pump. Detection already
      // waits for several qualifying frames before reporting the pump on, so
      // confirming that again here would only shorten the first period.
      if (!pumpDetected) return false;
      beginPhase(Phase::PumpOn, nowMs);
      return true;

    case Phase::PumpOn:
      if (pumpDetected) {
        clearPumpChange();
        return false;
      }
      rememberPumpChange(nowMs);
      if (!shouldApplyPumpChange(nowMs)) return false;
      _results.onMs[_cycleIndex] = _pumpChangeStartedMs - _phaseStartedMs;
      if (_cycleIndex + 1 == CYCLE_COUNT) {
        beginPhase(Phase::Complete, _pumpChangeStartedMs);
        return true;
      }
      beginPhase(Phase::PumpOff, _pumpChangeStartedMs);
      return true;

    case Phase::PumpOff:
      if (!pumpDetected) {
        clearPumpChange();
        return false;
      }
      rememberPumpChange(nowMs);
      if (!shouldApplyPumpChange(nowMs)) return false;
      _results.offMs[_cycleIndex] = _pumpChangeStartedMs - _phaseStartedMs;
      ++_cycleIndex;
      beginPhase(Phase::PumpOn, _pumpChangeStartedMs);
      return true;

    case Phase::Complete:
      return false;
  }
  return false;
}
