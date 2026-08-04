// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "button.h"

class Buttons {
 public:
  struct PollResult {
    button::Button pressed = button::Button::None;
    button::Gesture gesture = button::Gesture::NONE;
  };

  button::Gesture _makeGesture(button::Button button,
                               button::GestureKind kind) const;

  bool _was_both_holding = false;

  struct RepeatState {
    bool enabled = false;
    uint32_t initialMs = 400;   // hold time before first repeat
    uint32_t intervalMs = 200;  // interval between repeats
    uint32_t lastEventMs = 0;   // millis() of last generated repeat event
    uint32_t holdStartMs = 0;   // millis() when the long gesture fired
  } _repeatA, _repeatB;

  struct ClickState {
    // When disabled, a short gesture fires immediately on release. When
    // enabled, it waits `windowMs` for a possible second click, which produces
    // a double gesture instead.
    bool enabled = false;
    uint16_t windowMs = 250;
    uint8_t count = 0;
    uint32_t firstClickMs = 0;
  } _clickA, _clickB;

  struct HoldState {
    // Set when wasHold() fires but the other button is also currently
    // pressed (race against A_B_LONG). The LONG event is then withheld
    // until the grace window elapses (emit deferred LONG) or the other
    // button reaches hold too (both-branch in poll() emits A_B_LONG and
    // clears pendingLong).
    bool pendingLong = false;
    uint32_t deferredMs = 0;
  } _holdA, _holdB;

  bool _twoButtonLongEnabled = false;
  uint32_t _twoButtonGraceMs = 75;

  button::Gesture _update(m5::Button_Class& btn, button::Button button,
                          m5::Button_Class& other, HoldState& hold,
                          ClickState& cs, uint32_t nowMs);

 public:
  void setLongPressThreshold(uint32_t ms,
                             button::Button buttons = button::Button::Both);

  // Configure auto-repeat timings. While a button is held and auto-repeat
  // is enabled, the first repeat follows the long gesture (or exit from a
  // two-button gesture) after `initialMs`; further repeats follow every
  // `intervalMs` until release or disable/clear. Does not enable the feature.
  void setAutoRepeat(uint32_t initialMs, uint32_t intervalMs,
                     button::Button buttons = button::Button::Both);

  // Enable auto-repeat using the configured timings.
  void enableAutoRepeat(button::Button buttons = button::Button::Both);

  // Stop repeat events until user releases the button.
  // Will auto-repeat if user holds the button again.
  void clearAutoRepeat(button::Button buttons = button::Button::Both);

  // Permanently disable auto-repeat.
  void disableAutoRepeat(button::Button buttons = button::Button::Both);

  // Configure the double-press decision window.
  // Does not enable the feature on its own.
  void setDoublePressWindow(uint16_t windowMs,
                            button::Button buttons = button::Button::Both);

  // Enable double-press detection using the configured window. A short press
  // waits until that window expires so a second click can supersede it.
  void enableDoublePress(button::Button buttons = button::Button::Both);

  // Disable double-press detection and drop any pending click decision. Short
  // presses then fire immediately on release.
  void disableDoublePress(button::Button buttons = button::Button::Both);

  // Configure the grace window used to discriminate A_LONG / B_LONG from
  // A_B_LONG when two-button-long detection is enabled. When wasHold()
  // fires for one button while the other is also being pressed, the LONG
  // event is withheld for up to `graceMs`. If the other button reaches
  // hold during that window, A_B_LONG fires and the deferred single LONG
  // is dropped; otherwise the deferred LONG fires when the window expires.
  // Default 75 ms is generous for human timing skew but barely
  // perceptible. Single-button LONGs (no other button pressed) are never
  // deferred and pay no latency.
  void setTwoButtonGrace(uint32_t graceMs);

  // Enable A_B_LONG disambiguation. Off by default: A_LONG / B_LONG fire
  // immediately and A_B_LONG may follow when both reach hold.
  void enableTwoButtonLong();
  void disableTwoButtonLong();

  PollResult poll();
};
