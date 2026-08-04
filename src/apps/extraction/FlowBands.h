// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// FlowBands: shared vocabulary for classifying scale-weight motion.
// Defines the flow-rate bands of a pour and the per-sample step sizes that mark
// a non-pour transient (a placement, tare, or cup lift).
//
// All consumers of this vocabulary derive from the SAME raw weight stream and
// must agree on what counts as a pour versus a transient, so the thresholds and
// the step classifier live here, in one place, rather than being duplicated:
//   - LivePourTracker and computePourEvidence, which decide the live pour state
//     and the finalized shot evidence.
//   - RobustFlow, which produces the per-sample flow series shown on the charts
//     and reduced into flow statistics.
//
// The flow-rate bands (floor/ceiling/settle/discontinuity) describe the
// SMOOTHED flow estimate. The step helpers (positiveStepLimitCg /
// isNonPourStep) describe a single RAW sample-to-sample jump, read from the raw
// difference rather than the smoothed flow because smoothing would spread a
// sharp step across its window and hide it. One physical fact shapes the
// asymmetric limits below: a pour only ever adds weight to the cup, so a sharp
// drop can never be a pour, at any sampling rate.
//
// Pure logic, no Arduino/BLE dependency, so it runs in host tests. The web
// chart carries a hand-kept JS port of the same constants and helpers (see
// web-src/app/extraction/robust-flow.js); keep the two in sync.
namespace pump_scale {

// Flow band: below the floor is "not flowing"; above the ceiling is a
// placement or bump.
inline constexpr float kFloorCgPerS = 30.0f;      // 0.3 g/s
inline constexpr float kCeilingCgPerS = 1000.0f;  // 10 g/s
// Settled = |flow| below this (≈ weight flat); the gap to the floor is the
// drip-tail band. Empirically a clean shot settles to < 2 cg/s.
inline constexpr float kSettleEpsCgPerS = 10.0f;  // 0.1 g/s
// A downward slope this steep is a tare/cup-lift/removal, not a pour tail.
inline constexpr float kDiscontinuityCgPerS = -500.0f;  // -5 g/s

// A single-sample downward step at least this large is a discontinuity, not a
// pour. The magnitude is fixed rather than scaled by the sample gap: since a
// drop is never a pour, no gap length makes a large one legitimate.
inline constexpr int32_t kStepDiscontinuityCg = 300;  // 3 g
// Margin added to the upward step limit in positiveStepLimitCg, absorbing
// minor overshoot of the ceiling rate. At the nominal ~150 ms sample interval
// the resulting upward limit is about 3 g, the same size as the fixed
// downward limit.
inline constexpr int32_t kStepMarginCg = 150;  // 1.5 g
// The largest gap between samples that still counts as a continuous stream.
// Matches the FlowEstimator window, past which the estimator reports a
// dropout. The positiveStepLimitCg function below uses this value as cap to
// make sure one long gap cannot raise the upward step limit without bound. This
// is also the maximum gap before CausalPourClassifier discards its saved
// settled weight.
inline constexpr uint32_t kMaxSampleGapMs = 750;

// The largest upward single-sample step (centigrams) that could still be a real
// pour over the given inter-sample gap: the ceiling rate times the gap (capped
// at kMaxSampleGapMs), plus kStepMarginCg. A jump past this is a placement or
// an upward knock, not a pour.
inline int32_t positiveStepLimitCg(uint32_t dtMs) {
  const uint32_t stepDtMs = dtMs < kMaxSampleGapMs ? dtMs : kMaxSampleGapMs;
  return static_cast<int32_t>(kCeilingCgPerS * stepDtMs / 1000.0f) +
         kStepMarginCg;
}

// True when a single-sample step is too big to be a pour, in either direction:
// a downward fall past the fixed discontinuity size, or an upward jump past
// the gap-aware limit above.
inline bool isNonPourStep(int32_t rawStepCg, uint32_t dtMs) {
  return rawStepCg <= -kStepDiscontinuityCg ||
         rawStepCg >= positiveStepLimitCg(dtMs);
}

// True when an adjacent-sample DROP is steeper than any pour tail, i.e. its
// rate is past the discontinuity slope. This complements isNonPourStep's fixed
// downward magnitude: the magnitude test catches a slow large drop, this rate
// test catches a fast small one (a collapse, suck-back, or removal that
// arrives as a run of small fast steps rather than one big fall). The upward
// direction needs no rate companion: positiveStepLimitCg already scales with
// the gap.
inline bool isSteepDownwardSlope(int32_t rawStepCg, uint32_t dtMs) {
  if (rawStepCg >= 0 || dtMs == 0) return false;
  // rawStepCg/dtMs * 1000 <= kDiscontinuityCgPerS, kept integer-friendly.
  return static_cast<float>(rawStepCg) * 1000.0f <=
         kDiscontinuityCgPerS * static_cast<float>(dtMs);
}

// A single adjacent-sample edge the robust flow series treats as a transient
// break: a non-pour step (placement up, or a large single-sample drop) OR a
// steep downward slope. Both halves are needed because a collapse can arrive as
// one big step or as a run of smaller fast drops; either way the centred
// difference across it would measure the transient, not local flow.
inline bool isFlowBreakEdge(int32_t rawStepCg, uint32_t dtMs) {
  return isNonPourStep(rawStepCg, dtMs) ||
         isSteepDownwardSlope(rawStepCg, dtMs);
}

}  // namespace pump_scale
