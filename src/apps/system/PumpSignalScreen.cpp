// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "PumpSignalScreen.h"

#include <cmath>
#include <cstdio>
#include <iterator>

#include "power/PowerManager.h"
#include "ui/blocks.h"
#include "ui/theme.h"
#include "vibration/VibrationWindowTrigger.h"

namespace {

constexpr uint32_t kSessionMs = 5 * 60 * 1000;

void formatFinite(float value, const char* suffix, int decimals, char* out,
                  size_t size) {
  if (!std::isfinite(value)) {
    std::snprintf(out, size, "%s", "--");
    return;
  }
  std::snprintf(out, size, "%.*f %s", decimals, value, suffix);
}

}  // namespace

void PumpSignalScreen::startSession() {
  _data = {};
  _hasFrame = false;
  _sessionEnded = false;
  _running = _sensor.begin();
  if (_running) {
    _sequence = _sensor.seq();
    _sessionStartedMs = millis();
    power::powerManager.keepAwake();
  }
}

void PumpSignalScreen::onEnter() {
  _sequence = 0;
  startSession();
}

void PumpSignalScreen::onExit() {
  if (_running) {
    _sensor.end();
    _running = false;
  }
}

ScreenResult PumpSignalScreen::onEvent(button::Gesture event) {
  if (event == button::Gesture::B_SHORT) return exit();
  if (event == button::Gesture::A_SHORT && !_running) {
    startSession();
    requestDraw();
    return stay();
  }
  return ignored();
}

ScreenResult PumpSignalScreen::tick() {
  if (!_running) return stay();
  if (millis() - _sessionStartedMs >= kSessionMs) {
    _sensor.end();
    _running = false;
    _sessionEnded = true;
    requestDraw();
    return stay();
  }
  // Placement testing needs a continuously visible signal. The session
  // timeout stops the IMU and FFT if the screen is left unattended.
  power::powerManager.keepAwake();
  const uint32_t sequence = _sensor.seq();
  if (sequence != _sequence) {
    _sequence = _sensor.snapshot(_data);
    _hasFrame = true;
    requestDraw();
  }
  return stay();
}

bool PumpSignalScreen::onDraw(LGFX_Sprite* c) {
  using Trigger = VibrationWindowTrigger;

  c->fillScreen(theme::bg());
  const int headerH = ui::drawViewHeader(c, "PUMP SIGNAL", theme::accent());
  if (!_running) {
    ui::drawCardBody(c, headerH,
                     _sessionEnded ? "Test ended. Press A to restart."
                                   : "Sensor unavailable. Press A to retry.");
    return true;
  }
  if (!_hasFrame) {
    ui::drawCardBody(c, headerH, "Starting vibration sensor...");
    return true;
  }

  const auto& f = _data.triggerFeatures;
  const float snrThreshold =
      _data.pumpSignal.isOn() ? Trigger::SNR_STAY_DB : Trigger::SNR_ON_DB;
  const bool snrPass =
      std::isfinite(f.decisionSnrDb) && f.decisionSnrDb >= snrThreshold;
  const bool peakPass = std::isfinite(f.peakHz) &&
                        f.peakHz >= Trigger::PEAK_MIN_HZ &&
                        f.peakHz <= Trigger::PEAK_MAX_HZ;

  char snrLabel[32];
  char peakLabel[32];
  char motionLabel[32];
  char snrValue[20];
  char peakValue[20];
  char motionValue[24];
  std::snprintf(snrLabel, sizeof(snrLabel), "SNR (ON %.0f / STAY %.0f)",
                Trigger::SNR_ON_DB, Trigger::SNR_STAY_DB);
  std::snprintf(peakLabel, sizeof(peakLabel), "PEAK (%.0f-%.0f Hz)",
                Trigger::PEAK_MIN_HZ, Trigger::PEAK_MAX_HZ);
  std::snprintf(motionLabel, sizeof(motionLabel), "MOTION (FLUX <%.1f)",
                VibrationSensor::STATIONARY_FLUX_MAX);
  formatFinite(f.decisionSnrDb, "dB", 1, snrValue, sizeof(snrValue));
  formatFinite(f.peakHz, "Hz", 0, peakValue, sizeof(peakValue));
  if (std::isfinite(f.spectralFlux)) {
    std::snprintf(motionValue, sizeof(motionValue), "%s %.1f",
                  f.stationary ? "STILL" : "MOVING", f.spectralFlux);
  } else {
    std::snprintf(motionValue, sizeof(motionValue), "--");
  }

  const char* pumpStateText = "NOT DETECTED";
  uint32_t pumpStateColor = theme::fg();
  if (_data.pumpSignal.isDecayCandidate()) {
    pumpStateText = "DECAY";
    pumpStateColor = theme::warn();
  } else if (_data.pumpSignal.isOn()) {
    pumpStateText = "DETECTED";
    pumpStateColor = theme::ok();
  }

  const bool landscape = c->width() > c->height();
  const ui::InfoBlock blocks[] = {
      {"STATE", pumpStateText, pumpStateColor, 34},
      {landscape ? "SNR" : snrLabel, snrValue,
       snrPass ? theme::ok() : theme::warn(), 28},
      {landscape ? "PEAK" : peakLabel, peakValue,
       peakPass ? theme::ok() : theme::warn(), 28},
      {landscape ? "MOTION" : motionLabel, motionValue,
       f.stationary ? theme::ok() : theme::warn(), 28},
  };
  ui::drawInfoScreen(c, headerH, nullptr, nullptr, blocks,
                     static_cast<int>(std::size(blocks)), theme::bg(),
                     snrLabel);
  return true;
}
