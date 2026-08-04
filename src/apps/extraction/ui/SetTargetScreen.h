// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ui/Screen.h"
#include "ui/button.h"

class TargetStore;

// Standalone editor for the extraction target-weight stop alert.
//
// Launched from the main menu. Edits the shared TargetStore and persists on
// exit.
//
// Digit-by-digit editing. A-long advances the cursor through the fields
// (tens -> ones -> half-gram -> armed -> settled); A-short changes the active
// field; changing a numeric field also arms the alert, while the armed field
// can still turn it off explicitly. The active field blinks. B-short exits and
// saves.
class SetTargetScreen : public Screen {
 public:
  explicit SetTargetScreen(TargetStore& store) : _store(store) {}

  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  uint32_t desiredTickMs() const override { return 120; }
  ButtonHints buttonHints() const override;

 private:
  // Edit cursor. None = settled (nothing blinking); the numeric fields select a
  // single digit of the "TT.H g" readout.
  enum class Field : uint8_t { None, Tens, Ones, Half, Armed };
  Field _field = Field::None;
  uint8_t _tens = 0;  // 0-9, tens of grams
  uint8_t _ones = 0;  // 0-9, grams
  uint8_t _half = 0;  // 0 or 1 => .0 / .5 g
  bool _armed = false;
  // Current blink phase for the active field, advanced by tick().
  bool _blinkOn = true;

  // Edited weight in centigrams (0.0-99.5 g, 0.5 g resolution).
  uint16_t valueCg() const {
    return static_cast<uint16_t>((_tens * 10 + _ones) * 100 + _half * 50);
  }
  void advanceField();  // A-long: move the cursor to the next field
  void bumpField();     // A-short: change the active field's value
  void persist() const;

  TargetStore& _store;
};
