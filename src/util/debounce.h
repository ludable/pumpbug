// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <Arduino.h>

template <int minIntervalMs>
class Debounce {
  unsigned long _last = 0;

 public:
  bool update() {
    const unsigned long now = millis();
    if (now - _last < minIntervalMs) return false;
    _last = now;
    return true;
  }

  bool operator()() { return update(); }

  // Restart the interval: the next update() waits a full minIntervalMs from now.
  void reset() { _last = millis(); }
};
