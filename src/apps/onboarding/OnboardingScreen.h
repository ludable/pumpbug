// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "OnboardingStore.h"
#include "ui/Screen.h"

class OnboardingScreen : public Screen {
 public:
  // Loads completion state. Returns true if this screen should open at boot.
  bool begin();
  // True when the current visit reached the final card and completed.
  bool completedThisVisit() const { return _completedThisVisit; }

  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  // First run owns B-hold's confirmation. Once complete, the Tips screen lets
  // an unhandled B-hold use the ordinary root shortcut.
  bool allowsRootShortcut() const override { return _store.isComplete(); }
  void onLayoutChanged() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;

 private:
  enum class Card : uint8_t {
    Buttons,
    Navigate,
    Hold,
    Power,
    Motion,
    Live,
    MainMenu,
    Count
  };
  struct CardInfo;

  ScreenResult _complete();
  ScreenResult _skip();
  bool _exerciseComplete() const;
  void _drawCard(LGFX_Sprite* canvas, const char* text,
                 bool showConfirmation) const;
  void _drawConfirmation(LGFX_Sprite* canvas) const;

  OnboardingStore _store;
  Card _card = Card::Buttons;
  bool _holdComplete = false;
  bool _rotationComplete = false;
  bool _completedThisVisit = false;
  bool _confirmExit = false;

  static const CardInfo _cards[static_cast<uint8_t>(Card::Count)];
};
