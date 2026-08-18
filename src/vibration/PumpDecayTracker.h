// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "PumpSignalObservation.h"

// Tracks when pump-signal decay may have begun and reports the onset if
// pump-off is confirmed.
class PumpDecayTracker {
 public:
  static constexpr size_t REFERENCE_FRAMES = 6;
  static constexpr size_t MIN_REFERENCE_FRAMES = 4;
  static constexpr float START_DROP_DB = 4.0f;
  static constexpr float RECOVERY_DROP_DB = 2.0f;
  static constexpr uint32_t MAX_CARRY_MS = 2000;

  // Advance from one vibration frame. `windowWasActive` and `windowIsActive`
  // are the VibrationWindowTrigger active states before and after processing
  // that frame. `hasUsablePumpMeasurement` is true when `rawSnrDb` is finite
  // and comes from a stationary frame with a spectral peak in the
  // pump-frequency range.
  PumpSignalObservation step(uint32_t nowMs, bool windowWasActive,
                             bool windowIsActive, bool hasUsablePumpMeasurement,
                             float rawSnrDb) {
    if (windowWasActive) {
      const bool haveReference = _referenceCount >= MIN_REFERENCE_FRAMES;
      const float reference = haveReference ? referenceMedian() : 0.0f;

      if (_candidate) {
        const bool recovered = hasUsablePumpMeasurement && haveReference &&
                               rawSnrDb >= reference - RECOVERY_DROP_DB;
        const bool expired = nowMs - _onsetMs > MAX_CARRY_MS;
        if (recovered || expired) {
          _candidate = false;
          _onsetMs = 0;
          if (expired) clearReference();
          if (hasUsablePumpMeasurement) appendReference(rawSnrDb);
        }
      } else if (haveReference && (!hasUsablePumpMeasurement ||
                                   rawSnrDb <= reference - START_DROP_DB)) {
        _candidate = true;
        _onsetMs = nowMs;
      } else if (hasUsablePumpMeasurement) {
        appendReference(rawSnrDb);
      }
    }

    if (!windowWasActive && windowIsActive) {
      clearAll();
      if (hasUsablePumpMeasurement) appendReference(rawSnrDb);
    } else if (windowWasActive && !windowIsActive) {
      _accepted = _candidate;
      _acceptedOnsetMs = _candidate ? _onsetMs : 0;
      clearWindowState();
    } else if (!windowIsActive) {
      clearWindowState();
    }

    PumpSignalState state = PumpSignalState::Off;
    if (windowIsActive) {
      state =
          _candidate ? PumpSignalState::DecayCandidate : PumpSignalState::On;
    }
    return {state, _acceptedOnsetMs, _accepted};
  }

  void reset() { clearAll(); }

 private:
  float _reference[REFERENCE_FRAMES] = {};
  size_t _referenceCount = 0;
  bool _candidate = false;
  uint32_t _onsetMs = 0;
  bool _accepted = false;
  uint32_t _acceptedOnsetMs = 0;

  void appendReference(float snrDb) {
    if (_referenceCount < REFERENCE_FRAMES) {
      _reference[_referenceCount++] = snrDb;
      return;
    }
    for (size_t i = 1; i < REFERENCE_FRAMES; ++i) {
      _reference[i - 1] = _reference[i];
    }
    _reference[REFERENCE_FRAMES - 1] = snrDb;
  }

  float referenceMedian() const {
    float sorted[REFERENCE_FRAMES];
    for (size_t i = 0; i < _referenceCount; ++i) sorted[i] = _reference[i];
    std::sort(sorted, sorted + _referenceCount);
    const size_t middle = _referenceCount / 2;
    return (_referenceCount & 1U)
               ? sorted[middle]
               : (sorted[middle - 1] + sorted[middle]) * 0.5f;
  }

  void clearReference() { _referenceCount = 0; }

  void clearWindowState() {
    clearReference();
    _candidate = false;
    _onsetMs = 0;
  }

  void clearAll() {
    clearWindowState();
    _accepted = false;
    _acceptedOnsetMs = 0;
  }
};
