// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "util/ema.h"

// FlowEstimator: measures how fast the weight is changing (the "flow").
//
// It fits a straight line to the recent weight readings over a short trailing
// time window and reports the slope in centigrams per second, then smooths that
// with an EMA. The same estimator is used in two places, fed different inputs:
//   - TargetAlert feeds it the live yield, to predict when to tell the operator
//     to cut the pump.
//   - LivePourTracker and computePourEvidence feed it the raw scale weight, to
//     decide what counts as a pour. A slope ignores the absolute weight, so a
//     constant tare offset makes no difference, and a one-off step or tare is a
//     brief blip the window quickly recovers from.
//
// Pure logic, no Arduino/BLE/filesystem dependency, so it runs in host tests.
// Deciding which slopes count as a pour (the floor and ceiling) is left to the
// callers; this class only answers "how fast is the weight changing right now,
// and do we have enough data to say".
namespace pump_scale {

class FlowEstimator {
 public:
  void reset() {
    _count = 0;
    _head = 0;
    _estimable = false;
    _ema.reset();
  }

  // Take one new sample (the caller has already dropped duplicates) and update
  // the smoothed flow. The line fit is done here, once, and cached; flow() just
  // returns the cached result, so the regression — the most expensive step —
  // runs only once per sample rather than again on every read.
  void push(uint32_t tMs, int16_t cg) {
    _ring[_head] = {tMs, cg};
    _head = (_head + 1) % kRingSize;
    if (_count < kRingSize) ++_count;
    _newestT = tMs;
    float raw;
    _estimable = _estimateRaw(raw);
    if (_estimable) _ema.update(raw);
  }

  // Returns the smoothed flow (cg/s) in `out`. Returns false when there isn't
  // enough to say: too few samples in the window, the samples span too little
  // time, the smoothing hasn't warmed up yet, or the newest sample is older
  // than the window relative to `nowMs` (a dropout). Whether a given flow
  // counts as a pour is the caller's decision, not this class's.
  bool flow(uint32_t nowMs, float& out) const {
    if (!_estimable || !_ema.primed()) return false;
    if (nowMs - _newestT > kWindowMs) return false;  // dropout
    out = _ema.value();
    return true;
  }

 private:
  static constexpr int kRingSize = 16;
  static constexpr uint32_t kWindowMs = 750;
  static constexpr uint32_t kMinSpanMs = 200;  // need this much time spread
  static constexpr int kMinPoints = 3;

  struct Pt {
    uint32_t tMs;
    int16_t cg;
  };
  Pt _ring[kRingSize];
  int _count = 0;
  int _head = 0;            // index of next write
  uint32_t _newestT = 0;    // tMs of the most recent push (for dropout guard)
  bool _estimable = false;  // did the last push's fit succeed (cached for flow)
  EMA _ema{0.25f};

  // Fit a line to (time, weight) over the window using least-squares, and
  // return its slope as cg/s. Walks from the newest sample backwards until a
  // sample falls outside the window. Times are measured from the window start
  // only to keep the running sums small; that shift doesn't change the slope.
  bool _estimateRaw(float& out) const {
    if (_count < kMinPoints) return false;
    const int newestIdx = (_head - 1 + kRingSize) % kRingSize;
    const uint32_t newestT = _ring[newestIdx].tMs;

    double n = 0, sumT = 0, sumW = 0, sumTT = 0, sumTW = 0;
    uint32_t maxAge = 0;
    int cnt = 0;
    for (int i = 0; i < _count; ++i) {
      const int idx = (_head - 1 - i + 2 * kRingSize) % kRingSize;
      const uint32_t t = _ring[idx].tMs;
      const uint32_t age = newestT - t;
      if (age > kWindowMs) break;
      const double tt = static_cast<double>(kWindowMs - age);
      const double w = _ring[idx].cg;
      n += 1.0;
      sumT += tt;
      sumW += w;
      sumTT += tt * tt;
      sumTW += tt * w;
      if (age > maxAge) maxAge = age;
      ++cnt;
    }
    if (cnt < kMinPoints) return false;
    if (maxAge < kMinSpanMs) return false;
    const double denom = n * sumTT - sumT * sumT;
    if (denom == 0.0) return false;
    out = static_cast<float>((n * sumTW - sumT * sumW) / denom * 1000.0);
    return true;
  }
};

}  // namespace pump_scale
