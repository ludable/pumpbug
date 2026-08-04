// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// RobustFlow — the web-side port of src/apps/extraction/RobustFlow.h (which in
// turn shares its transient classifier with FlowBands.h). Since the wire carries
// only raw weight, the web re-derives flow itself; this is the one definition it
// uses, so the chart, flow statistics, and any future diagnostics all agree —
// including the rejection of placement bumps, tares, and cup lifts that would
// otherwise render as spurious flow.
//
// Keep in sync with RobustFlow.h / FlowBands.h. Operates on the chart's `pts`
// array: objects { tMs, g (grams, baseline-relative), scaleTimerMs }.
const RobustFlow = (() => {
  // --- FlowBands.h vocabulary ---
  const K_CEILING_CG_PER_S = 1000.0;      // 10 g/s
  const K_DISCONTINUITY_CG_PER_S = -500.0; // -5 g/s
  const K_STEP_DISCONTINUITY_CG = 300;    // 3 g
  const K_STEP_MARGIN_CG = 150;           // 1.5 g
  const K_MAX_SAMPLE_GAP_MS = 750;
  // --- RobustFlow.h window ---
  const K_FLOW_HALF_WINDOW_MS = 500;
  const K_FLOW_SCALE_TIMER_DRIFT_TOLERANCE_MS = 250;

  // Largest upward single-sample step (centigrams) that could still be a pour
  // over the given inter-sample gap. A jump past this is a placement/knock.
  function positiveStepLimitCg(dtMs) {
    const stepDtMs = dtMs < K_MAX_SAMPLE_GAP_MS ? dtMs : K_MAX_SAMPLE_GAP_MS;
    return Math.trunc(K_CEILING_CG_PER_S * stepDtMs / 1000) + K_STEP_MARGIN_CG;
  }

  // A single-sample step too big to be a pour in either direction.
  function isNonPourStep(rawStepCg, dtMs) {
    return rawStepCg <= -K_STEP_DISCONTINUITY_CG ||
           rawStepCg >= positiveStepLimitCg(dtMs);
  }

  // An adjacent-sample drop steeper than any pour tail (rate past the
  // discontinuity slope). Complements isNonPourStep's fixed downward magnitude:
  // magnitude catches a slow large drop, this catches a fast small one.
  function isSteepDownwardSlope(rawStepCg, dtMs) {
    if (rawStepCg >= 0 || dtMs === 0) return false;
    return rawStepCg * 1000 <= K_DISCONTINUITY_CG_PER_S * dtMs;
  }

  // A single edge the robust flow series treats as a transient break: a non-pour
  // step OR a steep downward slope. Both halves matter — a collapse can arrive as
  // one big step or a run of smaller fast drops.
  function isFlowBreakEdge(rawStepCg, dtMs) {
    return isNonPourStep(rawStepCg, dtMs) || isSteepDownwardSlope(rawStepCg, dtMs);
  }

  function hasScaleTimer(s) {
    return Number.isFinite(s.scaleTimerMs) &&
           s.scaleTimerMs !== SCALE_TIMER_UNKNOWN_MS;
  }

  // Time spanned by a before/after pair (ms), preferring the scale timer when it
  // agrees with host-arrival time.
  function flowDeltaMs(before, after) {
    const hostDt = after.tMs - before.tMs;
    if (hasScaleTimer(before) && hasScaleTimer(after) &&
        after.scaleTimerMs > before.scaleTimerMs) {
      const scaleDt = after.scaleTimerMs - before.scaleTimerMs;
      if (Math.abs(scaleDt - hostDt) <= K_FLOW_SCALE_TIMER_DRIFT_TOLERANCE_MS) {
        return scaleDt;
      }
    }
    return hostDt;
  }

  // Sample index whose tMs is nearest to `target`; scans forward from startHint.
  function findNearestSample(pts, target, startHint) {
    let best = -1;
    let bestDelta = Infinity;
    for (let i = Math.max(0, startHint); i < pts.length; i++) {
      const t = pts[i].tMs;
      const d = Math.abs(t - target);
      if (d < bestDelta) { bestDelta = d; best = i; }
      if (t > target && bestDelta < Infinity) break;  // passed target
    }
    return best;
  }

  // True if any adjacent edge in (iBefore, iAfter] is a transient break (a large
  // step or a steep collapse) — the centred window crosses a bump/tare/lift. Raw
  // step is recovered from the grams delta (the constant baseline offset cancels).
  function windowCrossesBreak(pts, iBefore, iAfter) {
    for (let k = iBefore + 1; k <= iAfter; k++) {
      const rawStepCg = Math.round((pts[k].g - pts[k - 1].g) * 100);
      // Scale-timer-aware dt (matches the magnitude path) so BLE host-arrival
      // jitter can't shrink dt and fake a steep break.
      const dtMs = flowDeltaMs(pts[k - 1], pts[k]);
      if (isFlowBreakEdge(rawStepCg, dtMs)) return true;
    }
    return false;
  }

  // Bump-robust centred-difference flow (g/s) at pts[i]. Returns null for "no
  // flow here" (out of range, missing/too-distant neighbours, or a window that
  // straddles a transient). `hint` is { before } and is advanced in place so a
  // forward sweep stays amortised O(1).
  function sampleFlow(pts, i, hint) {
    if (i < 0 || i >= pts.length) return null;
    const tCenter = pts[i].tMs;
    const tBefore = tCenter > K_FLOW_HALF_WINDOW_MS
      ? tCenter - K_FLOW_HALF_WINDOW_MS : 0;
    const tAfter = tCenter + K_FLOW_HALF_WINDOW_MS;

    const iBefore = findNearestSample(pts, tBefore, hint.before);
    const iAfter = findNearestSample(pts, tAfter, i);
    if (iBefore < 0 || iAfter < 0 || iBefore === iAfter) return null;

    const bMiss = Math.abs(pts[iBefore].tMs - tBefore);
    const aMiss = Math.abs(pts[iAfter].tMs - tAfter);
    if (bMiss > K_FLOW_HALF_WINDOW_MS || aMiss > K_FLOW_HALF_WINDOW_MS) return null;

    if (windowCrossesBreak(pts, iBefore, iAfter)) return null;

    const dtMs = flowDeltaMs(pts[iBefore], pts[iAfter]);
    if (dtMs <= 0) return null;
    hint.before = iBefore;
    return (pts[iAfter].g - pts[iBefore].g) * 1000 / dtMs;
  }

  // Sustained-peak hold window: the highest flow held continuously for at least
  // this long, so a brief spike can't set the headline number and the value is
  // repeatable shot-to-shot. 700 ms exceeds the ~450 ms span over which the
  // centred difference smears a single noisy sample, while still landing on a
  // real (multi-second) flow plateau. Keep in sync with kPeakHoldMs in
  // RobustFlow.h.
  const K_PEAK_HOLD_MS = 700;

  // Reduce the robust series into a sustained peak: the maximum, over the series,
  // of the minimum flow across each trailing hold-window. Taking the window
  // minimum is what rejects brief spikes. A non-estimable sample breaks the run,
  // so a window never spans a bump.
  function computeFlowSeriesStats(pts) {
    const hint = { before: 0 };
    const win = [];  // in-window samples: { t, f }
    let runStartT = null;
    let have = false;
    let sustainedPeakGPerS = 0;
    for (let i = 0; i < pts.length; i++) {
      const f = sampleFlow(pts, i, hint);
      if (f === null) { win.length = 0; runStartT = null; continue; }
      const t = pts[i].tMs;
      if (runStartT === null) runStartT = t;
      while (win.length && win[0].t < t - K_PEAK_HOLD_MS) win.shift();
      win.push({ t, f });
      // Score only once the run has covered a full hold window (mirrors
      // RobustFlow.h: t - runStartT >= kPeakHoldMs).
      if (t - runStartT >= K_PEAK_HOLD_MS) {
        let windowMin = win[0].f;
        for (let k = 1; k < win.length; k++) {
          if (win[k].f < windowMin) windowMin = win[k].f;
        }
        if (!have || windowMin > sustainedPeakGPerS) {
          sustainedPeakGPerS = windowMin;
          have = true;
        }
      }
    }
    return { have, sustainedPeakGPerS };
  }

  return {
    sampleFlow,
    computeFlowSeriesStats,
    isNonPourStep,
    positiveStepLimitCg,
  };
})();

if (typeof module !== 'undefined' && module.exports) {
  module.exports = RobustFlow;
}
