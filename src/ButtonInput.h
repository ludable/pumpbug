// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "power/PowerManager.h"
#include "ui/Buttons.h"
#include "ui/UiDebugRemote.h"
#include "ui/button.h"

// Converts physical button gestures into UI input and wakes a dimmed display.
class ButtonInput {
 public:
  explicit ButtonInput(UiDebugRemote& debugRemote)
      : _debugRemote(debugRemote) {}

  void begin() { _debugRemote.begin(); }

  button::Event poll() {
    const Buttons::PollResult physical = _physicalButtons.poll();

    if (physical.pressed != button::Button::None) {
      button::feedback::pressed();
      if (power::powerManager.isScreenDimmed() &&
          button::hasButton(physical.pressed, button::Button::A)) {
        // When dimmed, a tap on the A button simply undims and doesn't perform
        // any action.
        _consumeAShort = true;
      }
      power::powerManager.notifyActivity();
    }

    button::Gesture gesture = physical.gesture;
    if (button::hasButton(gesture, button::Button::A)) {
      if (_consumeAShort && gesture == button::Gesture::A_SHORT) {
        gesture = button::Gesture::NONE;
      }
      _consumeAShort = false;
    }

    if (gesture != button::Gesture::NONE) return {gesture, true};

    gesture = _debugRemote.poll();
    if (gesture != button::Gesture::NONE) {
      power::powerManager.notifyActivity();
    }
    return {gesture, false};
  }

 private:
  Buttons _physicalButtons;
  UiDebugRemote& _debugRemote;
  bool _consumeAShort = false;
};
