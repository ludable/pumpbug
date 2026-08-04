// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "CausalPourClassifier.h"

namespace pump_scale {

// LivePourTracker feeds the live gauge, target alert, and recorder settle
// decisions while a shot is in flight. It is causal: every value comes from
// samples already seen. Its results are provisional. When the shot ends and
// its record is written, computePourEvidence() (RobustFlow.h) re-processes
// the stored samples, and the pour-start weight it derives replaces the live
// one in the record.
class LivePourTracker {
 public:
  using Band = CausalPourBand;

  struct Snapshot {
    uint32_t lastSampleMs = 0;
    int16_t rawCg = 0;
    bool haveFlow = false;
    float flowCgPerS = 0.0f;
    Band band = Band::Unknown;
    bool pourActive = false;
    int32_t yieldFromPourStartCg = 0;
    bool hasMeaningfulYield = false;
    bool lastSampleTrusted = false;
    uint8_t settledCount = 0;
    bool settledSinceLastPour = false;
  };

  void reset() {
    _classifier.reset();
    _lastTMs = 0;
    _haveSample = false;
    _prevCg = 0;
    _lastTrustedCg = 0;
    _lastSampleTrusted = false;
    _haveFirstStart = false;
    _firstStartCg = 0;
    _pourActive = false;
    _sawPourBand = false;
    _settledCount = 0;
    _settledSinceLastPour = true;
    _hasMeaningfulYield = false;
    _lastBand = Band::Unknown;
    _lastFlow = 0.0f;
    _lastHaveFlow = false;
  }

  void update(uint32_t tMs, int16_t cg) {
    const CausalPourSample s = _classifier.update(tMs, cg);
    if (!s.accepted) return;

    _lastTMs = s.tMs;
    _haveSample = true;
    _prevCg = s.rawCg;
    _lastSampleTrusted = s.trustedStep;
    if (_lastSampleTrusted) _lastTrustedCg = s.rawCg;
    _lastHaveFlow = s.haveFlow;
    _lastFlow = s.flowCgPerS;
    _lastBand = s.band;

    advanceLivePour(s);
    updateMeaningfulYield(s);
  }

  bool hasPourStartWeight() const { return _haveFirstStart; }
  int16_t firstPourStartRawCg() const {
    return _haveFirstStart ? _firstStartCg : 0;
  }
  bool hasMeaningfulYield() const { return _hasMeaningfulYield; }
  bool hasUnsettledPour() const {
    return _sawPourBand && !_settledSinceLastPour;
  }
  bool settledCountReached() const {
    return _hasMeaningfulYield && _settledCount >= kRunCloseSamples;
  }
  int16_t lastTrustedRawCg() const { return _lastTrustedCg; }
  bool lastSampleWasTrusted() const { return _lastSampleTrusted; }

  Snapshot snapshot() const {
    Snapshot s;
    s.lastSampleMs = _lastTMs;
    s.rawCg = _prevCg;
    s.haveFlow = _lastHaveFlow;
    s.flowCgPerS = _lastFlow;
    s.band = _haveSample ? _lastBand : Band::Unknown;
    s.pourActive = _pourActive;
    s.yieldFromPourStartCg =
        _haveFirstStart ? static_cast<int32_t>(_prevCg) - _firstStartCg : 0;
    s.hasMeaningfulYield = _hasMeaningfulYield;
    s.lastSampleTrusted = _lastSampleTrusted;
    s.settledCount = _settledCount;
    s.settledSinceLastPour = _settledSinceLastPour;
    return s;
  }

 private:
  // Yield from the pour start that proves coffee actually flowed, filtering
  // out knocks and drift.
  static constexpr int32_t kMeaningfulYieldCg = 300;

  void advanceLivePour(const CausalPourSample& s) {
    if (isPourBand(s.band) && s.trustedStep) {
      if (!_haveFirstStart) {
        _firstStartCg = s.pourStartCandidateCg;
        _haveFirstStart = true;
      }
      _pourActive = true;
      _sawPourBand = true;
      _settledCount = 0;
      _settledSinceLastPour = false;
      return;
    }
    if (isSettledBand(s.band)) {
      if (_settledCount < kRunCloseSamples) ++_settledCount;
      if (_settledCount >= kRunCloseSamples) {
        _pourActive = false;
        _settledSinceLastPour = true;
      }
    } else if (s.haveFlow) {
      _settledCount = 0;
    }
  }

  // Once the trusted yield from the pour start reaches kMeaningfulYieldCg,
  // _hasMeaningfulYield turns true and stays true until reset().
  void updateMeaningfulYield(const CausalPourSample& s) {
    if (_haveFirstStart && s.trustedStep) {
      const int32_t y = static_cast<int32_t>(s.rawCg) - _firstStartCg;
      if (y >= kMeaningfulYieldCg) _hasMeaningfulYield = true;
    }
  }

  CausalPourClassifier _classifier;
  uint32_t _lastTMs = 0;
  bool _haveSample = false;
  int16_t _prevCg = 0;
  int16_t _lastTrustedCg = 0;
  bool _lastSampleTrusted = false;

  bool _haveFirstStart = false;
  int16_t _firstStartCg = 0;
  bool _pourActive = false;
  bool _sawPourBand = false;
  uint8_t _settledCount = 0;
  bool _settledSinceLastPour = true;
  bool _hasMeaningfulYield = false;

  Band _lastBand = Band::Unknown;
  float _lastFlow = 0.0f;
  bool _lastHaveFlow = false;
};

}  // namespace pump_scale
