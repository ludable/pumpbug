// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ui/Screen.h"
#include "util/storage.h"

// Lets the user retry unavailable shot storage, format it after confirmation,
// or continue using the timer and scale without recording shots.
class StorageRecoveryScreen : public Screen {
 public:
  bool shouldPresent() const;

  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  bool allowsRootShortcut() const override { return false; }
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  void onPresented() override;
  ButtonHints buttonHints() const override;

 private:
  enum class Stage : uint8_t {
    Notice,
    Confirm,
    Formatting,
    ActionFailed,
    Restarting
  };

  void drawStatus(LGFX_Sprite* canvas, const char* message,
                  uint32_t color) const;
  void restartSoon();

  Stage _stage = Stage::Notice;
  bool _formatPresented = false;
  uint32_t _rebootAtMs = 0;
  static constexpr uint32_t kRebootDelayMs = 500;
};
