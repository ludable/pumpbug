// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "ui/Screen.h"

class ResetShotCounterScreen : public Screen {
 public:
  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;

 private:
  enum class Stage : uint8_t { Confirm, Failed };

  Stage _stage = Stage::Confirm;
};
