// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "BackflushCycle.h"
#include "ui/Screen.h"
#include "ui/layout.h"
#include "vibration/VibrationSensor.h"

// Guides the operator through a backflush cleaning routine: it owns the
// vibration sensor session, says when to start and stop the pump, and shows
// the durations BackflushCycle measures.
//
// BackflushCycle decides what the pump is doing; this screen decides how long
// to keep watching. A session ends when the fifth cycle finishes, the operator
// starts over, nothing happens for several minutes, or the sensor stops
// reporting, and each of those leaves whatever was measured on screen.
//
// This screen doesn't add to shot history or create pump-detection log entries.
class BackflushScreen : public Screen {
 public:
  explicit BackflushScreen(VibrationSensor& sensor) : _sensor(sensor) {}

  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;
  uint32_t desiredTickMs() const override { return 100; }

 private:
  // Why a session stopped before the fifth cycle finished. The reason is shown
  // above the durations already measured, or on its own if there are none.
  enum class SessionEnd : uint8_t { None, Inactivity, SensorStalled };

  // Everything the active view draws from. Each tick compares this against the
  // last set drawn and repaints only when they differ, so a screen waiting on
  // the operator does not redraw an unchanged picture ten times a second.
  struct DisplayState {
    BackflushCycle::Phase phase = BackflushCycle::Phase::WaitingForPump;
    BackflushCycle::Instruction instruction =
        BackflushCycle::Instruction::StartPump;
    uint32_t elapsedTenths = 0;
    uint8_t cycleIndex = 0;
    bool hasSensorFrame = false;

    bool operator!=(const DisplayState& other) const {
      return phase != other.phase || instruction != other.instruction ||
             elapsedTenths != other.elapsedTenths ||
             cycleIndex != other.cycleIndex ||
             hasSensorFrame != other.hasSensorFrame;
    }
  };

  void startSession();
  void endSession(SessionEnd reason);
  void stopSensor();
  // The session has results to show once the first pump-on duration is
  // recorded.
  bool hasResults() const { return _cycle.onRecorded(0); }
  void updateAudio(uint32_t nowMs);
  DisplayState displayState(uint32_t nowMs) const;
  void drawActive(LGFX_Sprite* canvas, int headerH, uint32_t nowMs);
  void drawSummary(LGFX_Sprite* canvas, int headerH, const char* note,
                   uint32_t noteColor);
  void drawPortraitSummary(LGFX_Sprite* canvas, layout::rect body,
                           const char* valueSizeRef);
  void drawLandscapeSummary(LGFX_Sprite* canvas, layout::rect body,
                            const char* valueSizeRef);

  VibrationSensor& _sensor;
  BackflushCycle _cycle;
  // The analysis result last taken from the sensor.
  uint32_t _sensorSequence = 0;
  // When the routine last moved on, and when the summary appeared. One bounds
  // the idle timeout, the other how long the durations hold the device awake.
  uint32_t _lastProgressMs = 0;
  uint32_t _resultsShownMs = 0;
  DisplayState _drawnState{};
  // What the cues have already announced, so that each command and each
  // countdown second sounds once.
  BackflushCycle::Instruction _lastInstruction =
      BackflushCycle::Instruction::StartPump;
  bool _instructionAnnounced = false;
  uint8_t _lastCountdownSecond = 0;
  bool _pumpDetected = false;
  bool _sensorRunning = false;
  bool _hasSensorFrame = false;
  SessionEnd _sessionEnd = SessionEnd::None;
};
