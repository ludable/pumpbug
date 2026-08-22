// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ResetShotCounterScreen.h"

#include "apps/extraction/ShotCounter.h"
#include "ui/message_cards.h"

void ResetShotCounterScreen::onEnter() { _stage = Stage::Confirm; }

ScreenResult ResetShotCounterScreen::onEvent(button::Gesture event) {
  switch (event) {
    case button::Gesture::A_LONG:
      if (_shotCounter.reset()) return exit();
      _stage = Stage::Failed;
      requestDraw();
      return stay();
    case button::Gesture::B_SHORT:
      return exit();
    default:
      return ignored();
  }
}

bool ResetShotCounterScreen::onDraw(LGFX_Sprite* c) {
  if (_stage == Stage::Confirm) {
    ui::drawConfirmationScreen(c, "Reset counter?",
                               "This resets the status bar shot counter. Saved "
                               "shot records are kept.");
  } else {
    ui::drawCriticalMessageScreen(
        c, "Counter reset failed",
        "Device settings could not be saved. Retry or go back.");
  }
  return true;
}

ButtonHints ResetShotCounterScreen::buttonHints() const {
  return {
      {{}, Hint{HintGlyph::None, _stage == Stage::Confirm ? "RESET" : "RETRY"}},
      {Hint{HintGlyph::Back}}};
}
