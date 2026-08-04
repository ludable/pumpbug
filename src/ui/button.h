// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace button {

enum class Button : uint8_t {
  None = 0,
  A = 1 << 4,
  B = 1 << 5,
  Both = static_cast<uint8_t>(A) | static_cast<uint8_t>(B),
};

enum class GestureKind : uint8_t {
  None = 0,
  Short = 1 << 0,
  Double = 1 << 1,
  Long = 1 << 2,
  Repeat = 1 << 3,
};

enum class Gesture : uint8_t {
  NONE = 0,
  A_SHORT = static_cast<uint8_t>(Button::A) |
            static_cast<uint8_t>(GestureKind::Short),
  A_DOUBLE = static_cast<uint8_t>(Button::A) |
             static_cast<uint8_t>(GestureKind::Double),
  A_LONG =
      static_cast<uint8_t>(Button::A) | static_cast<uint8_t>(GestureKind::Long),
  A_REPEAT = static_cast<uint8_t>(Button::A) |
             static_cast<uint8_t>(GestureKind::Repeat),
  B_SHORT = static_cast<uint8_t>(Button::B) |
            static_cast<uint8_t>(GestureKind::Short),
  B_DOUBLE = static_cast<uint8_t>(Button::B) |
             static_cast<uint8_t>(GestureKind::Double),
  B_LONG =
      static_cast<uint8_t>(Button::B) | static_cast<uint8_t>(GestureKind::Long),
  B_REPEAT = static_cast<uint8_t>(Button::B) |
             static_cast<uint8_t>(GestureKind::Repeat),
  A_B_LONG = static_cast<uint8_t>(Button::A) | static_cast<uint8_t>(Button::B) |
             static_cast<uint8_t>(GestureKind::Long),
};

constexpr bool hasButton(Button buttons, Button button) {
  const uint8_t mask = static_cast<uint8_t>(button);
  return mask != 0 && (static_cast<uint8_t>(buttons) & mask) == mask;
}

constexpr bool hasButton(Gesture gesture, Button button) {
  const uint8_t mask = static_cast<uint8_t>(button);
  return mask != 0 && (static_cast<uint8_t>(gesture) & mask) == mask;
}

constexpr GestureKind kindOf(Gesture gesture) {
  constexpr uint8_t kindMask = static_cast<uint8_t>(GestureKind::Short) |
                               static_cast<uint8_t>(GestureKind::Double) |
                               static_cast<uint8_t>(GestureKind::Long) |
                               static_cast<uint8_t>(GestureKind::Repeat);
  return static_cast<GestureKind>(static_cast<uint8_t>(gesture) & kindMask);
}

// One gesture after physical and diagnostic input have been combined.
// Diagnostic input doesn't produce feedback.
struct Event {
  Gesture gesture = Gesture::NONE;
  bool physical = false;
};

// Produces immediate physical-button feedback and confirms gestures accepted
// by the foreground Screen.
namespace feedback {

void pressed();
void accepted(Gesture gesture);

}  // namespace feedback
}  // namespace button
