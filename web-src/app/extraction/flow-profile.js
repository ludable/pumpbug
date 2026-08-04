// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

const FlowProfile = (() => {
  const DEFAULT_THRESHOLDS = Object.freeze({
    minSamples: 10,
    minYieldCg: 1500,
    minActiveMs: 2000,
    bucketCount: 5,
    // Fixed thresholds keep historical profiles comparable. Change them only
    // when intentionally changing how flow profiles are classified.
    slowCgPerS: 230,
    fastCgPerS: 460,
    frontBalancePermille: 470,
    lateBalancePermille: 620,
    frontLateRatio: 0.78,
    lateEarlyRatio: 1.25,
    unevenPeakToMedian: 1.45,
    unevenCv: 0.42,
    unevenBucketSpread: 0.50,
  });

  const TEMPO_LABEL = {
    slow: 'Slow flow',
    steady: 'Steady flow',
    fast: 'Fast flow',
  };
  const SHAPE_LABEL = {
    steady: '',
    'late-rise': 'late rise',
    'early-surge': 'early surge',
    uneven: 'uneven',
  };

  function thresholds(overrides) {
    return { ...DEFAULT_THRESHOLDS, ...(overrides || {}) };
  }

  function hasWeight(v) {
    return Number.isFinite(v) && v !== NO_WEIGHT_CG;
  }

  function sampleYieldCg(extraction, sample) {
    const startRawCg = extraction && hasWeight(extraction.startRawCg)
      ? extraction.startRawCg
      : null;
    if (Number.isFinite(sample.cg)) {
      return startRawCg == null ? sample.cg : sample.cg - startRawCg;
    }
    if (Number.isFinite(sample.rawCg)) {
      return startRawCg == null ? sample.rawCg : sample.rawCg - startRawCg;
    }
    if (Number.isFinite(sample.yieldCg)) return sample.yieldCg;
    return null;
  }

  function buildYieldPoints(extraction) {
    const samples = extraction && Array.isArray(extraction.samples)
      ? extraction.samples
      : [];
    const pts = [];
    for (const sample of samples) {
      if (!Number.isFinite(sample.tMs)) continue;
      const cg = sampleYieldCg(extraction, sample);
      if (!Number.isFinite(cg)) continue;
      pts.push({
        tMs: sample.tMs,
        cg,
        g: cg / 100,
        scaleTimerMs: Number.isFinite(sample.scaleTimerMs)
          ? sample.scaleTimerMs
          : SCALE_TIMER_UNKNOWN_MS,
      });
    }
    pts.sort((a, b) => a.tMs - b.tMs);
    return pts;
  }

  function runningMaxYieldCg(pts) {
    let maxCg = 0;
    for (const pt of pts) maxCg = Math.max(maxCg, Math.round(pt.cg));
    return maxCg;
  }

  function finalYieldCg(extraction, pts) {
    if (hasWeight(extraction && extraction.yieldCg) && extraction.yieldCg > 0) {
      return extraction.yieldCg;
    }
    // decisionGainCg excludes abrupt weight changes. The sample maximum does
    // not, so a cup lift or scale-mode transition can make it much larger than
    // the amount poured.
    if (Number.isFinite(extraction && extraction.decisionGainCg) &&
        extraction.decisionGainCg > 0) {
      return extraction.decisionGainCg;
    }
    return runningMaxYieldCg(pts);
  }

  function crossingTimeMs(pts, targetCg) {
    if (!pts.length || !Number.isFinite(targetCg)) return null;
    let prevT = pts[0].tMs;
    let prevCg = Math.max(0, Math.round(pts[0].cg));
    if (prevCg >= targetCg) return prevT;
    for (let i = 1; i < pts.length; i++) {
      const curRawCg = Math.max(0, Math.round(pts[i].cg));
      const curCg = Math.max(prevCg, curRawCg);
      if (curCg >= targetCg) {
        if (curCg === prevCg) return pts[i].tMs;
        const frac = (targetCg - prevCg) / (curCg - prevCg);
        return prevT + frac * (pts[i].tMs - prevT);
      }
      prevT = pts[i].tMs;
      prevCg = curCg;
    }
    return null;
  }

  function computeFlowSamples(pts, startMs, endMs) {
    if (typeof RobustFlow === 'undefined' || !RobustFlow.sampleFlow) return [];
    const out = [];
    const hint = { before: 0 };
    for (let i = 0; i < pts.length; i++) {
      const pt = pts[i];
      const gps = RobustFlow.sampleFlow(pts, i, hint);
      if (gps === null) continue;
      if (pt.tMs < startMs || pt.tMs > endMs) continue;
      out.push({ tMs: pt.tMs, gps });
    }
    return out;
  }

  function median(values) {
    if (!values.length) return null;
    const xs = values.slice().sort((a, b) => a - b);
    const mid = xs.length >> 1;
    return xs.length & 1 ? xs[mid] : (xs[mid - 1] + xs[mid]) / 2;
  }

  function percentile(values, p) {
    if (!values.length) return null;
    const xs = values.slice().sort((a, b) => a - b);
    const i = Math.max(0, Math.min(xs.length - 1,
      Math.ceil((p / 100) * xs.length) - 1));
    return xs[i];
  }

  function mean(values) {
    if (!values.length) return null;
    let sum = 0;
    for (const v of values) sum += v;
    return sum / values.length;
  }

  function stddev(values, avg) {
    if (values.length < 2 || !Number.isFinite(avg)) return 0;
    let sum = 0;
    for (const v of values) {
      const d = v - avg;
      sum += d * d;
    }
    return Math.sqrt(sum / values.length);
  }

  function bucketFlow(flowSamples, startMs, endMs, count) {
    const buckets = Array.from({ length: count }, () => []);
    const span = endMs - startMs;
    if (span <= 0) return buckets.map(() => null);
    for (const sample of flowSamples) {
      const gps = Math.max(0, sample.gps);
      if (gps <= 0.05) continue;
      const frac = (sample.tMs - startMs) / span;
      const idx = Math.max(0, Math.min(count - 1, Math.floor(frac * count)));
      buckets[idx].push(gps);
    }
    return buckets.map((b) => median(b));
  }

  function fillBuckets(values, fallback) {
    const out = values.slice();
    for (let i = 0; i < out.length; i++) {
      if (Number.isFinite(out[i])) continue;
      let left = i - 1;
      while (left >= 0 && !Number.isFinite(out[left])) left--;
      let right = i + 1;
      while (right < out.length && !Number.isFinite(out[right])) right++;
      if (left >= 0 && right < out.length) {
        const frac = (i - left) / (right - left);
        out[i] = out[left] + (out[right] - out[left]) * frac;
      } else if (left >= 0) {
        out[i] = out[left];
      } else if (right < out.length) {
        out[i] = out[right];
      } else {
        out[i] = fallback;
      }
    }
    return out;
  }

  function tempoBand(tempoCgPerS, t) {
    if (tempoCgPerS < t.slowCgPerS) return 'slow';
    if (tempoCgPerS > t.fastCgPerS) return 'fast';
    return 'steady';
  }

  function classifyShape(stats, t) {
    const uneven = stats.flowCount >= 6 &&
      (stats.peakToMedian >= t.unevenPeakToMedian ||
       stats.flowCv >= t.unevenCv ||
       stats.bucketSpread >= t.unevenBucketSpread);
    const late = stats.balancePermille >= t.lateBalancePermille ||
      (stats.lateEarlyRatio >= t.lateEarlyRatio &&
       stats.balancePermille >= 540);
    const front = stats.balancePermille <= t.frontBalancePermille ||
      (stats.lateEarlyRatio <= t.frontLateRatio &&
       stats.balancePermille <= 530);
    if (uneven && (late || front)) return 'uneven';
    if (uneven) return 'uneven';
    if (late && !front) return 'late-rise';
    if (front && !late) return 'early-surge';
    return 'steady';
  }

  function compute(extraction, overrides) {
    const t = thresholds(overrides);
    const pts = buildYieldPoints(extraction);
    const invalid = (reason) => ({
      valid: false,
      reason,
      extraction,
      points: pts,
      thresholds: t,
    });
    if (pts.length < t.minSamples) return invalid('too few samples');

    const yieldCg = finalYieldCg(extraction, pts);
    if (!Number.isFinite(yieldCg) || yieldCg < t.minYieldCg) {
      return invalid('yield too small');
    }

    const t5 = crossingTimeMs(pts, yieldCg * 0.05);
    const t25 = crossingTimeMs(pts, yieldCg * 0.25);
    const t50 = crossingTimeMs(pts, yieldCg * 0.50);
    const t75 = crossingTimeMs(pts, yieldCg * 0.75);
    const t95 = crossingTimeMs(pts, yieldCg * 0.95);
    if (![t5, t25, t50, t75, t95].every(Number.isFinite)) {
      return invalid('incomplete yield crossings');
    }

    const activeMs = t95 - t5;
    if (activeMs < t.minActiveMs) return invalid('active window too short');

    const tempoCgPerS = yieldCg * 900 / activeMs;
    const balancePermille = Math.round((t50 - t5) * 1000 / activeMs);
    const flowSamples = computeFlowSamples(pts, t5, t95);
    const positiveFlow = flowSamples
      .map((s) => Math.max(0, s.gps))
      .filter((v) => v > 0.05);
    const medFlow = median(positiveFlow) || tempoCgPerS / 100;
    const p95Flow = percentile(positiveFlow, 95) || medFlow;
    const avgFlow = mean(positiveFlow) || medFlow;
    const flowCv = avgFlow > 0 ? stddev(positiveFlow, avgFlow) / avgFlow : 0;
    const rawBuckets = bucketFlow(flowSamples, t5, t95, t.bucketCount);
    const fallbackGps = tempoCgPerS / 100;
    const buckets = fillBuckets(rawBuckets, fallbackGps);
    const early = median(buckets.slice(0, 2)) || fallbackGps;
    const late = median(buckets.slice(-2)) || fallbackGps;
    const lateEarlyRatio = early > 0.05 ? late / early : (late > 0.05 ? Infinity : 1);
    const maxBucket = Math.max(...buckets);
    const minBucket = Math.min(...buckets);
    const bucketSpread = maxBucket > 0 ? (maxBucket - minBucket) / maxBucket : 0;
    const peakToMedian = medFlow > 0 ? p95Flow / medFlow : 1;

    const band = tempoBand(tempoCgPerS, t);
    const stats = {
      flowCount: positiveFlow.length,
      balancePermille,
      lateEarlyRatio,
      bucketSpread,
      peakToMedian,
      flowCv,
    };
    const shape = classifyShape(stats, t);
    const label = shape === 'steady'
      ? TEMPO_LABEL[band]
      : TEMPO_LABEL[band] + ' - ' + SHAPE_LABEL[shape];

    const iconLevels = canonicalLevels(band, shape);
    const iconScaleGps = Math.max(1, t.fastCgPerS / 100 * 1.4);
    const bucketLevels = buckets.map((v) => clamp(v / iconScaleGps, 0.08, 0.95));

    return {
      valid: true,
      extraction,
      points: pts,
      thresholds: t,
      yieldCg,
      t5Ms: t5,
      t25Ms: t25,
      t50Ms: t50,
      t75Ms: t75,
      t95Ms: t95,
      activeMs,
      tempoCgPerS,
      tempoBand: band,
      shape,
      profileKey: band + (shape === 'steady' ? '' : '-' + shape),
      label,
      balancePermille,
      lateEarlyRatio,
      peakToMedian,
      flowCv,
      bucketSpread,
      flowSamples,
      bucketGps: buckets,
      bucketLevels,
      iconLevels,
    };
  }

  function canonicalLevels(band, shape) {
    const high = band === 'slow' ? 0.34 : (band === 'fast' ? 0.82 : 0.56);
    const low = Math.max(0.12, high - 0.30);
    const mid = (low + high) / 2;
    if (shape === 'late-rise') return [low, low + 0.04, mid, high - 0.04, high];
    if (shape === 'early-surge') return [high, high - 0.04, mid, low + 0.04, low];
    if (shape === 'uneven') {
      return [
        clamp(high * 0.70, 0.08, 0.95),
        clamp(high * 1.12, 0.08, 0.95),
        clamp(high * 0.62, 0.08, 0.95),
        clamp(high * 1.00, 0.08, 0.95),
        clamp(high * 0.76, 0.08, 0.95),
      ];
    }
    return [high, high, high, high, high];
  }

  function clamp(v, lo, hi) {
    return Math.max(lo, Math.min(hi, v));
  }

  function levelsToLinePath(levels, width, height, pad) {
    const plotW = width - pad * 2;
    const plotH = height - pad * 2;
    const parts = [];
    for (let i = 0; i < levels.length; i++) {
      const x = pad + (levels.length === 1 ? 0 : i / (levels.length - 1) * plotW);
      const y = height - pad - clamp(levels[i], 0, 1) * plotH;
      parts.push((i === 0 ? 'M' : 'L') + x.toFixed(1) + ' ' + y.toFixed(1));
    }
    return parts.join('');
  }

  function levelsToAreaPath(levels, width, height, pad) {
    const line = levelsToLinePath(levels, width, height, pad);
    if (!line) return '';
    const baseline = height - pad;
    return line + 'L' + (width - pad).toFixed(1) + ' ' + baseline.toFixed(1) +
      'L' + pad.toFixed(1) + ' ' + baseline.toFixed(1) + 'Z';
  }

  function iconSvg(profile, opts) {
    opts = opts || {};
    const width = opts.width || 64;
    const height = opts.height || 36;
    const pad = opts.pad == null ? 4 : opts.pad;
    const levels = opts.levels || (profile && profile.iconLevels) || [0.5, 0.5];
    const cls = opts.className || 'flow-profile-glyph';
    return '<svg class="' + cls + '" viewBox="0 0 ' + width + ' ' + height +
      '" role="img" aria-label="' + escapeAttr(profile && profile.label || 'flow profile') + '">' +
      '<path class="flow-profile-glyph-fill" d="' +
      levelsToAreaPath(levels, width, height, pad) + '"></path>' +
      '<path class="flow-profile-glyph-line" d="' +
      levelsToLinePath(levels, width, height, pad) + '"></path>' +
      '</svg>';
  }

  function renderIcon(profile, opts) {
    const wrap = document.createElement('span');
    wrap.innerHTML = iconSvg(profile, opts);
    return wrap.firstElementChild;
  }

  function escapeAttr(s) {
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/"/g, '&quot;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  return {
    DEFAULT_THRESHOLDS,
    compute,
    buildYieldPoints,
    canonicalLevels,
    iconSvg,
    renderIcon,
  };
})();
