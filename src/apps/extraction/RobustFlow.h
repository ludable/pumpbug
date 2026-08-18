// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "CausalPourClassifier.h"
#include "Extraction.h"
#include "FlowBands.h"

// RobustFlow: the canonical per-sample flow series, derived from the stored
// weight samples and robust to placement bumps, tares, and cup lifts.
//
// Flow at a sample is the centred finite difference of raw weight over a ±500
// ms window (the same estimate the charts have always drawn), but it is
// reported as "no flow" when that window straddles a transient edge no real
// pour can produce, like a placement/bump up or a tare/lift/bump-collapse down.
// Without that guard a cup bumped onto the scale at the start of a shot (weight
// spikes, then falls back to baseline) renders as a large spurious negative
// flow. The transient test is the shared isNonPourStep from FlowBands.h, so
// "what is a bump" has one definition across the pour detector and the
// displayed flow.
//
// This is the single source of flow for everything downstream: the live and
// history charts, flow statistics, etc. It works on the Sample array and
// derives flow on demand; no flow values are stored or sent over the wire.
// Pure logic, no Arduino/BLE dependency, so it runs in host tests. The web
// chart carries a hand-kept JS port (see
// web-src/app/extraction/robust-flow.js); keep the two in sync.
namespace pump_scale {

// Half-width of the centred difference window. When the nearest sample to a
// window edge is farther than this from where the edge should be, the stream
// has a gap there, and flow is reported as not estimable rather than measured
// across the silence.
inline constexpr uint32_t kFlowHalfWindowMs = 500;
// Use scale time only when its elapsed interval stays close to host-arrival
// time. This accepts normal BLE jitter while rejecting timer resets, stale
// repeated timer samples, or mixed clock intervals that would make flow choppy.
inline constexpr uint32_t kFlowScaleTimerDriftToleranceMs = 250;

// Find the sample index whose tMs is nearest to `target`. Linear scan from
// `startHint`; samples are time-ordered ascending. Returns -1 if count is 0.
inline int findNearestSample(const Sample* samples, size_t count,
                             uint32_t target, int startHint) {
  int best = -1;
  uint32_t bestDelta = UINT32_MAX;
  const int n = static_cast<int>(count);
  int i = startHint > 0 ? startHint : 0;
  for (; i < n; ++i) {
    const uint32_t t = samples[i].tMs;
    const uint32_t d = t > target ? t - target : target - t;
    if (d < bestDelta) {
      bestDelta = d;
      best = i;
    }
    if (t > target && bestDelta < UINT32_MAX) {
      // We've passed target; bestDelta only grows from here.
      break;
    }
  }
  return best;
}

// Time spanned by a before/after pair, in milliseconds. Prefers the scale's own
// timer when both samples carry it and its interval agrees with host-arrival
// time; otherwise uses host-arrival time.
inline uint32_t flowDeltaMs(const Sample& before, const Sample& after) {
  const uint32_t hostDt = after.tMs - before.tMs;
  if (sampleHasScaleTimer(before) && sampleHasScaleTimer(after) &&
      after.scaleTimerMs > before.scaleTimerMs) {
    const uint32_t scaleDt = after.scaleTimerMs - before.scaleTimerMs;
    const uint32_t drift =
        scaleDt > hostDt ? scaleDt - hostDt : hostDt - scaleDt;
    if (drift <= kFlowScaleTimerDriftToleranceMs) return scaleDt;
  }
  return hostDt;
}

// True if any adjacent sample edge in (iBefore, iAfter] is a transient break
// (the centred window crosses a bump, tare, or cup lift), so its endpoint
// difference would measure the transient rather than local flow.
// isFlowBreakEdge covers both a single large step and a steep multi-step
// collapse. Each edge's dt comes from flowDeltaMs (the scale's own timer when
// trustworthy) so BLE host-arrival jitter can't shrink dt and turn a benign
// step into a spurious steep break.
inline bool windowCrossesBreak(const Sample* samples, int iBefore, int iAfter) {
  for (int k = iBefore + 1; k <= iAfter; ++k) {
    const int32_t rawStep =
        static_cast<int32_t>(samples[k].cg) - samples[k - 1].cg;
    const uint32_t dt = flowDeltaMs(samples[k - 1], samples[k]);
    if (isFlowBreakEdge(rawStep, dt)) return true;
  }
  return false;
}

// Bump-robust centred finite-difference flow rate (g/s) at sample i. Returns
// false — meaning "no flow here", which renderers draw as a line break and
// statistics skip — when:
//   - i is out of range, or the ±window neighbours don't exist;
//   - a neighbour is more than a half-window from its target (a scale-out gap
//     straddles the neighbourhood, so we'd measure flow across the gap);
//   - the window straddles a non-pour transient (a bump/tare/lift edge).
//
// `beforeHint` is threaded across successive calls so the before-side search is
// amortised O(1) as i advances (sample timestamps are monotonic, so the
// "before" target only moves forward). On return it is updated to the before
// index found.
inline bool robustSampleFlow(const Sample* samples, size_t count, size_t i,
                             int& beforeHint, float& outGPerS) {
  if (i >= count) return false;
  const uint32_t tCenter = samples[i].tMs;
  const uint32_t tBefore =
      tCenter > kFlowHalfWindowMs ? tCenter - kFlowHalfWindowMs : 0;
  const uint32_t tAfter = tCenter + kFlowHalfWindowMs;

  const int iBefore = findNearestSample(samples, count, tBefore, beforeHint);
  const int iAfter =
      findNearestSample(samples, count, tAfter, static_cast<int>(i));
  if (iBefore < 0 || iAfter < 0 || iBefore == iAfter) return false;

  // Reject if the "nearest" samples are too far from their targets — i.e., a
  // long scale-out gap straddles the neighbourhood and we'd be measuring flow
  // across the gap rather than locally.
  const uint32_t tBefAct = samples[iBefore].tMs;
  const uint32_t tAftAct = samples[iAfter].tMs;
  const uint32_t bMiss =
      tBefAct > tBefore ? tBefAct - tBefore : tBefore - tBefAct;
  const uint32_t aMiss = tAftAct > tAfter ? tAftAct - tAfter : tAfter - tAftAct;
  if (bMiss > kFlowHalfWindowMs || aMiss > kFlowHalfWindowMs) return false;

  if (windowCrossesBreak(samples, iBefore, iAfter)) return false;

  const uint32_t dt = flowDeltaMs(samples[iBefore], samples[iAfter]);
  if (dt == 0) return false;
  const float dg = (samples[iAfter].cg - samples[iBefore].cg) / 100.0f;
  outGPerS = dg * 1000.0f / static_cast<float>(dt);
  beforeHint = iBefore;
  return true;
}

// Sustained-peak hold window. The peak statistic is the highest flow the shot
// held continuously for at least this long, so a lone noisy sample can't set
// the headline number and the value is repeatable shot-to-shot (what an
// operator dialing grind against it depends on). 700 ms is chosen deliberately:
// the centred ±500 ms difference smears a single noisy weight sample across
// roughly a 450 ms span of the flow series, so the hold must exceed that to
// reject it. A real espresso flow plateau lasts well over a second, so 700 ms
// still lands on it. (Empirically, against simulated scale noise, 700 ms is
// near-unbiased where 400 ms still reads ~15% high.)
inline constexpr uint32_t kPeakHoldMs = 700;

// Summary statistics reduced over the robust flow series. The home for flow
// metrics; extend as diagnostics need them (time-to-peak, mean, ...).
struct FlowSeriesStats {
  bool have = false;                // was a sustained flow reading found?
  float sustainedPeakGPerS = 0.0f;  // highest flow held >= kPeakHoldMs (g/s)
};

// Pour evidence derived at finalize from the stored samples: total pour
// duration, summed pour gain, and the weight on the scale when the first pour
// began (the zero for yield). This is what the persisted record and the shot
// verdict use.
struct PourEvidence {
  uint32_t pourMs = 0;  // summed duration of the pour runs
  // Summed gain of the pour runs, no per-run floor. Each run's gain is its
  // peak weight minus its start weight (see computePourEvidence), so a cup
  // lift near the end of a run does not walk it back down.
  int32_t gainCg = 0;
  bool hasPour = false;  // did any pour run form?
  bool hasStartRawCg = false;
  int16_t startRawCg = 0;  // last flat weight before the first pour run
};

// Find the last trustworthy endpoint before the first material weight change
// after the final pump-off. Continued dripping changes weight gradually; a
// material step instead indicates that the cup or scale was moved. The final
// raw reading remains available separately for diagnosis.
inline bool endpointBeforePostPumpDiscontinuity(const Sample* samples,
                                                size_t count,
                                                uint32_t pumpOffConfirmedMs,
                                                int16_t& outCg) {
  if (count < 2 || pumpOffConfirmedMs == 0) return false;

  for (size_t i = 1; i < count; ++i) {
    if (static_cast<int32_t>(samples[i].tMs - pumpOffConfirmedMs) < 0) continue;
    const int32_t rawStep =
        static_cast<int32_t>(samples[i].cg) - samples[i - 1].cg;
    const uint32_t dt = flowDeltaMs(samples[i - 1], samples[i]);
    // Deliberately use the raw-step threshold, not isFlowBreakEdge: a fast
    // sub-gram tail fluctuation is a flow-series break but not material cup or
    // scale movement.
    if (isNonPourStep(rawStep, dt)) {
      outCg = samples[i - 1].cg;
      return true;
    }
  }
  return false;
}

// Reduce the robust flow series into a sustained peak: the largest flow level
// the weight held continuously for kPeakHoldMs. Computed as the maximum, over
// the series, of the MINIMUM flow across each trailing hold-window — taking the
// window minimum is what makes a brief spike unable to lift the result, so the
// number is repeatable shot-to-shot (the property an operator dialing grind
// against it depends on). A non-estimable sample (a transient or dropout)
// breaks the run, so a window never spans a bump. Single pass with a small ring
// of the in-window samples, so no per-shot heap and no large stack buffer.
inline FlowSeriesStats computeFlowSeriesStats(const Sample* samples,
                                              size_t count) {
  FlowSeriesStats stats;
  constexpr int kCap = 32;  // bounds the hold window at any realistic cadence
  uint32_t winT[kCap];
  float winF[kCap];
  int head = 0, n = 0;  // ring of in-window samples: n entries from head
  bool runValid = false;
  uint32_t runStartT = 0;

  int beforeHint = 0;
  for (size_t i = 0; i < count; ++i) {
    float f;
    if (!robustSampleFlow(samples, count, i, beforeHint, f)) {
      head = 0;
      n = 0;
      runValid = false;  // a gap breaks the sustained run
      continue;
    }
    const uint32_t t = samples[i].tMs;
    if (!runValid) {
      runValid = true;
      runStartT = t;
    }
    // Drop samples that fell out of the trailing hold window. The cutoff is
    // clamped at 0 so the unsigned subtraction can't wrap when t < kPeakHoldMs;
    // that early it simply evicts nothing (winT is never negative).
    const uint32_t cutoff = t >= kPeakHoldMs ? t - kPeakHoldMs : 0;
    while (n > 0 && winT[head] < cutoff) {
      head = (head + 1) % kCap;
      --n;
    }
    if (n == kCap) {  // guard; time eviction keeps n small in practice
      head = (head + 1) % kCap;
      --n;
    }
    const int tail = (head + n) % kCap;
    winT[tail] = t;
    winF[tail] = f;
    ++n;
    // Only score once the run has covered a full hold window behind this
    // sample. Tested directly (t - runStartT) rather than via the clamped
    // cutoff above: when t < kPeakHoldMs that cutoff floors to 0 and `runStartT
    // <= cutoff` would wrongly fire for a run starting at t==0, scoring a
    // partial window. t >= runStartT within a run, so the subtraction does not
    // wrap.
    if (t - runStartT >= kPeakHoldMs) {
      float windowMin = winF[head];
      for (int k = 1; k < n; ++k) {
        const float v = winF[(head + k) % kCap];
        if (v < windowMin) windowMin = v;
      }
      if (!stats.have || windowMin > stats.sustainedPeakGPerS) {
        stats.sustainedPeakGPerS = windowMin;
        stats.have = true;
      }
    }
  }
  return stats;
}

inline PourEvidence computePourEvidence(const Sample* samples, size_t count) {
  PourEvidence evidence;

  // Evidence comes from re-processing the stored samples, in order, through
  // the same per-sample classifier live tracking uses. The centred robust
  // flow series above stays the right tool for charts and flow statistics,
  // but here it would smear the pour onset backward and erase the last flat
  // sample that serves as the pour-start weight.
  CausalPourClassifier classifier;

  bool inRun = false;
  uint8_t outRun = 0;
  uint8_t settleRun = 0;
  uint32_t startMs = 0;
  int16_t startCg = 0;
  uint32_t activeEndMs = 0;
  // The run's gain is its peak weight minus its start, not its terminal weight
  // minus its start. Coffee only ever adds weight to the cup, so a weight drop
  // inside a run is never real pour — it is a cup lift, a suck-back, or noise.
  // Tracking the peak keeps a lift that collapses through the Settled/Tail
  // bands, before the run-break bands close the run, from walking the recorded
  // gain down the collapse. The peak advances only on trusted samples, so an
  // untrusted upward spike (a knock) cannot ratchet it up either.
  int16_t peakCg = 0;

  auto closeRun = [&]() {
    const uint32_t dur = activeEndMs - startMs;
    const int32_t gain = static_cast<int32_t>(peakCg) - startCg;
    evidence.gainCg += gain;
    evidence.pourMs += dur;
    inRun = false;
    outRun = 0;
    settleRun = 0;
  };

  // Raise the run's peak to this sample's weight if the sample is trusted and
  // heavier than the peak so far.
  auto liftPeak = [&](const CausalPourSample& s) {
    if (s.trustedStep && s.rawCg > peakCg) peakCg = s.rawCg;
  };

  for (size_t i = 0; i < count; ++i) {
    const CausalPourSample s = classifier.update(samples[i].tMs, samples[i].cg);
    if (!s.accepted) continue;

    if (!inRun) {
      if (isPourBand(s.band) && s.trustedStep) {
        inRun = true;
        startMs = s.tMs;
        startCg = s.pourStartCandidateCg;
        if (!evidence.hasStartRawCg) {
          evidence.hasStartRawCg = true;
          evidence.startRawCg = startCg;
        }
        activeEndMs = s.tMs;
        peakCg = s.rawCg;
        outRun = 0;
        settleRun = 0;
        evidence.hasPour = true;
      }
    } else if (isPourBand(s.band)) {
      activeEndMs = s.tMs;
      liftPeak(s);
      outRun = 0;
      settleRun = 0;
    } else if (isRunBreakBand(s.band)) {
      settleRun = 0;
      if (++outRun >= kRunCloseSamples) closeRun();
    } else {
      liftPeak(s);
      outRun = 0;
      if (isSettledBand(s.band)) {
        if (++settleRun >= kRunCloseSamples) closeRun();
      } else {
        settleRun = 0;
      }
    }
  }

  if (inRun) closeRun();
  return evidence;
}

}  // namespace pump_scale
