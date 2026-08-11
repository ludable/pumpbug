// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "BackflushScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "power/PowerManager.h"
#include "ui/blocks.h"
#include "ui/fonts.h"
#include "ui/gauge.h"
#include "ui/layout.h"
#include "ui/sounds.h"
#include "ui/theme.h"

namespace {

// How long since the routine last moved on before the session gives up.
constexpr uint32_t kInactivityMs = 5 * 60 * 1000;
// How long to wait for a new analysis result before treating the sensor as
// stopped. VibrationSensor updates the state every ~160 ms, so a gap this large
// is a failure rather than a late batch.
constexpr uint32_t kSensorStaleMs = 2000;
// How long the finished durations hold the device awake. They are not stored
// anywhere, so sleeping loses them, but a screen nobody is reading should not
// override the idle policy for longer than this.
constexpr uint32_t kSummaryAwakeMs = 2 * 60 * 1000;
constexpr int kBodyPad = 4;
// Height of the line above the summary table that names the outcome.
constexpr int kNoteH = 12;
// Widest heading in the backflush summary table.
constexpr const char* kHeadingSizeRef = "OFF";

const char* instructionText(BackflushCycle::Instruction instruction) {
  switch (instruction) {
    case BackflushCycle::Instruction::StartPump:
      return "START PUMP";
    case BackflushCycle::Instruction::PumpOn:
      return "PUMP ON";
    case BackflushCycle::Instruction::StopPump:
      return "STOP PUMP";
    case BackflushCycle::Instruction::Wait:
      return "WAIT";
    case BackflushCycle::Instruction::Complete:
      return "COMPLETE";
  }
  return "";
}

uint32_t instructionColor(BackflushCycle::Instruction instruction) {
  switch (instruction) {
    case BackflushCycle::Instruction::StartPump:
      return theme::accent();
    case BackflushCycle::Instruction::PumpOn:
      return theme::ok();
    case BackflushCycle::Instruction::StopPump:
      return theme::alarm();
    case BackflushCycle::Instruction::Wait:
      return theme::dim();
    case BackflushCycle::Instruction::Complete:
      return theme::ok();
  }
  return theme::fg();
}

struct HeadlineLines {
  const char* first;
  const char* second;
};

HeadlineLines portraitHeadline(BackflushCycle::Instruction instruction) {
  switch (instruction) {
    case BackflushCycle::Instruction::StartPump:
      return {"START", "PUMP"};
    case BackflushCycle::Instruction::PumpOn:
      return {"PUMP", "ON"};
    case BackflushCycle::Instruction::StopPump:
      return {"STOP", "PUMP"};
    case BackflushCycle::Instruction::Wait:
      return {"WAIT", nullptr};
    case BackflushCycle::Instruction::Complete:
      return {"COMPLETE", nullptr};
  }
  return {"", nullptr};
}

void drawHeadline(LGFX_Sprite* c, BackflushCycle::Instruction instruction,
                  layout::rect box, bool portrait) {
  const uint32_t color = instructionColor(instruction);
  if (!portrait) {
    ui::drawFittedText(c, instructionText(instruction), box, color, theme::bg(),
                       {.sizeRef = "START PUMP"});
    return;
  }

  const HeadlineLines lines = portraitHeadline(instruction);
  if (!lines.second) {
    ui::drawFittedText(c, lines.first, box, color, theme::bg(),
                       {.sizeRef = lines.first});
    return;
  }
  const auto split = layout::splitV(box, 0.5f, 0);
  ui::drawFittedText(c, lines.first, split.first, color, theme::bg(),
                     {.sizeRef = "START"});
  ui::drawFittedText(c, lines.second, split.second, color, theme::bg(),
                     {.sizeRef = "START"});
}

void drawCycleIndicator(LGFX_Sprite* c, layout::rect box, size_t cycleIndex,
                        bool portrait) {
  char count[12];
  std::snprintf(count, sizeof(count), "%u / %u",
                static_cast<unsigned>(cycleIndex + 1),
                static_cast<unsigned>(BackflushCycle::CYCLE_COUNT));
  if (!portrait) {
    char label[24];
    std::snprintf(label, sizeof(label), "CYCLE %s", count);
    ui::drawFittedText(c, label, box, theme::accent_light(), theme::bg(),
                       {.sizeRef = "CYCLE 5 / 5"});
    return;
  }

  const auto split = layout::splitV(box, 0.36f, 1);
  ui::drawFittedText(c, "CYCLE", split.first, theme::dim(), theme::bg(),
                     {.sizeRef = "CYCLE"});
  ui::drawFittedText(c, count, split.second, theme::accent_light(), theme::bg(),
                     {.sizeRef = "5 / 5"});
}

// The countdown for the running period. It holds at zero after the target,
// while the final summary retains the measured duration.
void formatPhaseTime(uint32_t elapsed, char* out, size_t size) {
  if (elapsed >= BackflushCycle::PHASE_TARGET_MS) {
    std::snprintf(out, size, "0.0");
    return;
  }

  const uint32_t remainingTenths =
      (BackflushCycle::PHASE_TARGET_MS - elapsed + 99) / 100;
  std::snprintf(out, size, "%lu.%lu",
                static_cast<unsigned long>(remainingTenths / 10),
                static_cast<unsigned long>(remainingTenths % 10));
}

// One duration for the summary table, or "-" for a period that was not part of
// the routine or never ended.
// Nothing is marked good or bad: detection takes about as long to report a
// change as any tolerance worth checking would allow.
void formatSummaryTime(bool recorded, uint32_t durationMs, char* out,
                       size_t size) {
  if (!recorded) {
    std::snprintf(out, size, "%s", "-");
    return;
  }
  const uint32_t tenths = (durationMs + 50) / 100;
  std::snprintf(out, size, "%lu.%lu", static_cast<unsigned long>(tenths / 10),
                static_cast<unsigned long>(tenths % 10));
}

// Builds a widest-value reference for one summary table. Replacing every digit
// with 8 makes the reference wide enough for any duration with the same number
// of digits, while keeping all value cells at one readable size. Recorded
// values always have one decimal place, so length identifies the digit count.
void summaryValueSizeRef(const BackflushCycle& cycle, char* out, size_t size) {
  std::snprintf(out, size, "-");
  const auto& results = cycle.results();
  const auto consider = [&](bool recorded, uint32_t durationMs) {
    char value[12];
    formatSummaryTime(recorded, durationMs, value, sizeof(value));
    if (std::strlen(value) > std::strlen(out))
      std::snprintf(out, size, "%s", value);
  };

  for (size_t i = 0; i < BackflushCycle::CYCLE_COUNT; ++i) {
    consider(cycle.onRecorded(i), results.onMs[i]);
    consider(cycle.offRecorded(i), results.offMs[i]);
  }
  for (char* p = out; *p; ++p) {
    if (*p >= '0' && *p <= '9') *p = '8';
  }
}

void drawTableCell(LGFX_Sprite* c, layout::rect box, const char* text,
                   uint32_t color, uint32_t bg, const char* sizeRef = nullptr) {
  c->fillRect(box.x, box.y, box.w, box.h, bg);
  ui::drawFittedText(c, text, layout::inset(box, 2), color, bg,
                     {.sizeRef = sizeRef});
  c->drawFastHLine(box.x, box.y + box.h - 1, box.w, theme::muted());
  c->drawFastVLine(box.x + box.w - 1, box.y, box.h, theme::muted());
}

}  // namespace

void BackflushScreen::startSession() {
  stopSensor();
  // A restart can land inside the previous session's completion tone.
  sounds::backflushSilence();
  _cycle.reset();
  _pumpDetected = false;
  _hasSensorFrame = false;
  _sessionEnd = SessionEnd::None;
  _lastInstruction = BackflushCycle::Instruction::StartPump;
  _instructionAnnounced = false;
  _lastCountdownSecond = 0;
  _drawnState = {};
  _sensorRunning = _sensor.begin();
  if (_sensorRunning) {
    _sensorSequence = _sensor.seq();
    _lastProgressMs = millis();
    power::powerManager.keepAwake();
  }
}

void BackflushScreen::endSession(SessionEnd reason) {
  stopSensor();
  sounds::backflushSilence();
  _sessionEnd = reason;
  _resultsShownMs = millis();
  requestDraw();
}

void BackflushScreen::stopSensor() {
  if (!_sensorRunning) return;
  _sensor.end();
  _sensorRunning = false;
}

void BackflushScreen::onEnter() { startSession(); }

void BackflushScreen::onExit() {
  stopSensor();
  sounds::backflushSilence();
}

ScreenResult BackflushScreen::onEvent(button::Gesture event) {
  if (event == button::Gesture::B_SHORT) return exit();
  // Holding A starts the routine over from wherever it is, so a mistimed pump
  // start does not have to sit in the summary for the rest of the run.
  if (event == button::Gesture::A_LONG) {
    startSession();
    requestDraw();
    return stay();
  }
  // Tapping A restarts only while no duration has been measured, so a summary
  // is never cleared by a stray press. Discarding one takes the hold above.
  if (event == button::Gesture::A_SHORT && !_sensorRunning && !hasResults()) {
    startSession();
    requestDraw();
    return stay();
  }
  return ignored();
}

// Sounds the cues for the routine: one tone when the operator is asked to
// start or stop the pump, a longer one when the run finishes, and a countdown
// over the last three seconds of a timed period.
//
// Nothing sounds while a detected pump change is being confirmed, since the
// operator may already have done what the screen is about to ask for. If the
// change turns out to be spurious, the command sounds on the next tick.
void BackflushScreen::updateAudio(uint32_t nowMs) {
  const BackflushCycle::Instruction instruction = _cycle.instruction(nowMs);
  if (_cycle.confirmingPumpChange()) return;

  if (!_instructionAnnounced || instruction != _lastInstruction) {
    _lastCountdownSecond = 0;
    if (instruction == BackflushCycle::Instruction::StartPump)
      sounds::backflushStartPump();
    else if (instruction == BackflushCycle::Instruction::StopPump)
      sounds::backflushStopPump();
    else if (instruction == BackflushCycle::Instruction::Complete)
      sounds::backflushComplete();
    _lastInstruction = instruction;
    _instructionAnnounced = true;
  }

  if (instruction != BackflushCycle::Instruction::PumpOn &&
      instruction != BackflushCycle::Instruction::Wait)
    return;

  const uint32_t remainingMs = _cycle.phaseRemainingMs(nowMs);
  const uint8_t remainingSecond =
      static_cast<uint8_t>((remainingMs + 999) / 1000);
  if (remainingSecond == 0 || remainingSecond > 3 ||
      remainingSecond == _lastCountdownSecond)
    return;
  _lastCountdownSecond = remainingSecond;
  sounds::backflushCountdown(sounds::countdownBeepFreq(remainingSecond));
}

BackflushScreen::DisplayState BackflushScreen::displayState(
    uint32_t nowMs) const {
  DisplayState state;
  state.phase = _cycle.displayPhase();
  state.instruction = _cycle.displayInstruction(nowMs);
  // Tenths are the finest thing the timer shows, so anything smaller would
  // repaint with no visible difference. An untimed period reports zero.
  state.elapsedTenths = _cycle.displayPhaseElapsedMs(nowMs) / 100;
  state.cycleIndex = static_cast<uint8_t>(_cycle.displayCycleIndex());
  state.hasSensorFrame = _hasSensorFrame;
  return state;
}

// Carries the session forward one step: take the newest pump state, end the
// session if the sensor or the operator has gone quiet, hand the state to the
// cycle, and repaint only if the screen would look different.
ScreenResult BackflushScreen::tick() {
  const uint32_t nowMs = millis();
  if (!_sensorRunning) {
    if (hasResults() && nowMs - _resultsShownMs < kSummaryAwakeMs)
      power::powerManager.keepAwake();
    return stay();
  }

  if (_sensor.seq() != _sensorSequence) {
    const VibrationSensor::TriggerState state = _sensor.triggerState();
    _sensorSequence = state.seq;
    _pumpDetected = state.triggered;
    _hasSensorFrame = true;
  } else if (nowMs - _sensor.lastAnalysisProgressMs() >= kSensorStaleMs) {
    // The last pump state is stale, and timing periods against it would invent
    // measurements.
    endSession(SessionEnd::SensorStalled);
    return stay();
  }

  if (nowMs - _lastProgressMs >= kInactivityMs) {
    endSession(SessionEnd::Inactivity);
    return stay();
  }

  power::powerManager.keepAwake();

  if (_hasSensorFrame) {
    if (_cycle.update(nowMs, _pumpDetected)) _lastProgressMs = nowMs;
    updateAudio(nowMs);
    if (_cycle.complete()) {
      stopSensor();
      _resultsShownMs = nowMs;
      requestDraw();
      return stay();
    }
  }

  const DisplayState state = displayState(nowMs);
  if (state != _drawnState) {
    _drawnState = state;
    requestDraw();
  }
  return stay();
}

// Draws the view the operator works from: cycle, instruction, timer, and a bar
// filling toward the target. Both orientations keep that reading order;
// portrait splits two-word instructions over two lines so they can be larger.
void BackflushScreen::drawActive(LGFX_Sprite* c, int headerH, uint32_t nowMs) {
  layout::rect body =
      layout::inset({0, headerH, c->width(), c->height() - headerH}, kBodyPad);
  const bool portrait = body.h > body.w;
  const BackflushCycle::Phase phase = _cycle.displayPhase();
  const BackflushCycle::Instruction instruction =
      _cycle.displayInstruction(nowMs);
  const bool timed = phase == BackflushCycle::Phase::PumpOn ||
                     phase == BackflushCycle::Phase::PumpOff;

  const int progressH = portrait ? 10 : 9;
  const int outerGap = portrait ? 5 : 4;
  const auto barSplit = layout::splitVFixedBottom(body, progressH, outerGap);
  const int cycleH = portrait ? 46 : 20;
  const int contentGap = portrait ? 4 : 2;
  const auto cycleSplit =
      layout::splitVFixed(barSplit.first, cycleH, contentGap);
  const auto mainSplit =
      layout::splitV(cycleSplit.second, portrait ? 0.48f : 0.43f, contentGap);
  const layout::rect cycleBox = cycleSplit.first;
  const layout::rect instructionBox = mainSplit.first;
  const layout::rect timerBox = mainSplit.second;
  const layout::rect progressBox = barSplit.second;

  drawCycleIndicator(c, cycleBox, _cycle.displayCycleIndex(), portrait);
  drawHeadline(c, instruction, instructionBox, portrait);
  if (timed) {
    char timer[20];
    formatPhaseTime(_cycle.displayPhaseElapsedMs(nowMs), timer, sizeof(timer));
    c->setTextColor(theme::fg(), theme::bg());
    gauge::drawCentered(c, timer, "s", timerBox, {.sizeRef = "18.8"});
  }

  // The bar stops at the target, so a period the operator runs long stays full
  // rather than overflowing.
  c->fillRect(progressBox.x, progressBox.y, progressBox.w, progressBox.h,
              theme::surface());
  const uint32_t elapsed = std::min(_cycle.displayPhaseElapsedMs(nowMs),
                                    BackflushCycle::PHASE_TARGET_MS);
  const int fillW =
      static_cast<int>((static_cast<uint64_t>(progressBox.w) * elapsed) /
                       BackflushCycle::PHASE_TARGET_MS);
  const uint32_t fillColor = phase == BackflushCycle::Phase::PumpOn
                                 ? theme::accent()
                                 : theme::accent_light();
  if (fillW > 0)
    c->fillRect(progressBox.x, progressBox.y, fillW, progressBox.h, fillColor);
  c->drawRect(progressBox.x, progressBox.y, progressBox.w, progressBox.h,
              theme::muted());
}

// The tall form of the summary: one row per cycle, with the ON and OFF
// durations side by side under a heading row. The last row takes whatever
// height integer division left over, so the table meets the bottom edge.
void BackflushScreen::drawPortraitSummary(LGFX_Sprite* c, layout::rect body,
                                          const char* valueSizeRef) {
  constexpr int rows = static_cast<int>(BackflushCycle::CYCLE_COUNT) + 1;
  const int rowH = body.h / rows;
  const int cycleW = body.w / 4;
  const int valueW = (body.w - cycleW) / 2;
  drawTableCell(c, {body.x, body.y, cycleW, rowH}, "#", theme::dim(),
                theme::bg_alt(), kHeadingSizeRef);
  drawTableCell(c, {body.x + cycleW, body.y, valueW, rowH}, "ON", theme::dim(),
                theme::bg_alt(), kHeadingSizeRef);
  drawTableCell(
      c, {body.x + cycleW + valueW, body.y, body.w - cycleW - valueW, rowH},
      "OFF", theme::dim(), theme::bg_alt(), kHeadingSizeRef);

  const auto& results = _cycle.results();
  for (size_t i = 0; i < BackflushCycle::CYCLE_COUNT; ++i) {
    const int y = body.y + static_cast<int>(i + 1) * rowH;
    const int h =
        i + 1 == BackflushCycle::CYCLE_COUNT ? body.y + body.h - y : rowH;
    char cycle[4];
    char on[12];
    char off[12];
    const bool onRecorded = _cycle.onRecorded(i);
    const bool offRecorded = _cycle.offRecorded(i);
    std::snprintf(cycle, sizeof(cycle), "%u", static_cast<unsigned>(i + 1));
    formatSummaryTime(onRecorded, results.onMs[i], on, sizeof(on));
    formatSummaryTime(offRecorded, results.offMs[i], off, sizeof(off));
    drawTableCell(c, {body.x, y, cycleW, h}, cycle, theme::dim(), theme::bg());
    drawTableCell(c, {body.x + cycleW, y, valueW, h}, on,
                  onRecorded ? theme::fg() : theme::dim(), theme::bg(),
                  valueSizeRef);
    drawTableCell(c, {body.x + cycleW + valueW, y, body.w - cycleW - valueW, h},
                  off, offRecorded ? theme::fg() : theme::dim(), theme::bg(),
                  valueSizeRef);
  }
}

// The wide form of the same table, transposed: cycles run across as columns
// with a row each for ON and OFF, which fits a screen that is short but wide.
void BackflushScreen::drawLandscapeSummary(LGFX_Sprite* c, layout::rect body,
                                           const char* valueSizeRef) {
  constexpr int columns = static_cast<int>(BackflushCycle::CYCLE_COUNT) + 1;
  const int rowH = body.h / 3;
  const int columnW = body.w / columns;
  // The last column absorbs what integer division left over, so headings and
  // values have to work out their geometry the same way to stay in line.
  const auto cell = [&](size_t i, int y, int h) {
    const int x = body.x + static_cast<int>(i + 1) * columnW;
    const int w =
        i + 1 == BackflushCycle::CYCLE_COUNT ? body.x + body.w - x : columnW;
    return layout::rect{x, y, w, h};
  };

  drawTableCell(c, {body.x, body.y, columnW, rowH}, "#", theme::dim(),
                theme::bg_alt(), kHeadingSizeRef);
  for (size_t i = 0; i < BackflushCycle::CYCLE_COUNT; ++i) {
    char cycle[4];
    std::snprintf(cycle, sizeof(cycle), "%u", static_cast<unsigned>(i + 1));
    drawTableCell(c, cell(i, body.y, rowH), cycle, theme::dim(),
                  theme::bg_alt(), kHeadingSizeRef);
  }

  const auto& results = _cycle.results();
  const char* labels[] = {"ON", "OFF"};
  for (int row = 0; row < 2; ++row) {
    const int y = body.y + (row + 1) * rowH;
    const int h = row == 1 ? body.y + body.h - y : rowH;
    drawTableCell(c, {body.x, y, columnW, h}, labels[row], theme::dim(),
                  theme::bg_alt(), kHeadingSizeRef);
    for (size_t i = 0; i < BackflushCycle::CYCLE_COUNT; ++i) {
      const uint32_t duration = row == 0 ? results.onMs[i] : results.offMs[i];
      const bool recorded =
          row == 0 ? _cycle.onRecorded(i) : _cycle.offRecorded(i);
      char value[12];
      formatSummaryTime(recorded, duration, value, sizeof(value));
      drawTableCell(c, cell(i, y, h), value,
                    recorded ? theme::fg() : theme::dim(), theme::bg(),
                    valueSizeRef);
    }
  }
}

// Draws the durations with a line above them naming how the run ended. Portrait
// has no room for a header annotation, so that line carries the outcome in both
// orientations.
void BackflushScreen::drawSummary(LGFX_Sprite* c, int headerH, const char* note,
                                  uint32_t noteColor) {
  const bool landscape = c->width() > c->height();
  const auto split = layout::splitVFixed(
      layout::inset({0, headerH, c->width(), c->height() - headerH}, kBodyPad),
      kNoteH, 2);
  ui::drawFittedText(c, note, split.first, noteColor, theme::bg());
  const layout::rect body = split.second;
  char valueSizeRef[12];
  summaryValueSizeRef(_cycle, valueSizeRef, sizeof(valueSizeRef));
  if (landscape)
    drawLandscapeSummary(c, body, valueSizeRef);
  else
    drawPortraitSummary(c, body, valueSizeRef);
}

// Chooses which of the screen's states to draw: the durations from a run that
// finished or was interrupted, an explanation if a session never measured one,
// or the active view once the sensor has produced its first result.
bool BackflushScreen::onDraw(LGFX_Sprite* c) {
  c->fillScreen(theme::bg());
  const bool ended = _sessionEnd != SessionEnd::None;
  const int headerH = ui::drawViewHeader(c, "BACKFLUSH", theme::accent());

  if (_cycle.complete()) {
    drawSummary(c, headerH, "COMPLETE", theme::ok());
    return true;
  }
  if (ended) {
    if (hasResults()) {
      drawSummary(c, headerH,
                  _sessionEnd == SessionEnd::SensorStalled
                      ? "ENDED: SENSOR FAULT"
                      : "ENDED: NO ACTIVITY",
                  theme::warn());
      return true;
    }
    ui::drawCardBody(
        c, headerH,
        _sessionEnd == SessionEnd::SensorStalled
            ? "Pump sensor stopped responding. Tap A to restart."
            : "No pump activity for five minutes. Tap A to restart.");
    return true;
  }
  if (!_sensorRunning) {
    ui::drawCardBody(c, headerH, "Pump sensor unavailable. Tap A to retry.");
    return true;
  }
  if (!_hasSensorFrame) {
    ui::drawCardBody(c, headerH, "Starting pump sensor...");
    return true;
  }
  drawActive(c, headerH, millis());
  return true;
}

ButtonHints BackflushScreen::buttonHints() const {
  ButtonHints hints{};
  if (_sensorRunning || hasResults()) {
    // There is something to lose, so starting over is offered as a hold.
    hints.a.hold = {HintGlyph::None, "AGAIN"};
  } else {
    hints.a.tap = {HintGlyph::None,
                   _sessionEnd == SessionEnd::None ? "RETRY" : "AGAIN"};
  }
  hints.b.tap = {HintGlyph::Back};
  return hints;
}
