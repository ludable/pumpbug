// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdint>

#include "ble/ControllerTransition.h"

int main() {
  using State = ble::ControllerState;
  using Target = ble::ControllerTransition::Target;
  using Result = ble::ControllerTransition::Result;

  ble::ControllerTransition transition;
  assert(transition.result() == Result::Inactive);
  assert(transition.begin(Target::Stopped, 100, 5000));
  assert(!transition.begin(Target::Running, 100, 5000));
  assert(transition.update(State::Stopping, 5099) == Result::Pending);
  assert(transition.update(State::Stopped, 5100) == Result::Succeeded);

  transition.reset();
  assert(transition.begin(Target::Running, UINT32_MAX - 100, 200));
  assert(transition.update(State::Starting, 50) == Result::Pending);
  assert(transition.update(State::Starting, 100) == Result::TimedOut);

  assert(transition.begin(Target::Running, 1000, 5000));
  assert(transition.update(State::Failed, 6000) == Result::Failed);
  return 0;
}
