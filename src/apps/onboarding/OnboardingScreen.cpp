// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "OnboardingScreen.h"

#include "ui/blocks.h"
#include "ui/font_glyphs.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/message_cards.h"
#include "ui/theme.h"

namespace {
constexpr ButtonHint tapHint(Hint hint) { return {hint, {}, {}}; }
constexpr ButtonHint holdHint(Hint hint) { return {{}, hint, {}}; }

}  // namespace

struct OnboardingScreen::CardInfo {
  const char* title;
  const char* counter;
  const char* text;
  ButtonHints hints;
  ButtonHints completedHints;
};

const OnboardingScreen::CardInfo OnboardingScreen::_cards[] = {
    {"BUTTONS",
     "1/7",
     "This device has three buttons. Press A to begin.",
     {tapHint({HintGlyph::None, "A"}), tapHint({HintGlyph::None, "B"}),
      tapHint({HintGlyph::Power})},
     {}},
    {"A & B",
     "2/7",
     "Press A to move to the next screen. Press B to go back.",
     {tapHint({HintGlyph::None, "NEXT"}), tapHint({HintGlyph::Back}), {}},
     {}},
    {"HOLD A",
     "3/7",
     "Hold A to select, confirm, or perform actions.",
     {holdHint({HintGlyph::None, "TEST"}), tapHint({HintGlyph::Back}), {}},
     {tapHint({HintGlyph::None, "NEXT"}), tapHint({HintGlyph::Back}), {}}},
    {"POWER",
     "4/7",
     // Every card asks for the gesture it teaches, so the reader acts on each
     // one. The power card is deliberately descriptive: pressing that button
     // would interrupt the tour, so it is described rather than requested.
     "A single tap sleeps the device; a double-tap switches it off. On "
     "battery, it auto-sleeps when idle. Movement wakes it from sleep.",
     {tapHint({HintGlyph::None, "NEXT"}), tapHint({HintGlyph::Back}), {}},
     {}},
    {"MOTION",
     "5/7",
     "Rotating the device changes the screen orientation, and wakes "
     "the device from sleep. Try rotating now.",
     {{}, tapHint({HintGlyph::Back}), {}},
     {tapHint({HintGlyph::None, "NEXT"}), tapHint({HintGlyph::Back}), {}}},
    {"LIVE",
     "6/7",
     "The LIVE screen shows live weight and time, and keeps the last shot "
     "for review. Pressing B there opens the menu.",
     {tapHint({HintGlyph::None, "NEXT"}), tapHint({HintGlyph::Back}), {}},
     {}},
    {"MENU",
     "7/7",
     "The menu contains target settings, Wi-Fi, diagnostics, and Tips. Tap A "
     "to move, hold A to select, or press B to return to LIVE.",
     {tapHint({HintGlyph::None, "DONE"}), tapHint({HintGlyph::Back}), {}},
     {}},
};

namespace {
constexpr int kBadgeHeight = 24;
constexpr int kBadgeMargin = 6;
}  // namespace

bool OnboardingScreen::begin() {
  _store.load();
  return !_store.isComplete();
}

void OnboardingScreen::onEnter() {
  _card = Card::Buttons;
  _holdComplete = false;
  _rotationComplete = false;
  _completedThisVisit = false;
  _confirmExit = false;
  requestDraw();
}

ScreenResult OnboardingScreen::_complete() {
  _store.setComplete();
  _completedThisVisit = true;
  return exit();
}

ScreenResult OnboardingScreen::_skip() {
  _store.setComplete();
  // Normal completion introduces the menu. A deliberate skip returns to the
  // primary LIVE view through MainNavigation's ordinary exit fallback.
  _completedThisVisit = false;
  return exit();
}

bool OnboardingScreen::_exerciseComplete() const {
  switch (_card) {
    case Card::Hold:
      return _holdComplete;
    case Card::Motion:
      return _rotationComplete;
    default:
      return false;
  }
}

ScreenResult OnboardingScreen::onEvent(button::Gesture event) {
  if (_confirmExit) {
    if (event == button::Gesture::A_LONG) return _skip();
    if (event == button::Gesture::B_SHORT) {
      _confirmExit = false;
      requestDraw();
      return stay();
    }
    return ignored();
  }

  if (event == button::Gesture::B_LONG) {
    if (_store.isComplete()) return ignored();
    _confirmExit = true;
    requestDraw();
    return stay();
  }

  if (event == button::Gesture::B_SHORT) {
    if (_card == Card::Buttons) return exit();
    _card = static_cast<Card>(static_cast<uint8_t>(_card) - 1);
    requestDraw();
    return stay();
  }

  switch (_card) {
    case Card::Buttons:
    case Card::Navigate:
      if (event == button::Gesture::A_SHORT) {
        _card = static_cast<Card>(static_cast<uint8_t>(_card) + 1);
        requestDraw();
        return stay();
      }
      break;
    case Card::Hold:
      if (event == button::Gesture::A_LONG && !_holdComplete) {
        _holdComplete = true;
        requestDraw();
        return stay();
      } else if (event == button::Gesture::A_SHORT && _holdComplete) {
        _card = Card::Power;
        requestDraw();
        return stay();
      }
      break;
    case Card::Power:
      if (event == button::Gesture::A_SHORT) {
        _card = Card::Motion;
        requestDraw();
        return stay();
      }
      break;
    case Card::Motion:
      if (event == button::Gesture::A_SHORT && _rotationComplete) {
        _card = Card::Live;
        requestDraw();
        return stay();
      }
      break;
    case Card::Live:
      if (event == button::Gesture::A_SHORT) {
        _card = Card::MainMenu;
        requestDraw();
        return stay();
      }
      break;
    case Card::MainMenu:
      if (event == button::Gesture::A_SHORT) return _complete();
      break;
    case Card::Count:
      break;
  }
  return ignored();
}

void OnboardingScreen::onLayoutChanged() {
  if (_card == Card::Motion && !_rotationComplete) {
    _rotationComplete = true;
    requestDraw();
  }
}

void OnboardingScreen::_drawConfirmation(LGFX_Sprite* c) const {
  c->setFont(font::button_hint());
  c->setTextSize(1);
  const int glyphW = c->textWidth(font::glyph::OK);
  const int labelW = c->textWidth("GOT IT");
  constexpr int gap = 4;
  constexpr int pad = 7;
  const int width = glyphW + gap + labelW + 2 * pad;
  const int x = (c->width() - width) / 2;
  const int y = c->height() - kBadgeHeight - kBadgeMargin;

  c->fillRoundRect(x, y, width, kBadgeHeight, 3, theme::fg());
  c->setTextColor(theme::bg(), theme::fg());
  int textX = x + pad;
  layout::drawMiddleLeft(c, font::glyph::OK, textX, y + kBadgeHeight / 2);
  textX += glyphW + gap;
  layout::drawMiddleLeft(c, "GOT IT", textX, y + kBadgeHeight / 2);
}

void OnboardingScreen::_drawCard(LGFX_Sprite* c, const char* text,
                                 bool showConfirmation) const {
  const CardInfo& info = _cards[static_cast<uint8_t>(_card)];
  const int headerH =
      ui::drawViewHeader(c, info.title, theme::accent(), info.counter);
  int bodyH = c->height() - headerH;
  if (showConfirmation) bodyH -= kBadgeHeight + 2 * kBadgeMargin;
  const layout::rect body = layout::inset({0, headerH, c->width(), bodyH}, 6);
  c->setTextColor(theme::fg(), theme::bg());
  layout::drawWrappedCentered(c, text, body, font::textFamily());
  if (showConfirmation) _drawConfirmation(c);
}

bool OnboardingScreen::onDraw(LGFX_Sprite* c) {
  if (_confirmExit) {
    ui::drawConfirmationScreen(c, "Exit Tips?",
                               "This marks the guide complete. You can open "
                               "Tips again from the menu.");
    return true;
  }

  c->fillScreen(theme::bg());
  const CardInfo& info = _cards[static_cast<uint8_t>(_card)];
  const bool confirmed = _exerciseComplete();
  _drawCard(c, info.text, confirmed);
  return true;
}

ButtonHints OnboardingScreen::buttonHints() const {
  if (_confirmExit) {
    return {{{}, Hint{HintGlyph::None, "EXIT"}}, {Hint{HintGlyph::Back}}};
  }
  const CardInfo& info = _cards[static_cast<uint8_t>(_card)];
  return _exerciseComplete() ? info.completedHints : info.hints;
}
