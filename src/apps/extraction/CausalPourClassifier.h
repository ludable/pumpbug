// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "FlowBands.h"
#include "FlowEstimator.h"

namespace pump_scale {

// CausalPourBand assigns each scale sample to a flow band. The flow estimate
// behind it (FlowEstimator) smooths the weight changes over the last several
// samples, not just the last step, and uses only samples already seen, so it
// works live. Live pour tracking and the batch evidence pass that runs when a
// shot ends (computePourEvidence in RobustFlow.h) both classify samples
// through this enum, so they always see the same bands. Both group
// consecutive Pour samples into "pour runs", the stretches where coffee is
// judged to be flowing; the comments below say how each band affects an open
// run.
//
// Because the estimate is smoothed, a single sharp step (a placement or a
// tare) does not appear as one impossible jump; it is diluted across the
// window and can read as a brief stretch of plausible flow. Sharp steps are
// therefore detected separately, from the difference between adjacent
// samples: the trustedStep flag and the pour-start invalidation below.
// Consumers derive their decisions with isPourBand(), isSettledBand(), and
// isRunBreakBand() rather than comparing band values directly, so the
// grouping of bands into decisions stays in this file.
enum class CausalPourBand : uint8_t {
  // No sample has been classified yet; snapshots carry this before the first
  // sample. A real sample whose flow cannot be estimated classifies as
  // Dropout, not Unknown.
  Unknown,

  // Flow is effectively flat: |flow| < kSettleEpsCgPerS. Only at a Settled
  // sample does the classifier save a new pour-start candidate. Sustained
  // Settled samples close an open run; this is the normal end of a pour.
  Settled,

  // The weight is still moving, but too slowly to fit any other band: rising
  // below kFloorCgPerS (typically the drip tail at the end of a shot), or
  // falling too gently to be a tare or cup lift (less steep than
  // kDiscontinuityCgPerS). Tail samples do not close an open run, so a rising
  // drip tail still raises the run's recorded peak.
  Tail,

  // Flow is in the espresso pour band: kFloorCgPerS <= flow <= kCeilingCgPerS.
  // A run may open only when this band also arrives on a trusted raw step.
  Pour,

  // Flow is above kCeilingCgPerS: something was placed on or pushed the scale.
  // Sustained Step samples close an open run.
  Step,

  // Flow is at or below kDiscontinuityCgPerS: a tare, cup lift, or bump
  // collapse. Sustained Discontinuity samples close an open run.
  Discontinuity,

  // No flow estimate exists at this sample: either too few samples have
  // arrived yet, or the scale went silent long enough that the estimator's
  // trailing window has no usable data. Sustained Dropout samples close an
  // open run, rather than assume the pour continued while no data was
  // arriving.
  Dropout,
};

inline CausalPourBand classifyCausalPourBand(bool haveFlow, float flowCgPerS) {
  if (!haveFlow) return CausalPourBand::Dropout;
  if (flowCgPerS > kCeilingCgPerS) return CausalPourBand::Step;
  if (flowCgPerS <= kDiscontinuityCgPerS) {
    return CausalPourBand::Discontinuity;
  }
  if (flowCgPerS >= kFloorCgPerS) return CausalPourBand::Pour;
  if (flowCgPerS > -kSettleEpsCgPerS && flowCgPerS < kSettleEpsCgPerS) {
    return CausalPourBand::Settled;
  }
  return CausalPourBand::Tail;
}

inline bool isPourBand(CausalPourBand band) {
  return band == CausalPourBand::Pour;
}

inline bool isSettledBand(CausalPourBand band) {
  return band == CausalPourBand::Settled;
}

inline bool isRunBreakBand(CausalPourBand band) {
  return band == CausalPourBand::Dropout || band == CausalPourBand::Step ||
         band == CausalPourBand::Discontinuity;
}

// Number of consecutive samples outside the pour band that cause the closure of
// an open pour run (and, on the live side, confirm the weight has settled).
// Roughly 600 ms at the ~150 ms BLE sample interval. Live tracking and the
// shot-end evidence pass share this count so both end a pour at the same point.
inline constexpr uint8_t kRunCloseSamples = 4;

struct CausalPourSample {
  bool accepted = false;  // false for duplicate timestamps
  uint32_t tMs = 0;
  int16_t rawCg = 0;
  bool trustedStep = false;
  bool haveFlow = false;
  float flowCgPerS = 0.0f;
  CausalPourBand band = CausalPourBand::Unknown;
  // The weight to use as the pour's starting point if a run opens at this
  // sample: the last settled weight if still trustworthy, else this sample's
  // own weight.
  int16_t pourStartCandidateCg = 0;
};

// Shared causal per-sample classifier for live pour tracking and the shot-end
// evidence pass. It owns only per-sample decisions: duplicate suppression,
// trusted raw steps, trailing-flow classification, and the candidate
// pour-start weight. LivePourTracker (live decisions) and computePourEvidence
// (the shot-end pass) each build their own run logic on top; only this
// classification is shared.
//
// The candidate for pour start weight is the last settled weight. It is
// recorded from either of two signals that the scale is genuinely at rest:
//   - a Settled-band sample whose change from the previous sample is itself no
//     faster than the settled rate (see isFlatStep), or
//   - a sustained run of flat, trusted steps (kRunCloseSamples of them), which
//     seeds the candidate even while the smoothed flow is still outside the
//     Settled band.
// The second signal is the fallback for a large pre-pour transient: its
// collapse leaves the smoothed flow decaying through the negative bands for a
// second or more, so a flat zero plateau after it never classifies as Settled
// before the pour begins. Requiring the change to be flat (not just the
// smoothed flow to be settled) is what keeps the first rising coffee samples,
// which the smoothed flow lags into looking settled, from being taken as the
// start weight.
class CausalPourClassifier {
 public:
  void reset() {
    _flow.reset();
    _lastTMs = 0;
    _haveLast = false;
    _prevCg = 0;
    _lastSettledCg = 0;
    _haveSettled = false;
    _flatRun = 0;
  }

  CausalPourSample update(uint32_t tMs, int16_t cg) {
    CausalPourSample s;
    s.tMs = tMs;
    s.rawCg = cg;

    if (_haveLast && tMs == _lastTMs) return s;

    const uint32_t dtMs = _haveLast ? tMs - _lastTMs : 0;
    const int32_t rawStep = _haveLast ? static_cast<int32_t>(cg) - _prevCg : 0;
    _lastTMs = tMs;
    _prevCg = cg;
    _haveLast = true;

    s.accepted = true;
    s.trustedStep = !isNonPourStep(rawStep, dtMs);

    // Discard the saved settled weight whenever this sample proves it is no
    // longer a trustworthy zero for the next pour. A non-flat but trusted step
    // (the rise into the pour) does NOT discard it: the candidate must survive
    // the pour onset so the run opens against the pre-pour zero.
    if (!s.trustedStep || dtMs > kMaxSampleGapMs) {
      _haveSettled = false;
    }

    // Count consecutive flat, trusted steps within the stream. Any non-flat
    // step, non-pour step, or gap ends the plateau.
    const bool flatStep =
        s.trustedStep && dtMs <= kMaxSampleGapMs && isFlatStep(rawStep, dtMs);
    _flatRun =
        flatStep ? (_flatRun < kRunCloseSamples ? _flatRun + 1 : _flatRun) : 0;

    _flow.push(tMs, cg);
    float f = 0.0f;
    s.haveFlow = _flow.flow(tMs, f);
    s.flowCgPerS = s.haveFlow ? f : 0.0f;
    s.band = classifyCausalPourBand(s.haveFlow, f);

    // Seed the pour-start candidate from a settled-band sample, or from a
    // sustained flat plateau even when the smoothed flow has not yet settled.
    if ((isSettledBand(s.band) && flatStep) || _flatRun >= kRunCloseSamples) {
      _lastSettledCg = cg;
      _haveSettled = true;
    }
    s.pourStartCandidateCg = _haveSettled ? _lastSettledCg : cg;
    return s;
  }

 private:
  // True when the raw step over the gap stays within the settled flow rate
  // (kSettleEpsCgPerS) — the per-step counterpart of the Settled band.
  static bool isFlatStep(int32_t rawStepCg, uint32_t dtMs) {
    if (dtMs == 0) return true;
    const int32_t absStepCg = rawStepCg < 0 ? -rawStepCg : rawStepCg;
    return absStepCg * 1000.0f <= kSettleEpsCgPerS * static_cast<float>(dtMs);
  }

  FlowEstimator _flow;
  uint32_t _lastTMs = 0;
  bool _haveLast = false;
  int16_t _prevCg = 0;

  int16_t _lastSettledCg = 0;
  bool _haveSettled = false;
  uint8_t _flatRun = 0;  // consecutive flat, trusted steps
};

}  // namespace pump_scale
