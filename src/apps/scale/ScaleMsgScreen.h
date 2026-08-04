// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ble/BleScaleService.h"
#include "ui/Screen.h"
#include "ui/button.h"

// Diagnostics -> Scale msgs. Connects the scale and arms BleScaleService's
// passive message-log tap, showing live per-type counts of the BLE traffic
// (RX events + rejects, TX commands). The full hex+tag timeline lives on the
// web diagnostics page; the 135px screen just shows the connection state and
// the counters. B exits (which disarms + disconnects).
class ScaleMsgScreen : public Screen {
 public:
  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  void onLayoutChanged() override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  uint32_t desiredTickMs() const override { return 250; }

 private:
  BleScaleService::State _state = BleScaleService::State::OFF;
  BleScaleService::MsgLogSnapshot _log;
};
