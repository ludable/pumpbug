// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

// Times five 10-second pump-on periods, with a 10-second pump-off rest between
// them.
//
// Timing is synchronized with pump vibration detection. The state machine is
// flexible about period lengths if the operator decides to cut a period short,
// but will remain in the same period if two pump changes happen quickly (e.g.
// operator mistakenly switches off pump, immediately corrects).
//
// The caller supplies the clock and nothing here touches hardware.
class BackflushCycle {
 public:
  static constexpr size_t CYCLE_COUNT = 5;
  static constexpr uint32_t PHASE_TARGET_MS = 10'000;

  // A pump change before the target is reached, must last longer before it ends
  // the period. After the target is reached, the shorter wait only filters
  // brief detection errors.
  static constexpr uint32_t BEFORE_TARGET_PUMP_CHANGE_CONFIRM_MS = 5'000;
  static constexpr uint32_t AFTER_TARGET_PUMP_CHANGE_CONFIRM_MS = 1'500;

  enum class Phase : uint8_t {
    WaitingForPump,
    PumpOn,
    PumpOff,
    Complete,
  };

  enum class Instruction : uint8_t {
    StartPump,
    PumpOn,
    StopPump,
    Wait,
    Complete,
  };

  // Duration of each period in milliseconds for the final report.
  struct Results {
    uint32_t onMs[CYCLE_COUNT] = {};
    uint32_t offMs[CYCLE_COUNT] = {};
  };

  void reset();

  // Applies the pump state detected at `nowMs` and advances the routine. Call
  // it regularly rather than only when `pumpDetected` changes because pump
  // changes must remain stable for a measured time. Returns true when a new
  // period started or the run finished.
  bool update(uint32_t nowMs, bool pumpDetected);

  Phase phase() const { return _phase; }
  // The period shown to the operator. Once a new pump state is first seen, the
  // next period starts counting immediately while that state is confirmed. If
  // the report is withdrawn, this returns to the confirmed period.
  Phase displayPhase() const;
  Instruction instruction(uint32_t nowMs) const;
  Instruction displayInstruction(uint32_t nowMs) const;
  size_t cycleIndex() const { return _cycleIndex; }
  size_t displayCycleIndex() const {
    return _pumpChangePending && _phase == Phase::PumpOff ? _cycleIndex + 1
                                                          : _cycleIndex;
  }
  uint32_t phaseElapsedMs(uint32_t nowMs) const;
  uint32_t displayPhaseElapsedMs(uint32_t nowMs) const;
  uint32_t phaseRemainingMs(uint32_t nowMs) const;
  bool complete() const { return _phase == Phase::Complete; }
  const Results& results() const { return _results; }

  // True while a detected pump change is waiting out its confirmation window,
  // so a caller can show that the change has been seen rather than presenting
  // the period as settled.
  bool confirmingPumpChange() const { return _pumpChangePending; }

  // Report whether a period has ended and so has a duration worth reading. A
  // session that stops early leaves the periods it never finished without one.
  bool onRecorded(size_t cycle) const;
  bool offRecorded(size_t cycle) const;

 private:
  bool shouldApplyPumpChange(uint32_t nowMs) const;
  void beginPhase(Phase phase, uint32_t startedMs);
  void rememberPumpChange(uint32_t nowMs);
  void clearPumpChange();

  Phase _phase = Phase::WaitingForPump;
  size_t _cycleIndex = 0;
  uint32_t _phaseStartedMs = 0;
  uint32_t _pumpChangeStartedMs = 0;
  bool _pumpChangePending = false;
  Results _results{};
};
