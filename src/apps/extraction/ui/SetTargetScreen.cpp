// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "SetTargetScreen.h"

#include <cstdio>

#include "apps/extraction/ui/TargetStore.h"
#include "ui/blocks.h"
#include "ui/fonts.h"
#include "ui/gauge.h"
#include "ui/layout.h"
#include "ui/theme.h"

void SetTargetScreen::onEnter() {
  // Seed the digits from the shared store (load() is idempotent — a no-op once
  // ExtractionScreen has loaded it at boot).
  _store.load();
  const uint16_t cg = _store.targetCg();
  const uint16_t grams = cg / 100;  // whole grams, 0-99
  _tens = static_cast<uint8_t>((grams / 10) % 10);
  _ones = static_cast<uint8_t>(grams % 10);
  _half = (cg % 100) >= 50 ? 1 : 0;
  // An unset target (0) seeds the tens digit to 1 rather than an uneditable 0,
  // so editing opens at the minimum sensible target.
  if (_tens == 0) _tens = 1;
  _armed = _store.armed();
  _field = Field::None;
  _blinkOn = true;
  requestDraw();
}

void SetTargetScreen::onExit() { persist(); }

void SetTargetScreen::persist() const {
  // Unchanged values are a no-op write: setTargetAndArmed compares against the
  // store's current (in-memory) value first.
  _store.setTargetAndArmed(valueCg(), _armed);
}

void SetTargetScreen::advanceField() {
  switch (_field) {
    case Field::None:
      _field = Field::Tens;
      break;
    case Field::Tens:
      _field = Field::Ones;
      break;
    case Field::Ones:
      _field = Field::Half;
      break;
    case Field::Half:
      _field = Field::Armed;
      break;
    case Field::Armed:
      _field = Field::None;
      break;
  }
  _blinkOn = true;  // show the newly-selected field immediately
}

void SetTargetScreen::bumpField() {
  switch (_field) {
    case Field::Tens:
      // Never rest at 0: that would allow a target below
      // pump_scale::kMinTargetCg, too small a yield to alert on.
      _tens = (_tens >= 9) ? 1 : _tens + 1;
      break;
    case Field::Ones:
      _ones = (_ones + 1) % 10;
      break;
    case Field::Half:
      _half ^= 1;
      break;
    case Field::Armed:
      _armed = !_armed;
      return;
    case Field::None:
      return;  // nothing selected yet; A-long enters editing
  }
  // Changing the target implies wanting its alert; the armed field still
  // turns it off explicitly.
  _armed = true;
}

ScreenResult SetTargetScreen::onEvent(button::Gesture event) {
  switch (event) {
    case button::Gesture::A_LONG:
      advanceField();
      requestDraw();
      return stay();
    case button::Gesture::A_SHORT:
      bumpField();
      requestDraw();
      return stay();
    case button::Gesture::B_SHORT:
      return exit();  // onExit() persists.
    default:
      return ignored();
  }
}

ScreenResult SetTargetScreen::tick() {
  // Blink the active field at ~2 Hz; nothing animates once settled.
  const bool on = (_field == Field::None) ? true : ((millis() / 500) % 2 == 0);
  if (on != _blinkOn) {
    _blinkOn = on;
    requestDraw();
  }
  return stay();
}

bool SetTargetScreen::onDraw(LGFX_Sprite* c) {
  constexpr int pad = 2;
  const int W = c->width();
  const int H = c->height();
  c->fillScreen(theme::bg());

  const int headerH = ui::drawViewHeader(c, "SET TARGET", theme::accent());

  // Big value as "TT.H" with a leading zero so every digit has a stable column
  // to blink in (e.g. "05.5"). The active digit is blanked on the blink-off
  // phase; drawFixedColumn keeps the other digits in place.
  char value[8];
  std::snprintf(value, sizeof(value), "%02d.%d", _tens * 10 + _ones, _half * 5);
  if (!_blinkOn) {
    int idx = -1;
    if (_field == Field::Tens)
      idx = 0;
    else if (_field == Field::Ones)
      idx = 1;
    else if (_field == Field::Half)
      idx = 3;
    if (idx >= 0) value[idx] = ' ';
  }
  const int valY = headerH;
  const int valH = (H - headerH) * 6 / 10;
  c->setTextColor(theme::fg(), theme::bg());
  gauge::drawFixedColumn(c, value, "g", pad, valY, W - 2 * pad, valH,
                         {.sizeRef = "88.8"});

  // Arm-state pill. Its label blinks when armed is the active field; the fill
  // colour still shows the current state so selection doesn't hide it.
  const int armY = valY + valH;
  const int armH = H - armY;
  const uint32_t bg = _armed ? theme::ok_fill() : theme::surface();
  const uint32_t fg = _armed ? theme::bg() : theme::dim();
  c->fillSmoothRoundRect(pad, armY + pad, W - 2 * pad, armH - 2 * pad, 4, bg);
  const bool hideArmLabel = _field == Field::Armed && !_blinkOn;
  if (!hideArmLabel) {
    c->setTextColor(fg, bg);
    layout::drawCenteredInBox(c, _armed ? "ON" : "OFF", pad, armY + pad,
                              W - 2 * pad, armH - 2 * pad, font::textFamily());
  }
  return true;
}

ButtonHints SetTargetScreen::buttonHints() const {
  // A-hold advances the cursor; A-tap changes the active field (so it has no
  // meaning until editing starts). B-short exits and saves.
  ButtonHints h{};
  switch (_field) {
    case Field::None:
      h.a.hold = {HintGlyph::Edit, "EDIT"};
      break;
    case Field::Tens:
    case Field::Ones:
      h.a.tap = {HintGlyph::None, "+1"};
      h.a.hold = {HintGlyph::None, "NEXT"};
      break;
    case Field::Half:
      h.a.tap = {HintGlyph::None, "+0.5"};
      h.a.hold = {HintGlyph::None, "NEXT"};
      break;
    case Field::Armed:
      h.a.tap = {HintGlyph::Forth, _armed ? "OFF" : "ON"};
      h.a.hold = {HintGlyph::None, "NEXT"};
      break;
  }
  h.b.tap = {HintGlyph::Back};
  return h;
}
