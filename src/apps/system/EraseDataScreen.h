// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ui/Screen.h"
#include "ui/button.h"

class NetworkServicesHost;

// Stops storage consumers, erases the partitions used for device data, and
// reboots.
class EraseDataScreen : public Screen {
 public:
  explicit EraseDataScreen(NetworkServicesHost& networkServices)
      : _networkServices(networkServices) {}

  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  // After confirmation this screen must keep ticking through the wipe and
  // reboot; leaving is safe only while it is still a cancellable prompt.
  bool allowsRootShortcut() const override { return _stage == Stage::Confirm; }
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  void onPresented() override;
  ButtonHints buttonHints() const override;

 private:
  enum class Stage : uint8_t { Confirm, Erasing, Done, Incomplete };
  NetworkServicesHost& _networkServices;
  Stage _stage = Stage::Confirm;
  bool _erasePresented = false;
  uint32_t _rebootAtMs = 0;
  static constexpr uint32_t kRebootDelayMs = 1500;

  void _performErase();
  void _finishErase(Stage result);
  void _drawConfirm(LGFX_Sprite* c);
  void _drawResult(LGFX_Sprite* c, const char* msg, const char* sub,
                   uint32_t color);
};
