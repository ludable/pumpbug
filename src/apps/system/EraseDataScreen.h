// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ui/Screen.h"
#include "ui/button.h"

// Diagnostics maintenance action: remove stored user data and reboot.
//
// The operation requests a boot-time LittleFS format, clears the raw core-dump
// partition, resets the shot counter, and asks WifiManager to remove Wi-Fi
// credentials and pairing tokens from NVS. The power-event log survives
// because it contains reset metadata rather than user data.
//
// Every erase attempt finishes the Wi-Fi/auth wipe and reboots after a short
// delay, even if a storage erase fails. Rebooting clears user data held in RAM
// and prevents /last from serving a record that is no longer on disk.
class EraseDataScreen : public Screen {
 public:
  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  // After confirmation this screen must keep ticking through the wipe and
  // reboot; leaving is safe only while it is still a cancellable prompt.
  bool allowsRootShortcut() const override { return _stage == Stage::Confirm; }
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;

 private:
  enum class Stage : uint8_t { Confirm, Erasing, Done, Incomplete };
  Stage _stage = Stage::Confirm;
  // Set after all erase steps have been attempted; tick() reboots once millis()
  // reaches it and the deferred Wi-Fi/auth wipe has finished.
  uint32_t _rebootAtMs = 0;
  static constexpr uint32_t kRebootDelayMs = 1500;

  void _performErase();
  void _drawConfirm(LGFX_Sprite* c);
  void _drawResult(LGFX_Sprite* c, const char* msg, const char* sub,
                   uint32_t color);
};
