// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cmath>

// 1st-order high-pass filter (DC-blocker style):
//   y[n] = a * (y[n-1] + x[n] - x[n-1])
// `a` close to 1.0  → lower cutoff frequency.
// `a` closer to 0.0 → higher cutoff frequency.
class HPF {
 public:
  // Construct directly with the filter coefficient (0 < a < 1).
  constexpr explicit HPF(float a) : _a(a) {}

  // Construct from a desired -3 dB cutoff frequency and the sample rate (both
  // in Hz).
  static HPF fromCutoff(float fc_hz, float fs_hz) {
    return HPF(std::exp(-2.0f * M_PI * fc_hz / fs_hz));
  }

  // Process one sample.
  float operator()(float x) {
    const float y = _a * (_yPrev + x - _xPrev);
    _yPrev = y;
    _xPrev = x;
    return y;
  }

  // Clear filter state. Use when starting a new stream so the transient settles
  // from zero rather than from whatever the last invocation left behind.
  void reset() {
    _xPrev = 0.0f;
    _yPrev = 0.0f;
  }

  float alpha() const { return _a; }

 private:
  float _a;
  float _xPrev = 0.0f;
  float _yPrev = 0.0f;
};