// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>

#include "TargetStore.h"
#include "apps/extraction/ExtractionController.h"
#include "apps/extraction/RobustFlow.h"
#include "apps/extraction/ScaleReadingTimingObserver.h"
#include "apps/extraction/ShotReplayer.h"
#include "apps/extraction/TargetAlert.h"
#include "ble/BleScaleService.h"
#include "ui/Screen.h"
#include "ui/button.h"
#include "ui/chrome/BatteryIndicator.h"
#include "ui/chrome/BleIcon.h"
#include "ui/layout.h"
#include "ui/sprite.h"
#include "util/storage.h"
#include "vibration/VibrationSensor.h"

class HttpServer;

// ExtractionScreen: main view of the  espresso extraction recorder.
//
// This is the view. The recording authority (the recorder, persistence,
// cross-task synchronization, and the HTTP/SSE read paths) lives in
// ExtractionController.
// Each tick, ExtractionScreen:
//   1. reads its inputs: the vibration-window trigger (pump on/off, a level)
//      and the BLE scale snapshot;
//   2. calls _controller.tick(...), which advances the recorder under its mutex
//      and returns a TickOutcome describing what changed;
//   3. reacts on the UI side: refocus the screen when the controller reports
//      meaningful pour evidence, run the target-weight alert, request a redraw,
//      and keep the device awake.
// Button commands (tare / clear) are forwarded to the controller via
// contextAction(); session start/stop (onEnter/onExit) start and stop the
// vibration sensor, request the scale service, and call the controller's
// session methods. Pump starts are reported as generic activity to the power
// manager; active extraction keeps the display awake.
//
// Everything here runs on the main UI loop. ExtractionScreen holds no mutex;
// the controller owns all cross-task synchronization. drawLayout() and the
// other readers run on the same task that calls tick(), so they read the
// controller's main-task-safe accessors (current(), effectiveLastShot(), ...)
// without locking.
//
// =============================================================================
//   View modes (the Screen's own state machine)
// =============================================================================
//
//   Live      — the in-flight shot: the live weight gauge. Meaningful pour
//               evidence snaps focus here.
//   LastShot  — the controller-selected display shot summary.
//   LastShotChart — the controller-selected display shot chart.
//
//   A-tap cycles Live -> LastShot -> LastShotChart -> Live. A-hold is the
//   contextual recorder action for the current phase (IDLE: tare; DONE: clear).
//
// =============================================================================
//   Target-weight stop alert (UI-only)
// =============================================================================
//
//   The target-alert predictor lives in the engine (ExtractionController) and
//   emits its decision on TickOutcome.alertState; ExtractionScreen drives the
//   output (countdown beep cadence and sustained cut tone) from that state.
class ExtractionScreen : public Screen {
 public:
  ExtractionScreen(pump_scale::ExtractionController& controller,
                   TargetStore& targetStore, VibrationSensor& sensor)
      : _sensor(sensor), _controller(controller), _targetStore(targetStore) {}

  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  void onLayoutChanged() override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
#if PB_SCALE_READING_TIMING
  void onPresented() override;
#endif
  uint32_t desiredTickMs() const override { return 33; }  // ~30 fps for curve
  ButtonHints buttonHints() const override;
  bool wantsStatusBar() const override;

  bool startVibrationSensor();
  void allocateRenderingBuffers();

  // Sets the live title for a specialized firmware. The caller owns the text,
  // which must outlive the screen.
  void setLiveViewTitle(const char* title) { _liveViewTitle = title; }

 private:
  VibrationSensor& _sensor;
  bool _sensorRunning = false;

  // The headless coordination engine that owns the recorder, cross-task mutex,
  // shot graduation/persistence, shot store and the HTTP/SSE read paths.
  // ExtractionScreen gathers inputs, drives the controller once per tick, and
  // renders the result.
  pump_scale::ExtractionController& _controller;
  uint32_t _lastSensorSeq = 0;
  bool _lastTriggered = false;
  VibrationSensor::Data _lastSensorData = {};
  // Cached bleScale snapshot from the most recent tick. drawLayout() reads
  // it without re-snapshotting, so the UI sees the same state/weight/battery
  // that tick() used for its decisions.
  BleScaleService::Snapshot _lastScaleSnap;
#if PB_SCALE_READING_TIMING
  uint32_t _drawnWeightSequence = 0;
#endif

  // Last observed scale state, used to detect a scale-state change worth a
  // frame (passed to the controller, which wakes SSE readers and reports
  // frameNeeded). Main-task-only; no atomic needed.
  BleScaleService::State _lastScaleState = BleScaleService::State::OFF;

  // Which shot the screen is presenting. Pure UI state, owned and mutated
  // only by the main task (onEvent cycles it, tick auto-reverts to Live when
  // the controller reports meaningful pour evidence). Independent of the
  // recorder phase: Live shows current() with the live scale gauge; LastShot
  // shows the controller-selected display shot summary; LastShotChart shows
  // its chart.
  enum class ViewMode : uint8_t { Live, LastShot, LastShotChart, Replay };
  ViewMode _viewMode = ViewMode::Live;

  // ---- Shot replay (see ShotReplayer) -----------------------
  // Replay drives the controller's existing recorder via replayTick()
  // replayTick suppresses graduation/persist/SSE, and resetInFlightAndNotify()
  // brackets the session, so history is untouched.
  pump_scale::ShotReplayer _replayer;
  pump_scale::ScaleSnapshot _replaySnap = {};  // last synthesized scale frame
  // During replay, we can switch between flow band and target alert display.
  bool _replayShowFlowBand = false;

  TargetStore& _targetStore;
  const char* _liveViewTitle = "LIVE";

  // Latest predictor state plus output bookkeeping. The previous alert level
  // seeds the countdown cadence and silences the tone once; its flow estimate
  // also drives the inactive target band's live meter and replay's optional
  // flow-band view.
  pump_scale::TargetAlert::State _alertState;
  uint32_t _lastBeepMs = 0;        // last countdown/cut-tone emission
  bool _cutAudioSilenced = false;  // A_SHORT during cut state hushes this shot
  int _lastCountdownBucket = 0;    // 1..5, or 0 when not counting down

  // BLE icon used by the no-scale weight gauge.
  BleIcon _bleIcon{/*scale=*/2};
  // Reusable badge variants let the no-target flow meter composite its unit at
  // the exact fill boundary.
  ui::PersistentSprite _flowUnitUnfilled;
  ui::PersistentSprite _flowUnitFilled;
  // Battery gauge shown in the title bar of the live/last-shot views.
  BatteryIndicator _batteryIndicator;
  storage::MountState _storageState = storage::MountState::Unavailable;
  bool _latestAcceptedShotSaved = true;

  BleStatus bleStatus() const;
  // Push the store's current config into the engine so the predictor uses it.
  void applyTargetCoeffs();
  // Map the alert level to the countdown/cut audio and the blue cut-state
  // overlay. Main task only.
  void driveAlert(const pump_scale::TargetAlert::State& st, uint32_t nowMs);

  // Everything drawLayout needs, resolved once per frame from (_viewMode,
  // recorder phase, _lastScaleSnap). Collapses the live/last duality into a
  // single descriptor so drawLayout carries no phase conditionals.
  struct ScreenModel {
    // The UI groups recorder phases by what should be drawn. pumpDetected
    // distinguishes RUNNING from POST_PUMP within a continuing pour.
    enum class LiveState : uint8_t { Ready, Pouring, FinishedRealShot };
    LiveState liveState = LiveState::Ready;

    ViewMode viewMode;
    const pump_scale::Extraction* chart = nullptr;
    float gaugeGrams;
    bool gaugeValid;
    bool gaugeActive;  // self-tared yield is available (meaningful yield has
                       // been seen); also controls the raw-scale secondary
                       // readout.
    // Secondary raw scale reading, shown small under the headline yield only
    // when it diverges from our self-tared value (operator left the cup weight
    // on the scale). Suppressed otherwise so the two numbers don't duplicate.
    float scaleGrams;
    bool showScaleSecondary;
    // Draw the scrolling live trace only while actively pouring,
    // so flushes/grinder doses/failed shots are never painted.
    bool showLiveSamples;
    uint32_t timerMs;
    // True only while vibration detection keeps the recorder in RUNNING.
    bool pumpDetected = false;
    bool empty;              // LastShot view with no shot recorded yet
    bool cutState = false;   // true when the predicted cutoff point is reached
    int16_t flowCgPerS = 0;  // smoothed live flow for the inactive target band
    bool flowValid = false;

    // Idle Live summary panel (ready / after a shot finishes). Uses the
    // controller-selected display shot when one exists.
    const pump_scale::Extraction* summaryShot = nullptr;
    pump_scale::FlowSeriesStats summaryFlow;
  };
  ScreenModel modelFor() const;
  // Fill the gauge/chart fields of an evolving extraction (Live or Replay).
  // Parameters carry only what differs between the two callers: the scale
  // frame (`haveYield`/`yieldCg`, `rawGrams`/`haveRawWeight` — live BLE vs
  // the replay's synthesized frame) and `nowMs`, the clock the elapsed timer
  // is measured against (millis() live, the virtual replay clock in replay).
  void fillLiveModel(ScreenModel& m, bool haveYield, int16_t yieldCg,
                     float rawGrams, bool haveRawWeight, uint32_t nowMs) const;
  void fillLastShotModel(ScreenModel& m) const;
  // Target config resolved for the current view: replay uses the shot's
  // recorded coeffs, every other view uses the live store.
  uint16_t effectiveTargetCg(const ScreenModel& m) const;
  bool effectiveArmed(const ScreenModel& m) const;

  // Replay control + per-tick advance. startReplay uses the controller-selected
  // display shot.
  void startReplay();
  void stopReplay();
  void tickReplay();
  ScreenResult onEventReplay(button::Gesture event);

  void drawLayout(LGFX_Sprite* c);
  void drawStorageWarning(LGFX_Sprite* c) const;
  int drawHeader(LGFX_Sprite* c, const char* title, uint32_t titleColor,
                 uint32_t bg) const;
  void drawLiveView(LGFX_Sprite* c, const ScreenModel& m);
  void drawLiveBody(LGFX_Sprite* c, const ScreenModel& m, layout::rect body,
                    bool dimTimer, int gaugeMargin = 0);
  void drawLastShotView(LGFX_Sprite* c, const ScreenModel& m) const;
  void drawLastShotChartView(LGFX_Sprite* c, const ScreenModel& m,
                             uint32_t chartNow) const;
  void drawLastShot(LGFX_Sprite* c, const ScreenModel& m,
                    layout::rect body) const;
  // Draw the last-shot metrics into `box`. By default they are stacked
  // vertically with the yield as the headline; pass `horizontalMetrics=true`
  // to place the yield beside the secondary readouts. Duration, sustained peak
  // flow, and an optional end cause sit in a smaller, dimmed secondary style.
  // `showTitle` adds a "LAST" caption when the panel is not already under a
  // "LAST SHOT" header.
  void drawSummaryPanel(LGFX_Sprite* c, const ScreenModel& m,
                        layout::rect box) const;
  void drawTargetBand(LGFX_Sprite* c, const ScreenModel& m, uint16_t targetCg,
                      bool armed, bool forceFlow, layout::rect box);
  void drawTimerCell(LGFX_Sprite* c, uint32_t timerMs, layout::rect box,
                     uint32_t color) const;
  void drawWeightGauge(LGFX_Sprite* c, const ScreenModel& m,
                       layout::rect box) const;
};
