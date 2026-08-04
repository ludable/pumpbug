// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ui/Screen.h"
#include "util/storage.h"

// Recovery when shot history could not be prepared safely at boot. Formatting
// is requested here and completed after reboot, before network or UI tasks can
// access the filesystem.
class StorageRecoveryScreen : public Screen {
 public:
  bool shouldPresent() const;

  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  // Startup recovery must exit through its B action so root navigation can
  // choose onboarding or LIVE. Restarting must remain foreground as well.
  bool allowsRootShortcut() const override { return false; }
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;

 private:
  enum class Stage : uint8_t { Notice, Confirm, Restarting, RequestFailed };

  void drawNotice(LGFX_Sprite* canvas) const;
  void drawStatus(LGFX_Sprite* canvas, const char* message,
                  uint32_t color) const;

  Stage _stage = Stage::Notice;
  uint32_t _rebootAtMs = 0;
  static constexpr uint32_t kRebootDelayMs = 500;
};
