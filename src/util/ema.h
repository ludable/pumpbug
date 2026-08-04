// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// First-order exponential moving average:
//   y[n] = α·x[n] + (1-α)·y[n-1]
//
// The first sample primes the filter (y[0] = x[0]) rather than warming up
// from zero, which avoids a long initial transient.
//
// Callers decide which inputs reach update() — e.g. skip non-finite values, or
// only update when a stationarity/quality condition holds. Without such a
// check a single NaN poisons the state forever, since α·nan + (1-α)·y = nan.
class EMA {
 public:
  constexpr explicit EMA(float alpha) : _alpha(alpha) {}

  float update(float x) {
    if (!_primed) {
      _y = x;
      _primed = true;
    } else {
      _y += _alpha * (x - _y);
    }
    return _y;
  }

  float value() const { return _y; }
  bool primed() const { return _primed; }
  float alpha() const { return _alpha; }

  // Returns to default state. The next update() will prime from its input.
  void reset() {
    _y = 0.0f;
    _primed = false;
  }

 private:
  float _alpha;
  float _y = 0.0f;
  bool _primed = false;
};
