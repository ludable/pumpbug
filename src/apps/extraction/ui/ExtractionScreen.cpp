// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ExtractionScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ExtractionView.h"
#include "apps/extraction/ScaleReadingTimingObserver.h"
#include "apps/extraction/TargetAlert.h"
#include "apps/extraction/scale_snapshot_util.h"
#include "diagnostics/RuntimeEventLog.h"
#include "power/PowerManager.h"
#include "ui/blocks.h"
#include "ui/button.h"
#include "ui/chrome/BleIcon.h"
#include "ui/font_glyphs.h"
#include "ui/fonts.h"
#include "ui/gauge.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/segfont/segfont.h"
#include "ui/segfont/segfont_ifont.h"
#include "ui/sounds.h"
#include "ui/theme.h"
#include "util/time_format.h"
#include "util/wallclock.h"

using pump_scale::TargetAlert;

namespace {
// ---- Target-alert output tuning -------------------------------------------
// Wall-clock cadence for countdown beeps: each beep is scheduled within this
// band of the previous one, so prediction jitter does not cause stuttering or
// uneven spacing. The cut tone is exempt and fires immediately at cutoff.
constexpr uint32_t kCountdownBeepMinMs = 900;
constexpr uint32_t kCountdownBeepMaxMs = 1100;

// The battery icon's width is used as the lateral margin for the live weight
// gauge in portrait so the gauge sits centered between the title and battery.
constexpr int kBatteryIconW = 24;

// Transition callbacks run on the FIFO task. Keep this path bounded and
// task-safe; storage and network work must remain on their owning tasks.
void recordPumpTransition(
    const VibrationSensor::DetectionTransition& transition) {
  diagnostics::PumpDetectionEventKind kind;
  switch (transition.event) {
    case VibrationWindowTrigger::Event::Opened:
      kind = diagnostics::PumpDetectionEventKind::On;
      break;
    case VibrationWindowTrigger::Event::Closed:
      kind = diagnostics::PumpDetectionEventKind::Off;
      break;
    default:
      return;
  }
  runtimeEventLog.pushPumpDetection(
      {transition.ms, wallclock::utcNow(), transition.detectedForMs,
       transition.rawSnrDb, transition.smoothedSnrDb, transition.peakHz,
       transition.dominantPeakHz, transition.spectralFlux,
       transition.closeFailureMask, transition.stationary, kind});
}

}  // namespace

namespace {
auto segmentFontProportional = segfont::IFontAdapter(
    segfont::style_for_height(48, segfont::Weight::Regular),
    segfont::TextOptions{segfont::SpacingMode::Proportional});
const char* kNoScaleText = "no SCALE";
constexpr font::sizing_spec_t kNoScaleFont = &segmentFontProportional;

void formatShotWhen(uint32_t startUtcSec, char* out, size_t n) {
  if (startUtcSec == 0) {
    std::snprintf(out, n, "time unknown");
    return;
  }

  const uint32_t now = wallclock::utcNow();
  if (now != 0 && now >= startUtcSec) {
    const uint32_t delta = now - startUtcSec;
    if (delta < 60) {
      std::snprintf(out, n, "just now");
      return;
    }
    if (delta < 60 * 60) {
      std::snprintf(out, n, "%um ago", static_cast<unsigned>(delta / 60));
      return;
    }
    if (delta < 24 * 60 * 60) {
      std::snprintf(out, n, "%uh ago",
                    static_cast<unsigned>(delta / (60 * 60)));
      return;
    }
    if (delta < 7 * 24 * 60 * 60) {
      std::snprintf(out, n, "%ud ago",
                    static_cast<unsigned>(delta / (24 * 60 * 60)));
      return;
    }
  }

  timefmt::formatDateTime(startUtcSec, out, n);
}

// Size reference for the portrait last-shot labels, where TARGET is the
// widest label in the stacked grid (see ui::drawLabeledValue).
constexpr const char* kStackedDetailLabelRef = "TARGET";
// Stable product reference for the target band. The unit badge follows the
// band and icon geometry rather than the selected value font; values that
// exceed the reference width shrink defensively.
constexpr const char* kTargetValueSizeRef = "88.8";
constexpr const char* kFlowUnit = "G/S";
constexpr int kBandIconMax = 18;
constexpr int kPortraitUnitExtraHeight = 2;
constexpr int kMaxBandUnitHeight = kBandIconMax + kPortraitUnitExtraHeight;

void drawRule(LGFX_Sprite* c, layout::rect box) {
  c->fillRect(box.x, box.y, box.w, box.h, theme::muted());
}

struct TargetValueLayout {
  font::sized_font_t font;
  gauge::UnitBoxLayout unitLayout;
  layout::rect iconBox;
  int unitX = 0;
  int unitY = 0;
  int valueX = 0;
  int valueCenterY = 0;
};

struct BandValueStyle {
  int iconSize = 0;
  int unitHeight = 0;
  int valueHeightBudget = 0;
};

BandValueStyle bandValueStyle(LGFX_Sprite* c, layout::rect box) {
  const bool portrait = c->height() > c->width();
  const int iconSize = std::max(3, std::min(box.h, kBandIconMax));
  // Portrait aligns the unit with the 20 px visible figure cap below; the
  // tighter landscape band aligns it with the 18 px icon.
  const int unitHeight = std::max(
      1, std::min(box.h, iconSize + (portrait ? kPortraitUnitExtraHeight : 0)));
  // Montserrat 28 has 20 px visible lining figures, two pixels taller than the
  // icon. font::fit compares full line heights, so use that font's line metric
  // rather than the visible-height target directly.
  const int portraitValueHeight =
      font::metrics(&lgfx::fonts::lv_font_montserrat_28).height;
  const int valueHeightBudget =
      portrait ? std::min(box.h, portraitValueHeight) : box.h;
  return {iconSize, unitHeight, valueHeightBudget};
}

struct FittedBandValue {
  font::sized_font_t font;
  int width = 0;
};

FittedBandValue fitBandValue(LGFX_Sprite* c, const char* value, int valueBudget,
                             int valueHeightBudget) {
  font::sized_font_t valueFont =
      font::fit(c, kTargetValueSizeRef, valueBudget, valueHeightBudget,
                font::textFamily());
  int valueW = c->textWidth(value);
  if (valueW > valueBudget) {
    valueFont =
        font::fit(c, value, valueBudget, valueHeightBudget, font::textFamily());
    valueW = c->textWidth(value);
  }
  return {valueFont, valueW};
}

TargetValueLayout layoutTargetValue(LGFX_Sprite* c, const char* value,
                                    layout::rect box) {
  constexpr int iconGap = 3;
  const BandValueStyle style = bandValueStyle(c, box);
  const int leadingW = style.iconSize + iconGap;
  const gauge::UnitBoxLayout unitLayout =
      gauge::layoutUnitBoxWithinHeight("g", style.unitHeight);
  const int unitGap = std::max(1, unitLayout.height / 8);
  const int valueBudget =
      std::max(1, box.w - leadingW - unitGap - unitLayout.width);
  const FittedBandValue fitted =
      fitBandValue(c, value, valueBudget, style.valueHeightBudget);

  const int valueCenterY = box.y + box.h / 2;
  const int groupW = leadingW + fitted.width + unitGap + unitLayout.width;
  const int iconX = box.x + std::max(0, box.w - groupW) / 2;
  const int valueX = iconX + style.iconSize + iconGap;

  return {
      fitted.font,
      unitLayout,
      {iconX, valueCenterY - style.iconSize / 2, style.iconSize,
       style.iconSize},
      valueX + fitted.width + unitGap,
      valueCenterY - unitLayout.height / 2,
      valueX,
      valueCenterY,
  };
}

void drawTargetValue(LGFX_Sprite* c, const TargetValueLayout& targetLayout,
                     const char* value, uint32_t iconColor, uint32_t textColor,
                     uint32_t backgroundColor) {
  font::apply(c, targetLayout.font);
  ui::drawTargetIcon(c, targetLayout.iconBox, iconColor);
  c->setTextColor(textColor, backgroundColor);
  layout::drawMiddleLeft(c, value, targetLayout.valueX,
                         targetLayout.valueCenterY);
  gauge::drawUnitBox(c, "g", targetLayout.unitLayout, targetLayout.unitX,
                     targetLayout.unitY, textColor, backgroundColor);
}

struct FlowValueLayout {
  font::sized_font_t font;
  gauge::UnitBoxLayout unitLayout;
  int valueRightX = 0;
  int valueCenterY = 0;
  int unitX = 0;
  int unitY = 0;
};

FlowValueLayout layoutFlowValue(LGFX_Sprite* c, const char* value,
                                layout::rect box) {
  // The caller's inset plus this margin keeps the badge four pixels from the
  // band edge.
  constexpr int rightMargin = 2;
  // Reuse the target band's icon-derived style even though flow has no icon,
  // so switching between target and flow does not resize the value or badge.
  const BandValueStyle style = bandValueStyle(c, box);
  const gauge::UnitBoxLayout unitLayout =
      gauge::layoutUnitBoxWithinHeight(kFlowUnit, style.unitHeight);
  const int unitGap = std::max(1, unitLayout.height / 8);
  const int valueBudget =
      std::max(1, box.w - rightMargin - unitGap - unitLayout.width);
  const FittedBandValue fitted =
      fitBandValue(c, value, valueBudget, style.valueHeightBudget);
  const int rightX = box.x + box.w - rightMargin;
  const int unitX = rightX - unitLayout.width;
  const int centerY = box.y + box.h / 2;
  return {
      fitted.font, unitLayout, unitX - unitGap,
      centerY,     unitX,      centerY - unitLayout.height / 2,
  };
}

void drawFlowNumber(LGFX_Sprite* c, const FlowValueLayout& flowLayout,
                    const char* value, uint32_t textColor,
                    uint32_t backgroundColor) {
  font::apply(c, flowLayout.font);
  c->setTextColor(textColor, backgroundColor);
  layout::drawMiddleRight(c, value, flowLayout.valueRightX,
                          flowLayout.valueCenterY);
}

void drawFlowUnit(LGFX_Sprite* c, ui::PersistentSprite& unfilledBuffer,
                  ui::PersistentSprite& filledBuffer,
                  const FlowValueLayout& flowLayout, int fillRight) {
  const int width = flowLayout.unitLayout.width;
  const int height = flowLayout.unitLayout.height;
  if (!unfilledBuffer.allocated() || !filledBuffer.allocated()) {
    gauge::drawUnitBox(c, kFlowUnit, flowLayout.unitLayout, flowLayout.unitX,
                       flowLayout.unitY, theme::dim(), theme::surface());
    return;
  }

  LGFX_Sprite& unfilled = unfilledBuffer.shape(width, height);
  LGFX_Sprite& filled = filledBuffer.shape(width, height);
  unfilled.fillScreen(theme::surface());
  gauge::drawUnitBox(&unfilled, kFlowUnit, flowLayout.unitLayout, 0, 0,
                     theme::dim(), theme::surface());
  filled.fillScreen(theme::chart_flow());
  gauge::drawUnitBox(&filled, kFlowUnit, flowLayout.unitLayout, 0, 0,
                     theme::dim(), theme::chart_flow());

  unfilled.pushSprite(c, flowLayout.unitX, flowLayout.unitY);
  const int coveredWidth = std::clamp(fillRight - flowLayout.unitX, 0, width);
  if (coveredWidth == width) {
    filled.pushSprite(c, flowLayout.unitX, flowLayout.unitY);
    return;
  }
  if (coveredWidth == 0) return;

  const auto* pixels = static_cast<const uint16_t*>(filled.getBuffer());
  for (int y = 0; y < height; ++y) {
    c->pushImage(flowLayout.unitX, flowLayout.unitY + y, coveredWidth, 1,
                 pixels + y * width);
  }
}

}  // namespace

bool ExtractionScreen::startVibrationSensor() {
  if (_sensorRunning) return true;
  _sensorRunning = _sensor.begin(recordPumpTransition);
  return _sensorRunning;
}

void ExtractionScreen::onEnter() {
  power::powerManager.setForegroundFullPerformanceRequired(false);
  if (!startVibrationSensor()) M5_LOGE("VibrationSensor begin failed");
  bleScale.enable();
  _lastSensorSeq = 0;
  _lastTriggered = false;
  _lastSensorData = VibrationSensor::Data{};
  _lastScaleSnap = BleScaleService::Snapshot{};
  _storageState = storage::mountState();
  _viewMode = ViewMode::Live;
  // Load persisted target config and prime the live predictor. The alert state
  // resets per shot on the RUNNING edge; reset here too so stale alert state
  // from a prior session can't carry over.
  _targetStore.load();
  applyTargetCoeffs();
  _alertState = {};
  _cutAudioSilenced = false;
  _lastCountdownBucket = 0;
  _replayShowFlowBand = false;
  // Start the coordination session: clears in-flight recorder state, publishes
  // active=true (after the sensors above are up), and wakes SSE readers. Treat
  _controller.beginSession();
  _replayer.stop();

  _controller.requestRestoreLastShotIfEmpty();

  requestDraw();
}

void ExtractionScreen::onExit() {
  power::powerManager.setForegroundFullPerformanceRequired(false);
  // Mark inactive before disabling so the HTTP path stops trusting live scale
  // data before the service tears the link down.
  _controller.deactivate();
  _viewMode = ViewMode::Live;
  // Stop any in-progress replay; the recorder it drove is wiped by
  // resetInFlightAndNotify() at the end of this function.
  _replayer.stop();
  // Kill any lingering alert tone and STOP_NOW state so they can't outlive the
  // foreground.
  sounds::targetSilence();
  _alertState = {};
  _cutAudioSilenced = false;
  _lastCountdownBucket = 0;
  _replayShowFlowBand = false;
  if (_sensorRunning) {
    _sensor.end();
    _sensorRunning = false;
  }
  bleScale.disable();
  // Reset in-flight recorder state and wake SSE readers so /state and /current
  // stop reporting a stale RUNNING/POST_PUMP. /last is preserved so
  // accepted-shot history survives, per the controller's contract.
  _controller.resetInFlightAndNotify();
}

void ExtractionScreen::onLayoutChanged() { requestDraw(); }

bool ExtractionScreen::wantsStatusBar() const {
  // The shot UI deliberately keeps the full display in every mode. Its
  // buttonHints() map documents the gestures but is not presented by Chrome.
  return false;
}

BleStatus ExtractionScreen::bleStatus() const {
  switch (bleScale.snapshot().state) {
    case BleScaleService::State::OFF:
      return BleStatus::Off;
    case BleScaleService::State::READY:
      return BleStatus::Connected;
    case BleScaleService::State::RECONNECTING:
      // RECONNECTING actively scans and tries to re-pair; show the pulsing
      // searching beacon instead of a static disconnected slash.
      return BleStatus::Searching;
    default:
      return BleStatus::Searching;
  }
}

ButtonHints ExtractionScreen::buttonHints() const {
  // Reference map for this full-screen UI. ExtractionScreen keeps Chrome
  // hidden, so these hints are not currently presented on the device.
  ButtonHints h{};
  if (_viewMode == ViewMode::LastShot) {
    if (_controller.hasEffectiveLastShot()) {
      h.a.tap = {HintGlyph::None, "CHART"};
      h.a.hold = {HintGlyph::None, "REPLAY"};
    } else {
      h.a.tap = {HintGlyph::Back, "LIVE"};
    }
  } else if (_viewMode == ViewMode::LastShotChart) {
    h.a.tap = {HintGlyph::Back, "LIVE"};
    if (_controller.hasEffectiveLastShot()) {
      h.a.hold = {HintGlyph::None, "REPLAY"};
    }
  } else if (_viewMode == ViewMode::Replay) {
    h.a.tap = {HintGlyph::None, _replayer.paused() ? "RESUME" : "PAUSE"};
    h.a.hold = {HintGlyph::None, _replayShowFlowBand ? "TARGET" : "FLOW"};
    h.b.tap = {HintGlyph::Back, "LAST"};
  }
  return h;
}

void ExtractionScreen::allocateRenderingBuffers() {
  const gauge::UnitBoxLayout layout =
      gauge::layoutUnitBoxWithinHeight(kFlowUnit, kMaxBandUnitHeight);
  _flowUnitUnfilled.allocate(layout.width, layout.height);
  _flowUnitFilled.allocate(layout.width, layout.height);
}

ScreenResult ExtractionScreen::tick() {
  const uint32_t vs = _sensor.seq();
  if (vs != _lastSensorSeq) {
    _lastSensorSeq = _sensor.snapshot(_lastSensorData);
    // The trigger is a level. Short windows still produce records, and the
    // controller rejects records without enough pour evidence.
    const bool pumpStarted = _lastSensorData.triggered && !_lastTriggered;
    _lastTriggered = _lastSensorData.triggered;
    if (pumpStarted) power::powerManager.notifyActivity();
  }

  // One coherent read of {state, weight, battery, epoch} for this tick;
  // every downstream check is against the same snapshot.
  const BleScaleService::Snapshot prevScaleSnap = _lastScaleSnap;
  _lastScaleSnap = bleScale.snapshot();
  const bool newScaleSample =
      _lastScaleSnap.weight.sequence != prevScaleSnap.weight.sequence &&
      _lastScaleSnap.weight.timestampMs != 0;

  if (newScaleSample) {
    pump_scale::observeScaleReadingConsumed(_lastScaleSnap.weight.sequence,
                                            _lastScaleSnap.weight.timestampMs,
                                            millis());
  }

  const pump_scale::ScaleSnapshot snap =
      pump_scale::scaleSnapshotFromBle(_lastScaleSnap);

  // Detect a scale connect/flap for the frame decision, and update the tracker.
  // Main-task-only state.
  const bool scaleStateChanged = _lastScaleSnap.state != _lastScaleState;
  _lastScaleState = _lastScaleSnap.state;

  // Battery gauge applies to every view that draws it in the title bar.
  if (_batteryIndicator.poll()) requestDraw();
  // Replay drives a sandbox recorder from a stored shot; the live recorder and
  // the real sensors above are ignored while it runs (so a real pull mid-replay
  // is simply not recorded, this is an accepted trade-off).
  if (_viewMode == ViewMode::Replay) {
    tickReplay();
    return stay();
  }

  // Advance the recorder and persist any finished shot. The controller does the
  // locked work and wakes SSE readers when web-visible state changed, then
  // hands back what the UI needs to react to without touching the recorder
  // again.
  const auto outcome =
      _controller.tick(millis(), _lastTriggered, snap, wallclock::utcNow(),
                       newScaleSample, scaleStateChanged);
  if (outcome.latestAcceptedShotSaved != _latestAcceptedShotSaved) {
    _latestAcceptedShotSaved = outcome.latestAcceptedShotSaved;
    requestDraw();
  }
  power::powerManager.setForegroundFullPerformanceRequired(outcome.active);

  // A confirmed pour grabs focus and re-enables the alert audio for the shot.
  // Candidate vibration-only windows should not steal focus from the
  // LastShot / LastShotChart views.
  if (outcome.meaningfulPourStarted) {
    _cutAudioSilenced = false;
    if (_viewMode != ViewMode::Live) {
      _viewMode = ViewMode::Live;
    }
  }

  driveAlert(outcome.alertState, millis());

  // Keep the in-gauge BLE icon animation ticking when searching.
  if (_bleIcon.poll(bleStatus())) requestDraw();

  // The controller already woke SSE readers if web-visible state changed; the
  // display answers the same question, so redraw on the same signal.
  if (outcome.frameNeeded) requestDraw();

  if (outcome.active) {
    power::powerManager.keepAwake();
  }
  return stay();
}

ScreenResult ExtractionScreen::onEvent(button::Gesture event) {
  // Replay has its own button map.
  if (_viewMode == ViewMode::Replay) return onEventReplay(event);

  switch (event) {
    case button::Gesture::A_SHORT: {
      // During the cut state, A_SHORT silences the current shot's audio cue
      // without changing the visual cut-now screen.
      if (_controller.currentCutState()) {
        _cutAudioSilenced = true;
        sounds::targetSilence();
        return stay();
      }
      // Tap cycles Live -> LastShot -> LastShotChart -> Live when a shot is
      // available. With no recorded shot, the cycle skips the chart view.
      if (_viewMode == ViewMode::Live) {
        _viewMode = ViewMode::LastShot;
      } else if (_viewMode == ViewMode::LastShot) {
        _viewMode = _controller.hasEffectiveLastShot() ? ViewMode::LastShotChart
                                                       : ViewMode::Live;
      } else {  // LastShotChart
        _viewMode = ViewMode::Live;
      }
      requestDraw();
      return stay();
    }
    case button::Gesture::A_LONG: {
      // From LastShot or LastShotChart, hold replays the shot on screen
      // (loaded or last accepted) through the sandbox pipeline.
      if (_viewMode == ViewMode::LastShot ||
          _viewMode == ViewMode::LastShotChart) {
        if (_controller.hasEffectiveLastShot()) {
          startReplay();
          return stay();
        }
        return ignored();
      }
      // In Live, hold is the contextual recorder action.
      if (_viewMode != ViewMode::Live) return ignored();
      using CA = pump_scale::ExtractionController::ContextAction;
      switch (_controller.contextAction()) {
        case CA::RequestTare:
          bleScale.requestTare();
          return stay();
        case CA::Cleared:
          // Clear any alert output along with the finished live view.
          sounds::targetSilence();
          _alertState = {};
          _cutAudioSilenced = false;
          _lastCountdownBucket = 0;
          requestDraw();
          return stay();
        case CA::None:
          return ignored();
      }
      return ignored();
    }
    case button::Gesture::B_SHORT:
      return exit();
    default:
      return ignored();
  }
}

bool ExtractionScreen::onDraw(LGFX_Sprite* c) {
  drawLayout(c);
#if PB_SCALE_READING_TIMING
  if (_viewMode == ViewMode::Live &&
      _lastScaleSnap.state == BleScaleService::State::READY &&
      _lastScaleSnap.weight.timestampMs != 0) {
    _drawnWeightSequence = _lastScaleSnap.weight.sequence;
  } else {
    _drawnWeightSequence = 0;
  }
#endif
  return true;
}

#if PB_SCALE_READING_TIMING
void ExtractionScreen::onPresented() {
  if (_drawnWeightSequence == 0) return;
  pump_scale::observeScaleReadingPresented(_drawnWeightSequence, millis());
}
#endif

ExtractionScreen::ScreenModel ExtractionScreen::modelFor() const {
  ScreenModel m{};
  m.viewMode = _viewMode;

  if (_viewMode == ViewMode::LastShot || _viewMode == ViewMode::LastShotChart) {
    fillLastShotModel(m);
  } else if (_viewMode == ViewMode::Replay) {
    // The controller's recorder holds the replay (driven by replayTick); the
    // scale reading is the synthesized frame, and the elapsed timer runs on
    // the virtual replay clock.
    int16_t yieldCg = pump_scale::Extraction::NO_WEIGHT;
    const bool haveYield = _controller.currentYieldCg(_replaySnap, yieldCg);
    fillLiveModel(m, haveYield, yieldCg, _replaySnap.grams, _replaySnap.present,
                  _replayer.virtualNowMs());
    m.cutState = _controller.currentCutState();
    m.flowCgPerS = _alertState.flowCgPerS;
    m.flowValid = _alertState.flowValid;
  } else {  // Live
    const pump_scale::ScaleSnapshot snap =
        pump_scale::scaleSnapshotFromBle(_lastScaleSnap);
    const bool scaleReady =
        _lastScaleSnap.state == BleScaleService::State::READY;
    const bool haveLiveWeight =
        scaleReady && _lastScaleSnap.weight.timestampMs != 0;
    int16_t yieldCg = pump_scale::Extraction::NO_WEIGHT;
    const bool haveYield = _controller.currentYieldCg(snap, yieldCg);
    fillLiveModel(m, haveYield, yieldCg, _lastScaleSnap.weight.grams,
                  haveLiveWeight, millis());
    m.cutState = _controller.currentCutState();
    m.flowCgPerS = _alertState.flowCgPerS;
    m.flowValid = _alertState.flowValid;

    const pump_scale::ExtractionController::LastShotSelection selection =
        _controller.effectiveLastShotSelection();
    m.summaryShot = selection.shot;
    if (selection.flowStats) m.summaryFlow = *selection.flowStats;
  }
  return m;
}

void ExtractionScreen::fillLiveModel(ScreenModel& m, bool haveYield,
                                     int16_t yieldCg, float rawGrams,
                                     bool haveRawWeight, uint32_t nowMs) const {
  const pump_scale::Extraction& cur = _controller.current();
  m.empty = false;
  m.chart = &cur;
  m.pumpDetected = cur.phase == pump_scale::Phase::RUNNING;

  if (_controller.currentFinishedShotIsDisplayable()) {
    m.liveState = ScreenModel::LiveState::FinishedRealShot;
  } else if (_controller.currentIsPouring()) {
    m.liveState = ScreenModel::LiveState::Pouring;
  } else {
    m.liveState = ScreenModel::LiveState::Ready;
  }

  // The live scrolling trace is shown only while actively pouring. Finished
  // real shots render a static chart; everything else shows a blank baseline.
  m.showLiveSamples = (m.liveState == ScreenModel::LiveState::Pouring);

  // Self-tared display for yield in Pouring and FinishedRealShot; raw scale
  // weight otherwise. The band brightens to mark yield mode.
  m.gaugeActive = (m.liveState == ScreenModel::LiveState::Pouring ||
                   m.liveState == ScreenModel::LiveState::FinishedRealShot) &&
                  haveYield;
  if (m.gaugeActive) {
    m.gaugeGrams = yieldCg / 100.0f;
    m.gaugeValid = true;
    // Show the scale's own reading underneath only when it diverges from our
    // self-tared value — i.e. the operator left the cup weight on the scale.
    // 0.2 g rides just above BLE weight jitter.
    m.scaleGrams = rawGrams;
    m.showScaleSecondary =
        haveRawWeight && std::fabs(rawGrams - m.gaugeGrams) > 0.2f;
  } else {
    m.gaugeGrams = rawGrams;
    m.gaugeValid = haveRawWeight;
    m.showScaleSecondary = false;
  }

  // Start the timer when vibration first indicates pump operation, even before
  // enough yield exists to identify a pour. If detection ends without a
  // confirmed pour, return to 0:00 with the Ready state. Confirmed pours keep
  // showing elapsed time from the record start.
  if (m.liveState == ScreenModel::LiveState::Pouring ||
      m.liveState == ScreenModel::LiveState::FinishedRealShot ||
      m.pumpDetected) {
    m.timerMs = pump_scale::extractionElapsedMs(cur, nowMs);
  } else {
    m.timerMs = 0;
  }
}

void ExtractionScreen::fillLastShotModel(ScreenModel& m) const {
  const pump_scale::ExtractionController::LastShotSelection selection =
      _controller.effectiveLastShotSelection();
  const pump_scale::Extraction* last = selection.shot;
  m.empty = last == nullptr;
  m.chart = last;
  m.gaugeGrams = m.empty ? 0.0f : last->yieldCg / 100.0f;
  m.gaugeValid = !m.empty && last->yieldCg != pump_scale::Extraction::NO_WEIGHT;
  m.gaugeActive = false;
  m.showScaleSecondary = false;
  m.showLiveSamples = true;  // static chart; the pouring-only rule is live-only
  m.timerMs = m.empty ? 0 : pump_scale::extractionElapsedMs(*last, millis());
  m.pumpDetected = false;
  m.summaryShot = last;
  if (selection.flowStats) m.summaryFlow = *selection.flowStats;
  // liveState is only consulted by Live/Replay; keep it well-defined anyway.
  m.liveState = ScreenModel::LiveState::Ready;
}

void ExtractionScreen::startReplay() {
  const pump_scale::Extraction* shot = _controller.effectiveLastShot();
  if (!shot) return;
  // Prime the replay predictor with the target config captured when the
  // original pull began, so the alert fires at the same point.
  _controller.setTargetCoeffs(
      pump_scale::resolveReplayCoeffs(*shot, _targetStore.coeffs()));
  // Wipe the recorder's in-flight state (and sync the finalize cursor) so the
  // replay starts clean and its later finalize isn't mistaken for a real shot.
  _controller.resetInFlightAndNotify();
  _replaySnap = pump_scale::ScaleSnapshot{};
  _replayer.start(*shot, millis());
  _viewMode = ViewMode::Replay;
  _replayShowFlowBand = false;
  power::powerManager.setForegroundFullPerformanceRequired(true);
  sounds::targetSilence();
  _alertState = {};
  _cutAudioSilenced = false;
  _lastCountdownBucket = 0;
  requestDraw();
}

void ExtractionScreen::stopReplay() {
  power::powerManager.setForegroundFullPerformanceRequired(false);
  _replayer.stop();
  // Wipe the recorder's replay state (and resync the finalize cursor) so
  // /current and the next real shot are unaffected.
  _controller.resetInFlightAndNotify();
  // Restore the live user target coeffs; startReplay() overwrites them with
  // the replayed shot's recorded snapshot.
  applyTargetCoeffs();
  sounds::targetSilence();
  _alertState = {};
  _cutAudioSilenced = false;
  _lastCountdownBucket = 0;
  _replayShowFlowBand = false;
  _viewMode = ViewMode::LastShot;  // back to the static view of the shot
  requestDraw();
}

void ExtractionScreen::tickReplay() {
  // Pausing freezes the whole replay presentation, not only its virtual clock.
  // In particular, driveAlert() schedules countdown beeps from wall time, so
  // feeding it a frozen APPROACHING state would keep the cadence sounding.
  if (_replayer.paused()) {
    power::powerManager.keepAwake();
    return;
  }

  const uint32_t now = millis();
  bool pumpOn = false;
  bool newSample = false;
  pump_scale::ScaleSnapshot snap{};
  if (!_replayer.tick(now, pumpOn, snap, newSample)) {
    stopReplay();  // played to the end
    return;
  }
  _replaySnap = snap;

  // Drive the controller's recorder on the virtual clock (so sample times,
  // dropout/settle timeouts, and synthesized timestamps share one ms space),
  // with all live side effects suppressed by replayTick.
  const uint32_t vNow = _replayer.virtualNowMs();
  const auto outcome = _controller.replayTick(vNow, pumpOn, snap);

  if (outcome.meaningfulPourStarted) {
    _cutAudioSilenced = false;
  }

  driveAlert(outcome.alertState, millis());

  power::powerManager.keepAwake();
  requestDraw();
}

ScreenResult ExtractionScreen::onEventReplay(button::Gesture event) {
  switch (event) {
    case button::Gesture::A_SHORT: {
      const uint32_t now = millis();
      _replayer.setPaused(!_replayer.paused(), now);
      if (_replayer.paused()) {
        // Hush the current cue; tickReplay() suspends the cadence while frozen.
        sounds::targetSilence();
      } else {
        // Restart cadence timing from resume rather than counting paused wall
        // time. Do not replay a latched STOP_NOW cue: after pump-off it may be
        // historical rather than a sound that was active when playback paused.
        _lastBeepMs = now;
        if (_alertState.level == TargetAlert::Level::APPROACHING) {
          sounds::prepareTargetAlert();
        }
      }
      requestDraw();
      return stay();
    }
    case button::Gesture::A_LONG:
      _replayShowFlowBand = !_replayShowFlowBand;
      requestDraw();
      return stay();
    case button::Gesture::B_SHORT:
      stopReplay();
      return stay();
    default:
      return ignored();
  }
}

int ExtractionScreen::drawHeader(LGFX_Sprite* c, const char* title,
                                 uint32_t titleColor, uint32_t bg) const {
  const int headerH =
      ui::drawViewHeader(c, title, titleColor, nullptr, 0, bg, bg);

  // Battery gauge on the right side of the title bar. It is skipped
  // automatically because drawHeader is only used by Live/Replay/LastShot;
  // LastShotChart has its own header.
  constexpr int batterySlotW = 24 + 4;
  _batteryIndicator.draw(*c, c->width() - batterySlotW,
                         ui::kViewHeaderContentOffsetY, batterySlotW, headerH,
                         bg);

  return headerH;
}

void ExtractionScreen::drawTimerCell(LGFX_Sprite* c, uint32_t timerMs,
                                     layout::rect box, uint32_t color) const {
  const int totalTenths = static_cast<int>(timerMs / 100);
  const int minutes = totalTenths / 600;
  const int secTenths = totalTenths % 600;
  const int seconds = secTenths / 10;
  const int tenths = secTenths % 10;
  char timer[16];
  std::snprintf(timer, sizeof(timer), "%d:%02d.%d", minutes, seconds, tenths);
  c->setTextColor(color, theme::bg());

  const int valueX = box.x;
  const int valueW = box.w;
  // The timer resolves to tenths of a second; keep the decimal smaller and the
  // decimal column steady as the value changes. No unit is shown for the new
  // M:SS.D format.
  gauge::drawCentered(c, timer, "", valueX, box.y, valueW, box.h,
                      {.sizeRef = "8:88.8"});
}

void ExtractionScreen::drawWeightGauge(LGFX_Sprite* c, const ScreenModel& m,
                                       layout::rect box) const {
  const int pad = 2;
  // The weight gauge has no background in normal operation. The cut state is
  // the only time it fills blue, so the white-over-blue cue is reserved.
  const uint32_t bg = m.cutState ? theme::accent() : theme::bg();
  const uint32_t fg = m.cutState ? theme::bg() : theme::fg();
  if (m.cutState) {
    c->fillSmoothRoundRect(box.x, box.y, box.w, box.h, pad, bg);
  }
  c->setTextColor(fg, bg);

  if (!m.gaugeValid) {
    // No scale connected: show the BLE beacon icon pulsing above the message.
    auto [iconBox, textBox] =
        layout::splitV(layout::inset(box, pad), 0.5f, pad);
    // Draw the BLE beacon at 2x scale so it reads clearly in the center of the
    // weight gauge.
    _bleIcon.draw(*c, iconBox.x, iconBox.y, iconBox.w, iconBox.h, bg);
    font::fit(c, kNoScaleText, textBox, kNoScaleFont);
    layout::drawTopCenter(c, kNoScaleText, textBox);
    return;
  }

  char value[16];
  std::snprintf(value, sizeof(value), "%.1f", m.gaugeGrams);

  // Draw the weight value + unit in a fixed-column gauge.
  gauge::drawFixedColumn(c, value, "", layout::inset(box, pad),
                         {.sizeRef = "88.8"});

  // Raw scale reading, small in the top-right corner, when it diverges from the
  // self-tared headline (the operator didn't pre-tare the cup). The headline
  // stays the dominant number; this just shows what the scale itself reads.
  if (m.showScaleSecondary) {
    char raw[16];
    std::snprintf(raw, sizeof(raw), "%.1fg", m.scaleGrams);
    c->setFont(font::tiny());
    c->setTextSize(1);
    c->setTextColor(fg, bg);
    const int rawWidth = c->textWidth(raw) + pad * 2;
    const int rawHeight = c->fontHeight() + pad * 2;
    const layout::rect rawBox{box.x + box.w - rawWidth - pad, box.y + pad,
                              rawWidth, rawHeight};
    c->fillRectAlpha(rawBox.x, rawBox.y, rawBox.w, rawBox.h, 128, theme::dim());
    layout::drawTopRight(c, raw, layout::inset(rawBox, 2));
  }
}

void ExtractionScreen::drawLayout(LGFX_Sprite* c) {
  c->fillScreen(theme::bg());

  const ScreenModel m = modelFor();
  const uint32_t chartNow =
      _viewMode == ViewMode::Replay ? _replayer.virtualNowMs() : millis();

  if (m.viewMode == ViewMode::Live || m.viewMode == ViewMode::Replay) {
    drawLiveView(c, m);
  } else if (m.viewMode == ViewMode::LastShot) {
    drawLastShotView(c, m);
  } else if (m.viewMode == ViewMode::LastShotChart) {
    drawLastShotChartView(c, m, chartNow);
  }
  drawStorageWarning(c);
}

void ExtractionScreen::drawStorageWarning(LGFX_Sprite* c) const {
  const char* message = nullptr;
  // Keep this precedence aligned with appendStorageWarning() in
  // web-src/app/extraction/app.js.
  if (_storageState != storage::MountState::Ready) {
    message = "NOT RECORDING";
  } else if (!_latestAcceptedShotSaved) {
    message = "SHOT NOT SAVED";
  }
  if (!message) return;

  if (_viewMode == ViewMode::LastShotChart) {
    ui::drawViewHeader(c, message, theme::critical_fill_fg(), nullptr, 0,
                       theme::critical_fill(), theme::critical_fill());
  } else {
    drawHeader(c, message, theme::critical_fill_fg(), theme::critical_fill());
  }
}

void ExtractionScreen::drawLiveView(LGFX_Sprite* c, const ScreenModel& m) {
  const bool isLive = m.viewMode == ViewMode::Live;
  const char* title =
      isLive ? _liveViewTitle
             : (_replayer.paused() ? font::glyph::PAUSE : font::glyph::PLAY);

  constexpr int pad = 2;
  const bool landscape = c->width() > c->height();
  const bool dimTimer = (m.liveState != ScreenModel::LiveState::Pouring);

  // During the cut state, tint the whole view blue so the cue is ambient.
  const uint32_t titleColor = m.cutState ? theme::bg() : theme::accent();
  const uint32_t headerBg = m.cutState ? theme::accent() : theme::bg();
  if (m.cutState) c->fillScreen(theme::accent());

  const int headerH = drawHeader(c, title, titleColor, headerBg);

  if (landscape) {
    // Landscape: use the full screen for the gauge; the header is drawn
    // underneath so the gauge sits on top of it.
    const layout::rect body =
        layout::inset({0, 0, c->width(), c->height()}, pad);
    drawLiveBody(c, m, body, dimTimer, kBatteryIconW);
  } else {
    // Portrait: plenty of vertical space, so keep the header band reserved.
    const layout::rect body =
        layout::inset({0, headerH, c->width(), c->height() - headerH}, pad);
    drawLiveBody(c, m, body, dimTimer);
  }
}

void ExtractionScreen::drawLastShotView(LGFX_Sprite* c,
                                        const ScreenModel& m) const {
  constexpr int pad = 2;
  const int headerH = drawHeader(c, "LAST SHOT", theme::accent(), theme::bg());
  const layout::rect body =
      layout::inset({0, headerH, c->width(), c->height() - headerH}, pad);
  drawLastShot(c, m, body);
}

void ExtractionScreen::drawLastShot(LGFX_Sprite* c, const ScreenModel& m,
                                    layout::rect body) const {
  if (m.empty) {
    // Last-shot view with nothing recorded: placeholder only.
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawCenteredInBox(c, "No recent shot", body, font::textFamily());
    return;
  }

  // Full-body summary: choose horizontal or vertical stacking based on
  // aspect ratio so the metrics read well in both orientations.
  drawSummaryPanel(c, m, body);
}

void ExtractionScreen::drawLastShotChartView(LGFX_Sprite* c,
                                             const ScreenModel& m,
                                             uint32_t chartNow) const {
  constexpr int pad = 2;
  c->setFont(font::body());
  c->setTextSize(1);
  const int fontH = c->fontHeight();

  const uint32_t kWeightColor = theme::accent_light();
  const uint32_t kFlowColor = theme::chart_flow();
  constexpr int kSwatchPad = 4;
  const int swatchSize = std::max(4, fontH / 2);

  const int weightTextW = c->textWidth("WEIGHT");
  const int flowTextW = c->textWidth("FLOW");

  // Portrait: stack WEIGHT and FLOW vertically; landscape: place them side by
  // side so the legend always fits without crowding.
  const bool verticalLegend = c->height() > c->width();
  const int legendGap = verticalLegend ? 2 : 12;
  int legendH;
  int legendX, legendY;
  if (verticalLegend) {
    legendH = std::max(2 * fontH + legendGap + 2 * pad, 24);
    const int groupW =
        swatchSize + kSwatchPad + std::max(weightTextW, flowTextW);
    legendX = (c->width() - groupW) / 2;
    legendY = (legendH - (2 * fontH + legendGap)) / 2;
  } else {
    legendH = std::max(fontH + pad * 2, 24);
    const int legendW =
        2 * swatchSize + 2 * kSwatchPad + weightTextW + legendGap + flowTextW;
    legendX = (c->width() - legendW) / 2;
    legendY = (legendH - fontH) / 2;
  }

  // The chart legend uses a black band so both series colors remain readable
  // in either theme.
  c->fillRect(0, 0, c->width(), legendH, 0x000000);

  auto drawLegendItem = [&](int x, int y, uint32_t color, const char* label) {
    c->fillRoundRect(x, y + (fontH - swatchSize) / 2, swatchSize, swatchSize,
                     swatchSize / 2, color);
    c->setTextColor(color, 0x000000);
    layout::drawTopLeft(c, label, x + swatchSize + kSwatchPad, y);
  };

  if (verticalLegend) {
    drawLegendItem(legendX, legendY, kFlowColor, "FLOW");
    drawLegendItem(legendX, legendY + fontH + legendGap, kWeightColor,
                   "WEIGHT");
  } else {
    int x = legendX;
    drawLegendItem(x, legendY, kFlowColor, "FLOW");
    x += swatchSize + kSwatchPad + weightTextW + legendGap;
    drawLegendItem(x, legendY, kWeightColor, "WEIGHT");
  }

  const layout::rect body = {0, legendH, c->width(), c->height() - legendH};

  if (m.empty || !m.chart) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawCenteredInBox(c, "No recent shot", body, font::textFamily());
    return;
  }

  pump_scale::ExtractionView::draw(c, *m.chart, chartNow, body.x, body.y,
                                   body.w, body.h, m.showLiveSamples);
}

// During replay, m.chart is the shot being reconstructed. Until the first
// replay tick reaches RUNNING and stamps the recorded target, these accessors
// fall back to the user's live TargetStore. The PUMP_ON event is at offset
// zero, so the difference is limited to the pre-tick frame.
uint16_t ExtractionScreen::effectiveTargetCg(const ScreenModel& m) const {
  if (m.viewMode == ViewMode::Replay && m.chart) {
    return pump_scale::resolveReplayCoeffs(*m.chart, _targetStore.coeffs())
        .targetCg;
  }
  return _targetStore.targetCg();
}

bool ExtractionScreen::effectiveArmed(const ScreenModel& m) const {
  if (m.viewMode == ViewMode::Replay && m.chart) {
    return pump_scale::resolveReplayCoeffs(*m.chart, _targetStore.coeffs())
        .armed;
  }
  return _targetStore.armed();
}

void ExtractionScreen::drawTargetBand(LGFX_Sprite* c, const ScreenModel& m,
                                      uint16_t targetCg, bool armed,
                                      bool forceFlow, layout::rect box) {
  // Two signals share the band and move independently: the bar is pour
  // progress, the outline around the band means the pump is running. Vibration
  // stopping mid-pour hollows the bar out at the point it reached; a finished
  // shot keeps a solid bar with no outline.
  constexpr int cornerR = 5;
  c->fillRoundRect(box.x, box.y, box.w, box.h, cornerR, theme::surface());

  const auto drawPumpDetectedOutline = [&]() {
    if (m.pumpDetected) {
      c->drawRoundRect(box.x, box.y, box.w, box.h, cornerR, theme::accent());
    }
  };

  if (forceFlow || targetCg == 0 || !armed) {
    const bool pouring = forceFlow || m.pumpDetected ||
                         m.liveState == ScreenModel::LiveState::Pouring;

    if (pouring) {
      // Six grams per second covers the useful range of the archived corpus:
      // ordinary pours occupy the middle of the meter while unusually fast
      // readings saturate without stretching its scale.
      constexpr float kFlowMeterFullScaleCgPerS = 600.0f;
      const float progress =
          m.flowValid
              ? std::clamp(m.flowCgPerS / kFlowMeterFullScaleCgPerS, 0.0f, 1.0f)
              : 0.0f;
      const int fillW = static_cast<int>(box.w * progress);
      if (fillW > 0) {
        c->fillRoundRect(box.x, box.y, fillW, box.h, cornerR,
                         theme::chart_flow());
      }

      char flowValue[16];
      if (m.flowValid) {
        std::snprintf(flowValue, sizeof(flowValue), "%.1f",
                      m.flowCgPerS / 100.0f);
      } else {
        std::snprintf(flowValue, sizeof(flowValue), "--");
      }
      const layout::rect contentBox = layout::inset(box, 2);
      const FlowValueLayout flowLayout =
          layoutFlowValue(c, flowValue, contentBox);
      const int fillRight = box.x + fillW;
      if (fillW < box.w) {
        layout::ClipScope clip(c, fillRight, box.y, box.w - fillW, box.h);
        drawFlowNumber(c, flowLayout, flowValue, theme::dim(),
                       theme::surface());
      }
      if (fillW > 0) {
        layout::ClipScope clip(c, box.x, box.y, fillW, box.h);
        drawFlowNumber(c, flowLayout, flowValue, theme::chart_bg(),
                       theme::chart_flow());
      }
      drawFlowUnit(c, _flowUnitUnfilled, _flowUnitFilled, flowLayout,
                   fillRight);
      drawPumpDetectedOutline();
      c->setTextSize(1);
      return;
    }

    const char* label = "OFF";
    const layout::rect contentBox = layout::inset(box, 3);
    const int iconSize = std::min(16, std::max(3, contentBox.h - 2));
    constexpr int iconGap = 3;
    const int textBudget = std::max(1, contentBox.w - iconSize - iconGap);
    const int textHeight = std::min(contentBox.h, iconSize + 4);
    font::fit(c, label, textBudget, textHeight, font::textFamily());
    const int groupW = iconSize + iconGap + c->textWidth(label);
    const int groupX = contentBox.x + std::max(0, contentBox.w - groupW) / 2;
    const int centerY = contentBox.y + contentBox.h / 2;
    ui::drawTargetIcon(c, {groupX, centerY - iconSize / 2, iconSize, iconSize},
                       theme::dim());
    c->setTextColor(theme::dim(), theme::surface());
    layout::drawMiddleLeft(c, label, groupX + iconSize + iconGap, centerY);
    c->setTextSize(1);
    return;
  }

  const float targetGrams = targetCg / 100.0f;

  // Progress tracks self-tared yield only; raw scale weight before the pour
  // (Ready state) or after a reset should not fill the bar.
  float progress =
      (m.gaugeActive && m.gaugeValid) ? (m.gaugeGrams / targetGrams) : 0.0f;
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;

  char targetValue[16];
  std::snprintf(targetValue, sizeof(targetValue), "%.1f", targetGrams);

  const int fillW = static_cast<int>(box.w * progress);
  const bool solidProgress =
      m.pumpDetected || m.liveState == ScreenModel::LiveState::FinishedRealShot;
  if (fillW > 0 && solidProgress) {
    c->fillRoundRect(box.x, box.y, fillW, box.h, cornerR, theme::accent());
  } else if (fillW > 0) {
    // Clip the full outline instead of drawing a vertical endpoint that could
    // cross the target label.
    layout::ClipScope clip(c, box.x, box.y, fillW, box.h);
    c->drawRoundRect(box.x, box.y, box.w, box.h, cornerR, theme::accent());
  }

  const layout::rect contentBox = layout::inset(box, 2);
  const TargetValueLayout targetLayout =
      layoutTargetValue(c, targetValue, contentBox);
  drawTargetValue(c, targetLayout, targetValue,
                  solidProgress ? theme::accent() : theme::dim(), theme::dim(),
                  theme::surface());

  // Knock out the part of the label that overlaps the accent fill by redrawing
  // it in the background color, clipped to the section occupied by the fill.
  if (fillW > 0 && solidProgress) {
    layout::ClipScope clip(c, box.x, box.y, fillW, box.h);
    drawTargetValue(c, targetLayout, targetValue, theme::bg(), theme::bg(),
                    theme::accent());
  }
  drawPumpDetectedOutline();
}

void ExtractionScreen::drawLiveBody(LGFX_Sprite* c, const ScreenModel& m,
                                    layout::rect body, bool dimTimer,
                                    int gaugeMargin) {
  constexpr int pad = 2;
  const uint16_t targetCg = effectiveTargetCg(m);
  const bool targetArmed = effectiveArmed(m);
  const bool forceFlow = m.viewMode == ViewMode::Replay && _replayShowFlowBand;

  // Portrait: weight gauge on top, target/flow band, then timer. Landscape:
  // weight gauge occupies the full width above a taller bottom strip where the
  // timer and band sit side-by-side.
  layout::rect weightBox, timerBox;
  if (body.w > body.h) {
    const int bottomH = std::max(static_cast<int>(body.h * 0.3), 32);
    const auto areaSplit = layout::splitVFixedBottom(body, bottomH, pad);
    weightBox = areaSplit.first;
    layout::rect bottomBox = areaSplit.second;
    // Give the timer a fixed share of the bottom line so the target band keeps
    // a stable position whether configured, disabled, or unset.
    const int timerW = static_cast<int>(bottomBox.w * 0.38f);
    const auto bottomSplit = layout::splitHFixed(bottomBox, timerW, pad);
    timerBox = bottomSplit.first;
    const auto barBox = layout::inset(bottomSplit.second, 4);
    drawTargetBand(c, m, targetCg, targetArmed, forceFlow, barBox);
  } else {
    const auto split = layout::splitV(body, 0.5, pad);
    weightBox = split.first;
    const auto barSplit = layout::splitV(split.second, 0.45, pad * 2);
    timerBox = barSplit.second;
    const auto barBox = layout::inset(barSplit.first, 4);
    drawTargetBand(c, m, targetCg, targetArmed, forceFlow, barBox);
  }

  // Lateral inset for the weight gauge: centered with a margin equal to the
  // battery icon width on each side.
  if (gaugeMargin > 0) {
    const int inset = std::min(gaugeMargin, weightBox.w / 2);
    weightBox = {weightBox.x + inset, weightBox.y,
                 std::max(0, weightBox.w - 2 * inset), weightBox.h};
  }

  drawWeightGauge(c, m, weightBox);
  const uint32_t timerColor =
      m.cutState ? theme::bg_alt() : (dimTimer ? theme::dim() : theme::fg());
  drawTimerCell(c, m.timerMs, timerBox, timerColor);
}

void ExtractionScreen::drawSummaryPanel(LGFX_Sprite* c, const ScreenModel& m,
                                        layout::rect box) const {
  constexpr int pad = 3;
  const bool horizontalMetrics = box.w > box.h;
  c->fillSmoothRoundRect(box.x, box.y, box.w, box.h, pad, theme::surface());
  const layout::rect contentBox = layout::inset(box, pad);

  if (!m.summaryShot) {
    c->setTextColor(theme::dim(), theme::surface());
    layout::drawCenteredInBox(c, "No recent shot", contentBox,
                              font::textFamily());
    return;
  }

  const pump_scale::Extraction& e = *m.summaryShot;
  const bool haveWeight = e.yieldCg != pump_scale::Extraction::NO_WEIGHT;
  const float grams = haveWeight ? e.yieldCg / 100.0f : 0.0f;
  const uint32_t durMs = pump_scale::extractionElapsedMs(e, millis());

  const char* cause = nullptr;
  switch (e.endCause) {
    case pump_scale::EndCause::TIMEOUT:
      cause = "TIMEOUT";
      break;
    default:
      break;
  }

  char weightVal[16];
  char durationVal[16];
  char flowVal[16];
  char targetVal[16];
  char whenVal[24];
  if (haveWeight) {
    std::snprintf(weightVal, sizeof(weightVal), "%.1f", grams);
  } else {
    std::snprintf(weightVal, sizeof(weightVal), "--.-");
  }
  std::snprintf(durationVal, sizeof(durationVal), "%.1f", durMs / 1000.0f);
  formatShotWhen(e.startUtcSec, whenVal, sizeof(whenVal));
  if (m.summaryFlow.have) {
    std::snprintf(flowVal, sizeof(flowVal), "%.1f",
                  m.summaryFlow.sustainedPeakGPerS);
  } else {
    std::snprintf(flowVal, sizeof(flowVal), "--.-");
  }
  const bool hasTarget = e.hasTargetSnapshot && e.target.targetCg > 0;
  if (hasTarget) {
    std::snprintf(targetVal, sizeof(targetVal), "%.1f g",
                  e.target.targetCg / 100.0f);
  }

  char durText[24];
  char flowText[24];
  std::snprintf(durText, sizeof(durText), "%s s", durationVal);
  std::snprintf(flowText, sizeof(flowText), "%s g/s", flowVal);

  struct Metric {
    const char* label;
    const char* value;
    uint32_t color;
    uint32_t labelColor;
    ui::InfoIconRenderer icon;
  };
  Metric details[4];
  size_t detailCount = 0;
  auto addDetail = [&](const char* label, const char* value, uint32_t color,
                       uint32_t labelColor = theme::dim(),
                       ui::InfoIconRenderer icon = nullptr) {
    if (detailCount >= sizeof(details) / sizeof(details[0])) return;
    details[detailCount++] = {label, value, color, labelColor, icon};
  };

  addDetail("TIME", durText, theme::fg());
  addDetail("FLOW", flowText, theme::fg());
  if (hasTarget) {
    addDetail("TARGET", targetVal, theme::fg(), theme::accent(),
              ui::drawTargetIcon);
  }
  if (cause) addDetail("END", cause, theme::warn());

  auto drawWhenYield = [&](layout::rect yieldBox) {
    const int whenH = layout::clampedSpan(yieldBox.h, 5, 12, 24);
    const auto split = layout::splitVFixed(yieldBox, whenH, 1);
    ui::drawFittedText(c, whenVal, split.first, theme::dim(), theme::surface());

    c->setTextColor(theme::fg(), theme::surface());
    gauge::drawCentered(c, weightVal, "g", layout::inset(split.second, 1),
                        {.sizeRef = "88.8"});
  };

  auto drawDetails = [&](layout::rect detailsBox, bool compactRows) {
    if (detailCount == 0) return;
    if (compactRows) {
      const int rowH = detailsBox.h / static_cast<int>(detailCount);
      const int labelMaxH = std::max(4, rowH - 4);
      const char* labels[sizeof(details) / sizeof(details[0])] = {};
      for (size_t i = 0; i < detailCount; ++i) {
        if (!details[i].icon) labels[i] = details[i].label;
      }
      const ui::InfoRowLabelLayout rowLabels =
          ui::measureInfoRowLabels(c, labels, static_cast<int>(detailCount),
                                   detailsBox.w / 3, labelMaxH);

      for (size_t i = 0; i < detailCount; ++i) {
        const int y = detailsBox.y + static_cast<int>(i) * rowH;
        const int h =
            (i + 1 == detailCount) ? detailsBox.y + detailsBox.h - y : rowH;
        const layout::rect rowBox = {detailsBox.x, y, detailsBox.w, h};
        if (details[i].icon) {
          ui::drawInfoIconRow(c, rowBox, details[i].icon, theme::accent(),
                              details[i].value, details[i].color,
                              theme::surface(), rowLabels.columnWidth);
        } else {
          ui::drawInfoRow(c, rowBox, details[i].label, details[i].value,
                          details[i].color, theme::surface(),
                          rowLabels.columnWidth, rowLabels.font,
                          details[i].labelColor);
        }
      }
      return;
    }

    const int cols = detailCount == 1 ? 1 : 2;
    const int rows = (static_cast<int>(detailCount) + cols - 1) / cols;
    const int cellW = detailsBox.w / cols;
    const int cellH = detailsBox.h / rows;
    for (size_t i = 0; i < detailCount; ++i) {
      const int col = static_cast<int>(i) % cols;
      const int row = static_cast<int>(i) / cols;
      const int x = detailsBox.x + col * cellW;
      const int y = detailsBox.y + row * cellH;
      const int w = col + 1 == cols ? detailsBox.x + detailsBox.w - x : cellW;
      const int h = row + 1 == rows ? detailsBox.y + detailsBox.h - y : cellH;
      const layout::rect cell = layout::inset({x, y, w, h}, 1);
      ui::drawInfoBlock(c, cell, details[i].label, details[i].value,
                        details[i].color, theme::surface(),
                        kStackedDetailLabelRef, details[i].labelColor);
    }
  };

  if (horizontalMetrics) {
    const auto split = layout::splitH(contentBox, 0.48f, 4);
    drawWhenYield(layout::inset(split.first, 1));
    drawRule(c, {split.second.x - 2, split.second.y, 1, split.second.h});
    drawDetails(layout::inset(split.second, 1), /*compactRows=*/true);
  } else {
    const auto split = layout::splitV(contentBox, 0.55f, 4);
    drawWhenYield(layout::inset(split.first, 1));
    drawRule(c, {split.second.x, split.second.y - 2, split.second.w, 1});
    drawDetails(layout::inset(split.second, 1), /*compactRows=*/false);
  }
}

void ExtractionScreen::applyTargetCoeffs() {
  _controller.setTargetCoeffs(_targetStore.coeffs());
}

void ExtractionScreen::driveAlert(const TargetAlert::State& st,
                                  uint32_t nowMs) {
  const TargetAlert::Level lvl = st.level;
  const bool wasOff = _alertState.level == TargetAlert::Level::OFF;

  switch (lvl) {
    case TargetAlert::Level::STOP_NOW: {
      // One sustained cut tone, played on the tick STOP_NOW first fires.
      if (st.stopNowEdge && !_cutAudioSilenced) {
        sounds::targetCut();
        _lastBeepMs = nowMs;
      }
      break;
    }
    case TargetAlert::Level::APPROACHING: {
      // Countdown bucket: ceil(tRemainingMs / 1000), clamped to 1..5.
      // Prediction jitter is smoothed in the audio layer: beeps are scheduled
      // on a bounded wall-clock cadence. The display still follows the raw
      // bucket so the user sees the current estimate.
      int bucket = (st.tRemainingMs > 0)
                       ? static_cast<int>((st.tRemainingMs + 999) / 1000)
                       : 1;
      if (bucket < 1) bucket = 1;
      if (bucket > 5) bucket = 5;
      if (wasOff) {
        // First valid prediction after OFF: seed the bucket and schedule the
        // first beep one cadence period out, so the tick that turns the
        // prediction on is itself silent.
        _lastCountdownBucket = bucket;
        _lastBeepMs = nowMs;
        // Power the codec now so the first beep isn't clipped by speaker-init
        // latency.
        sounds::prepareTargetAlert();
      } else {
        const uint32_t dt = nowMs - _lastBeepMs;
        // Allow an early beep only if the bucket has clearly decreased and we
        // are past the minimum spacing; otherwise enforce the maximum spacing
        // so the cadence stays regular even when the prediction wobbles.
        if (dt >= kCountdownBeepMaxMs ||
            (dt >= kCountdownBeepMinMs && bucket < _lastCountdownBucket)) {
          if (!_cutAudioSilenced) {
            sounds::targetApproach(sounds::countdownBeepFreq(bucket));
            _lastBeepMs = nowMs;
          }
          _lastCountdownBucket = bucket;
        }
      }
      // If the prediction jumps back up by 2+ seconds (the pour slowed),
      // track the new higher bucket. Otherwise the remembered bucket would
      // stay at the old low value, a decrease could never register, and the
      // early-beep path above would stay unreachable for the fresh approach.
      if (bucket > _lastCountdownBucket + 1) {
        _lastCountdownBucket = bucket + 1;
      }
      break;
    }
    case TargetAlert::Level::OFF:
    default:
      // The visual cut state is derived on demand by the controller via
      // currentCutState() and read in the draw path. We only handle audio
      // teardown here: cut the tone once on the way out of an active alert.
      if (_alertState.level != TargetAlert::Level::OFF) sounds::targetSilence();
      break;
  }
  // TargetAlert maintains its flow estimator even when its target is disabled.
  // Retain the whole state so the inactive target band uses that same signal
  // rather than calculating a display-only flow rate.
  _alertState = st;
}
