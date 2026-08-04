// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "apps/extraction/Extraction.h"
#include "apps/extraction/FlowEstimator.h"

// TargetAlert — live "cut the pump now" prediction for a target shot yield.
//
// Works entirely in yield (the coffee poured into the cup so far) instead of
// raw scale weight. Each tick the controller passes in the recorder's
// TrustedYieldSample (see ExtractionRecorder::lastTrustedYieldSample) and reads
// back an alert descriptor; the ExtractionScreen owns only the beeps and flash.
// This is a pure decision component with no BLE, filesystem, UI, or Arduino
// dependency, so it is host-testable and lives in the extraction core next to
// FlowEstimator.
//
// The input is deliberately the *trusted* yield, not the raw one: the recorder
// advances it only on flow-consistent samples, so an operator knock or a cup
// lift never moves the yield this class sees and never reaches its flow
// estimator.
//
// Prediction model: yield keeps rising after the pump cuts, because the column
// drains through the puck and the group head drips. That post-cut gain is
// modelled as
//
//     overshoot = clamp(flow·τ + c, 0, MAX)        // grams
//
// so the projected settled yield is `yield + overshoot`, and the alert says to
// cut when `projectedFinal ≥ target`. A single clamped `overshoot` value drives
// both the STOP_NOW decision and the cadence clock `tRemaining`, so a signed
// `c` can never make the two disagree.
//
//   tRemainingMs = (target − yield − overshoot)/flow − reactionLead
//   STOP_NOW  ⟺  tRemainingMs ≤ 0
//
// `τ` and `c` describe the physical post-cut yield and are learned later by a
// calibration step. `reactionLead` is the *human* lag between the beep and the
// operator flipping the switch — alert timing only, kept separate so it never
// pollutes `τ` or `c`.
//
// Units: centigrams (cg = 0.01 g) and cg/s used for the interface; the
// estimator works in floats internally. Flow estimation runs whenever samples
// arrive, whether or not the alert is enabled.
namespace pump_scale {

class TargetAlert {
 public:
  enum class Level : uint8_t {
    OFF,          // not enabled, no target, or no usable signal yet
    APPROACHING,  // enabled, flow valid, nearing the cut point — beep cadence
    STOP_NOW,     // cut the pump now — sustained tone + blue gauge cue; once
                  // it turns on it stays on for the rest of the shot
  };

  struct State {
    Level level = Level::OFF;
    int32_t tRemainingMs = 0;  // until "cut now"; ≤ 0 at STOP_NOW
    int16_t flowCgPerS = 0;    // smoothed flow (0 when invalid)
    bool flowValid = false;
    int16_t projectedFinalCg = 0;
    bool stopNowEdge = false;   // true only on the tick STOP_NOW first turns
                                // on; the recorder uses this to capture the
                                // single ALARM_TRIGGERED event per shot.
    bool stopNowFired = false;  // STOP_NOW has turned on this shot (it stays
                                // on until the next shot starts)
  };

  void setCoeffs(const TargetCoeffs& c) { _coeffs = c; }
  const TargetCoeffs& coeffs() const { return _coeffs; }

  // Start-of-shot reset (recorder IDLE→RUNNING). Clears the flow estimator
  // and STOP_NOW. Independent of `armed`.
  void onRunningEntry();

  // Full reset: estimator, STOP_NOW, and last-sample bookkeeping. For
  // onExit().
  void reset();

  // One tick. `sample` is the recorder's TrustedYieldSample; a repeated
  // `sample.seq` means "no new trusted reading" (a held value or a withheld
  // knock) and the flow and yield are held. `phaseRunning` is true only while
  // the recorder is RUNNING — predicting and alerting make sense only then,
  // and the flow estimate freezes across a pump-off so it survives if later a
  // pump-on is coalesced with (merged into) the same-shot. STOP_NOW turns on
  // once the projection reaches the target and stays on until the next
  // onRunningEntry(). It never needs to retract, because `sample` contains a
  // trusted yield, so STOP_NOW can only fire on a genuine approach.
  const State& update(uint32_t nowMs, bool phaseRunning,
                      const TrustedYieldSample& sample);

  const State& state() const { return _state; }

  // Snapshot the latest live values for the pump-off calibration pairing.
  AlarmContext snapshotAtStop() const;

 private:
  static constexpr float kFlowFloorCgPerS = 30.0f;  // 0.3 g/s
  static constexpr int16_t kMaxOvershootCg = 1000;  // 10 g safety clamp
  static constexpr uint32_t kApproachWindowMs = 5000;

  // Shared causal windowed-slope flow estimator, fed the trusted yield here.
  FlowEstimator _flow;

  TargetCoeffs _coeffs;
  State _state;

  uint32_t _lastSeq = 0;  // dedup key for incoming trusted samples
  bool _haveLastSeq = false;
  // True once the projection has reached the target this shot. One-way: stays
  // true until onRunningEntry()/reset(). The trusted input means it can only
  // turn on during a genuine approach, so it never needs to turn itself off.
  bool _stopNowFired = false;

  // Held live values for snapshotAtStop / dup-seq ticks. _haveSample is sticky:
  // true once any trusted sample has been ingested this shot.
  int16_t _lastYieldCg = 0;
  bool _haveSample = false;
};

}  // namespace pump_scale
