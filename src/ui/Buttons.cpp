// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "Buttons.h"

#include <M5Unified.h>

#include "sounds.h"

button::Gesture Buttons::_makeGesture(button::Button button,
                                      button::GestureKind kind) const {
  using button::Button;
  using button::Gesture;
  using button::GestureKind;
  if (button == Button::Both && kind == GestureKind::Long) {
    return Gesture::A_B_LONG;
  }

  const bool a = button == Button::A;
  switch (kind) {
    case GestureKind::Short:
      return a ? Gesture::A_SHORT : Gesture::B_SHORT;
    case GestureKind::Double:
      return a ? Gesture::A_DOUBLE : Gesture::B_DOUBLE;
    case GestureKind::Long:
      return a ? Gesture::A_LONG : Gesture::B_LONG;
    case GestureKind::Repeat:
      return a ? Gesture::A_REPEAT : Gesture::B_REPEAT;
  }
  return Gesture::NONE;
}

button::Gesture Buttons::_update(m5::Button_Class& btn, button::Button button,
                                 m5::Button_Class& other, HoldState& hold,
                                 ClickState& cs, uint32_t nowMs) {
  using button::Gesture;
  using button::GestureKind;
  if (btn.wasHold()) {
    cs.count = 0;  // a hold cancels any pending click
    if (_twoButtonLongEnabled && other.isPressed() && !other.isHolding()) {
      // Race against A_B_LONG: defer this LONG. If the other button reaches
      // hold within the grace window, the both-branch in poll() will emit
      // A_B_LONG and clear pendingLong before we ever resolve here.
      hold.pendingLong = true;
      hold.deferredMs = nowMs;
      return Gesture::NONE;
    }
    return _makeGesture(button, GestureKind::Long);
  }

  // Resolve deferred LONG when the grace window elapses. (The both-branch
  // clears pendingLong if both reach hold first.)
  if (hold.pendingLong && nowMs - hold.deferredMs >= _twoButtonGraceMs) {
    hold.pendingLong = false;
    return _makeGesture(button, GestureKind::Long);
  }

  // Fast path: M5Unified decides SHORT/DOUBLE on its own (DOUBLE shares the
  // hold threshold). SHORT fires immediately on release; DOUBLE is unused
  // unless the caller opts into deferred mode below.
  if (!cs.enabled) {
    if (btn.wasClicked()) return _makeGesture(button, GestureKind::Short);
    return Gesture::NONE;
  }

  // Deferred mode: we drive our own click-count timer off wasClicked() so
  // the double-click window is decoupled from the hold threshold.
  if (btn.wasClicked()) {
    if (cs.count == 0) cs.firstClickMs = nowMs;
    cs.count++;
    if (cs.count >= 2) {
      cs.count = 0;
      return _makeGesture(button, GestureKind::Double);
    }
  }

  if (cs.count == 1 && nowMs - cs.firstClickMs >= cs.windowMs) {
    cs.count = 0;
    return _makeGesture(button, GestureKind::Short);
  }

  return Gesture::NONE;
}

void Buttons::setLongPressThreshold(uint32_t ms, button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) M5.BtnA.setHoldThresh(ms);
  if (button::hasButton(buttons, button::Button::B)) M5.BtnB.setHoldThresh(ms);
}

void Buttons::setAutoRepeat(uint32_t initialMs, uint32_t intervalMs,
                            button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) {
    _repeatA.initialMs = initialMs;
    _repeatA.intervalMs = intervalMs;
    // Don't reset lastEventMs: lets callers change cadence mid-hold (e.g.,
    // accelerate) with a smooth transition
  }
  if (button::hasButton(buttons, button::Button::B)) {
    _repeatB.initialMs = initialMs;
    _repeatB.intervalMs = intervalMs;
    // Same as above
  }
}

void Buttons::enableAutoRepeat(button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) _repeatA.enabled = true;
  if (button::hasButton(buttons, button::Button::B)) _repeatB.enabled = true;
}

void Buttons::clearAutoRepeat(button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) _repeatA.holdStartMs = 0;
  if (button::hasButton(buttons, button::Button::B)) _repeatB.holdStartMs = 0;
}

void Buttons::disableAutoRepeat(button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) _repeatA.enabled = false;
  if (button::hasButton(buttons, button::Button::B)) _repeatB.enabled = false;
}

void Buttons::setDoublePressWindow(uint16_t windowMs, button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) {
    _clickA.windowMs = windowMs;
  }
  if (button::hasButton(buttons, button::Button::B)) {
    _clickB.windowMs = windowMs;
  }
}

void Buttons::enableDoublePress(button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) {
    _clickA.enabled = true;
    _clickA.count = 0;
  }
  if (button::hasButton(buttons, button::Button::B)) {
    _clickB.enabled = true;
    _clickB.count = 0;
  }
}

void Buttons::disableDoublePress(button::Button buttons) {
  if (button::hasButton(buttons, button::Button::A)) {
    _clickA.enabled = false;
    _clickA.count = 0;
  }
  if (button::hasButton(buttons, button::Button::B)) {
    _clickB.enabled = false;
    _clickB.count = 0;
  }
}

void Buttons::setTwoButtonGrace(uint32_t graceMs) {
  _twoButtonGraceMs = graceMs;
}

void Buttons::enableTwoButtonLong() { _twoButtonLongEnabled = true; }

void Buttons::disableTwoButtonLong() {
  _twoButtonLongEnabled = false;
  _holdA.pendingLong = false;
  _holdB.pendingLong = false;
}

// Returns true if a repeat should fire a REPEAT event
static bool checkRepeat(const m5::Button_Class& btn,
                        Buttons::RepeatState& state, uint32_t nowMs) {
  if (!state.enabled) return false;

  if (!btn.isHolding()) {
    state.lastEventMs = 0;
    return false;
  }

  // Button is holding, but repeat may have been cleared
  if (state.holdStartMs == 0) return false;

  // First repeat: wait initialMs from hold-start (the LONG event)
  if (state.lastEventMs == 0) {
    if (nowMs - state.holdStartMs >= state.initialMs) {
      state.lastEventMs = nowMs;
      return true;
    }
    return false;
  }

  // Subsequent repeats: wait intervalMs from last repeat
  if (nowMs - state.lastEventMs >= state.intervalMs) {
    state.lastEventMs = nowMs;
    return true;
  }

  return false;
}

Buttons::PollResult Buttons::poll() {
  using button::Button;
  using button::Gesture;
  using button::GestureKind;
  const uint32_t now = millis();
  Button pressed = Button::None;
  if (M5.BtnA.wasPressed()) pressed = Button::A;
  if (M5.BtnB.wasPressed()) {
    pressed = button::hasButton(pressed, Button::A) ? Button::Both : Button::B;
  }

  if (M5.BtnA.isHolding() && M5.BtnB.isHolding()) {
    if (_was_both_holding) return {pressed, Gesture::NONE};
    _was_both_holding = true;
    // A pending single LONG (deferred by the two-button grace) is subsumed
    // by A_B_LONG -- drop it. Same for any pending click.
    _clickA.count = 0;
    _clickB.count = 0;
    _holdA.pendingLong = false;
    _holdB.pendingLong = false;
    return {pressed, _makeGesture(Button::Both, GestureKind::Long)};
  } else {
    if (_was_both_holding) {
      // Restart repeat clock after exiting two-button gesture
      if (M5.BtnA.isHolding()) _repeatA.holdStartMs = now;
      if (M5.BtnB.isHolding()) _repeatB.holdStartMs = now;
    }
    _was_both_holding = false;
  }

  const Gesture evA =
      _update(M5.BtnA, Button::A, M5.BtnB, _holdA, _clickA, now);
  const Gesture evB =
      _update(M5.BtnB, Button::B, M5.BtnA, _holdB, _clickB, now);

  if (evA == Gesture::A_LONG) _repeatA.holdStartMs = now;
  if (evB == Gesture::B_LONG) _repeatB.holdStartMs = now;

  // Return single-shot events immediately (SHORT / LONG / DOUBLE)
  if (evA != Gesture::NONE) {
    return {pressed, evA};
  }
  if (evB != Gesture::NONE) {
    return {pressed, evB};
  }

  if (checkRepeat(M5.BtnA, _repeatA, now)) {
    return {pressed, Gesture::A_REPEAT};
  }
  if (checkRepeat(M5.BtnB, _repeatB, now)) {
    return {pressed, Gesture::B_REPEAT};
  }

  return {pressed, Gesture::NONE};
}

void button::feedback::pressed() { sounds::buttonPress(); }

void button::feedback::accepted(Gesture gesture) {
  switch (kindOf(gesture)) {
    case GestureKind::Long:
      sounds::buttonHold();
      break;
    case GestureKind::Double:
      sounds::buttonDoublePress();
      break;
    default:
      break;
  }
}
