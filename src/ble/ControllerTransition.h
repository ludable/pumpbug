// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "ble/bluetooth_controller_state.h"

namespace ble {

// Tracks the result and timeout for one asynchronous Bluetooth controller
// transition. Begin the wait only after BleScaleService accepts the matching
// request. The caller chooses its prerequisite checks and failure messages.
class ControllerTransition {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 5000;

  enum class Target : uint8_t { Running, Stopped };
  enum class Result : uint8_t {
    Inactive,
    Pending,
    Succeeded,
    Failed,
    TimedOut,
  };

  [[nodiscard]] bool begin(Target target, uint32_t nowMs,
                           uint32_t timeoutMs = kDefaultTimeoutMs) {
    if (_result == Result::Pending) return false;
    _target = target;
    _startedMs = nowMs;
    _timeoutMs = timeoutMs;
    _result = Result::Pending;
    return true;
  }

  Result update(ControllerState state, uint32_t nowMs) {
    if (_result != Result::Pending) return _result;
    if (state == ControllerState::Failed) {
      _result = Result::Failed;
    } else if ((_target == Target::Running &&
                state == ControllerState::Running) ||
               (_target == Target::Stopped &&
                state == ControllerState::Stopped)) {
      _result = Result::Succeeded;
    } else if (nowMs - _startedMs >= _timeoutMs) {
      _result = Result::TimedOut;
    }
    return _result;
  }

  bool pending() const { return _result == Result::Pending; }
  Target target() const { return _target; }
  Result result() const { return _result; }
  void reset() { _result = Result::Inactive; }

 private:
  Target _target = Target::Running;
  Result _result = Result::Inactive;
  uint32_t _startedMs = 0;
  uint32_t _timeoutMs = 0;
};

}  // namespace ble
