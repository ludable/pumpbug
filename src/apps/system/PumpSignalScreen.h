// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ui/Screen.h"
#include "ui/button.h"
#include "vibration/VibrationSensor.h"

// Shows whether the pump is detected, along with the signal-to-noise ratio,
// peak frequency, and motion values behind that decision. This lets mounting
// positions be tested without recording an extraction. Test transitions are
// omitted from the runtime Pump log.
class PumpSignalScreen : public Screen {
 public:
  explicit PumpSignalScreen(VibrationSensor& sensor) : _sensor(sensor) {}

  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  uint32_t desiredTickMs() const override { return 100; }

 private:
  void startSession();

  VibrationSensor& _sensor;
  VibrationSensor::Data _data{};
  uint32_t _sequence = 0;
  uint32_t _sessionStartedMs = 0;
  bool _running = false;
  bool _hasFrame = false;
  bool _sessionEnded = false;
};
