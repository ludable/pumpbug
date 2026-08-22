// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "ExtractionController.h"
#include "ExtractionStream.h"
#include "ShotCounter.h"
#include "ui/ExtractionScreen.h"
#include "ui/ResetShotCounterScreen.h"
#include "ui/SetTargetScreen.h"
#include "ui/TargetStore.h"

class HttpServer;
class SseServer;

// Owns the extraction app: recording engine, settings, screens, and web stream.
class ExtractionApp {
 public:
  explicit ExtractionApp(VibrationSensor& vibrationSensor)
      : _controller(_shotCounter),
        _extractionScreen(_controller, _targetStore, vibrationSensor),
        _resetShotCounterScreen(_shotCounter) {}

  void begin() {
    _shotCounter.load();
    _extractionScreen.allocateRenderingBuffers();
  }

  uint64_t shotCount() const { return _shotCounter.value(); }

  ExtractionScreen& extractionScreen() { return _extractionScreen; }
  SetTargetScreen& setTargetScreen() { return _setTargetScreen; }
  ResetShotCounterScreen& resetShotCounterScreen() {
    return _resetShotCounterScreen;
  }

  void registerWith(HttpServer& http, SseServer& sse) {
    _controller.registerWith(http);
    _stream.registerWith(sse, http, _controller);
  }

  void notifySleeping() const { _stream.notifySleeping(); }

 private:
  pump_scale::ShotCounter _shotCounter;
  pump_scale::ExtractionController _controller;
  TargetStore _targetStore;
  ExtractionScreen _extractionScreen;
  SetTargetScreen _setTargetScreen{_targetStore};
  ResetShotCounterScreen _resetShotCounterScreen;
  pump_scale::ExtractionStream _stream;
};
