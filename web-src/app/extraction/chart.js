// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// One chart instance owns one DOM subtree; createChart() builds it lazily on
// the first mount(). Instances are independent, so two charts can be on
// screen at once (the live curve and an expanded Last-shot card).
function createChart() {
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const FLOOR_MS = 15000;
  const MAX_CHART_SPAN_MS = 180000;
  const MAX_X_TICKS = 24;
  const EVENT_PUMP_ON = 2;
  const EVENT_PUMP_OFF_CONFIRMED = 3;
  const GAP_MS = 500;
  const FLOW_VISIBLE_THRESHOLD = 0.05;
  const PAD_L = 36, PAD_R = 36, PAD_T = 12, PAD_B = 22;

  let root = null;
  let svg = null;
  let overlay = null;
  let e = {};            // cached element refs
  let dims = { w: 0, h: 0 };
  let pendingFrame = 0;
  let latest = { extraction: null, nowMs: 0 };
  let resizeObs = null;

  // Pointer state.
  // scrub: active drag/pin gesture { pointerId, pinnedTms }
  // hover: mouse hover with no button (independent of scrub)
  let scrub = null;
  let hover = null;
  let windowHandlers = null;

  function svgEl(tag, attrs) {
    const n = document.createElementNS(SVG_NS, tag);
    if (attrs) for (const k in attrs) n.setAttribute(k, attrs[k]);
    return n;
  }
  function htmlEl(tag, cls) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    return n;
  }

  function niceCeilGrams(v) {
    if (v <= 5) return 5;
    if (v <= 10) return 10;
    if (v <= 25) return 25;
    if (v <= 50) return 50;
    return Math.ceil(v / 50) * 50;
  }
  function niceFloorGrams(v) {
    if (v >= -1) return -1;
    if (v >= -2) return -2;
    if (v >= -5) return -5;
    if (v >= -10) return -10;
    if (v >= -25) return -25;
    if (v >= -50) return -50;
    return Math.floor(v / 50) * 50;
  }
  function chooseYStep(range) {
    const steps = [1, 2, 5, 10, 25, 50];
    for (const s of steps) {
      const n = range / s;
      if (n >= 3 && n <= 8) return s;
    }
    return steps[steps.length - 1];
  }

  function ensureRoot() {
    if (root) return;
    root = htmlEl('div', 'chart');

    svg = svgEl('svg', { preserveAspectRatio: 'none' });
    svg.style.display = 'block';
    svg.style.width = '100%';
    svg.style.height = '100%';

    e.gridY = svgEl('g', { class: 'chart-grid' });
    e.gridX = svgEl('g', { class: 'chart-grid' });
    e.weightFill = svgEl('path', { class: 'chart-weight-fill' });
    e.zero = svgEl('line', { class: 'chart-zero' });
    e.flow = svgEl('path', { class: 'chart-flow' });
    e.weight = svgEl('path', { class: 'chart-weight' });
    e.annot = svgEl('g', { class: 'chart-annot' });
    e.crosshair = svgEl('line', { class: 'chart-crosshair', style: 'display:none' });
    e.hit = svgEl('rect', { class: 'chart-hit', fill: 'transparent' });

    svg.append(e.gridY, e.gridX, e.weightFill, e.zero, e.flow, e.weight, e.annot, e.crosshair, e.hit);

    overlay = htmlEl('div', 'chart-overlay');
    e.xLabels = htmlEl('div', 'chart-xlabels');
    e.yLabels = htmlEl('div', 'chart-ylabels');
    e.yLabelsFlow = htmlEl('div', 'chart-ylabels chart-ylabels-flow');
    e.readout = htmlEl('div', 'chart-readout');
    e.readout.style.display = 'none';
    overlay.append(e.xLabels, e.yLabels, e.yLabelsFlow, e.readout);

    root.append(svg, overlay);

    resizeObs = new ResizeObserver((entries) => {
      const r = entries[0].contentRect;
      dims.w = Math.floor(r.width);
      dims.h = Math.floor(r.height);
      svg.setAttribute('viewBox', `0 0 ${dims.w} ${dims.h}`);
      e.hit.setAttribute('x', PAD_L);
      e.hit.setAttribute('y', PAD_T);
      e.hit.setAttribute('width', Math.max(0, dims.w - PAD_L - PAD_R));
      e.hit.setAttribute('height', Math.max(0, dims.h - PAD_T - PAD_B));
      schedulePaint();
    });
    resizeObs.observe(root);

    attachPointer();
  }

  function mount(parent) {
    ensureRoot();
    if (root.parentElement !== parent) parent.appendChild(root);
  }

  function update(extraction, nowMs) {
    // Skip the repaint when nothing changed — a static record card is
    // re-rendered on every STATE packet, and repainting the full curve each
    // time is pure waste. In-flight records mutate in place, so sampleCount
    // is part of the identity check. Resize and pointer repaints don't pass
    // through here (they call schedulePaint directly).
    if (latest.extraction === extraction && latest.nowMs === nowMs &&
        latest.sampleCount === (extraction ? extraction.sampleCount : 0)) {
      return;
    }
    latest.extraction = extraction;
    latest.nowMs = nowMs;
    latest.sampleCount = extraction ? extraction.sampleCount : 0;
    schedulePaint();
  }

  function schedulePaint() {
    if (pendingFrame) return;
    pendingFrame = requestAnimationFrame(() => {
      pendingFrame = 0;
      paint();
    });
  }

  // ---------- Model derivation ----------

  function bsearchSamples(samples, tMs) {
    let lo = 0, hi = samples.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (samples[mid].tMs < tMs) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }

  function buildModel(extraction, nowMs) {
    const beginMs = extraction ? extraction.beginMs : 0;
    const wStart = beginMs;
    const wEnd = chartEndMs(beginMs, nowMs);
    const samples = extraction && extraction.samples ? extraction.samples : [];
    const startRawCg = extraction && extraction.startRawCg != null
      ? extraction.startRawCg
      : NO_WEIGHT_CG;
    const haveZero = startRawCg !== NO_WEIGHT_CG;
    const n = samples.length;
    const i0 = bsearchSamples(samples, wStart - GAP_MS);

    let maxG = -Infinity, minG = Infinity;

    // Weight segments: contiguous runs of samples, broken on sample gaps.
    const segments = [];
    let curSeg = null;
    let prevPt = null;

    // Sample timeline used for flow computation.
    const pts = [];

    for (let i = i0; i < n; i++) {
      const s = samples[i];
      if (s.tMs > wEnd + GAP_MS) break;

      const g = haveZero ? (s.cg - startRawCg) / 100 : s.cg / 100;

      if (g > maxG) maxG = g;
      if (g < minG) minG = g;

      if (prevPt && (s.tMs - prevPt.tMs) > GAP_MS) {
        if (curSeg && curSeg.length) segments.push(curSeg);
        curSeg = null;
      }
      if (!curSeg) curSeg = [];
      curSeg.push({ tMs: s.tMs, g });
      pts.push({
        tMs: s.tMs,
        g,
        scaleTimerMs: s.scaleTimerMs ?? SCALE_TIMER_UNKNOWN_MS,
      });
      prevPt = { tMs: s.tMs, g };
    }
    if (curSeg && curSeg.length) segments.push(curSeg);

    let yMax, yMin;
    if (isFinite(maxG)) {
      yMax = niceCeilGrams(Math.max(maxG, 0));
      yMin = minG < 0 ? niceFloorGrams(minG) : 0;
    } else {
      yMax = 5; yMin = 0;
    }
    if (yMax === 0) yMax = 5;

    // Flow rate: the bump-robust centred difference from RobustFlow, which
    // returns null wherever the window straddles a placement/tare/lift transient
    // or a scale dropout — drawn as a line break, the same as a sample gap.
    const flowSegs = [];
    let flowCur = null;
    let prevFlowT = null;
    let flowMax = 0, flowMin = 0;
    const flowHint = { before: 0 };

    for (let k = 0; k < pts.length; k++) {
      const cur = pts[k];
      if (cur.tMs < wStart || cur.tMs > wEnd) {
        if (flowCur && flowCur.length) { flowSegs.push(flowCur); flowCur = null; }
        prevFlowT = null;
        continue;
      }
      const gps = RobustFlow.sampleFlow(pts, k, flowHint);
      if (gps === null) {
        if (flowCur && flowCur.length) { flowSegs.push(flowCur); flowCur = null; }
        prevFlowT = null;
        continue;
      }
      if (gps > flowMax) flowMax = gps;
      if (gps < flowMin) flowMin = gps;

      if (prevFlowT && (cur.tMs - prevFlowT) > GAP_MS) {
        if (flowCur && flowCur.length) { flowSegs.push(flowCur); flowCur = null; }
      }
      if (!flowCur) flowCur = [];
      flowCur.push({ tMs: cur.tMs, gps });
      prevFlowT = cur.tMs;
    }
    if (flowCur && flowCur.length) flowSegs.push(flowCur);

    // Anchor the flow trace at the shot origin so it starts from zero
    // alongside the weight curve. The first centred-difference estimate is
    // half a second after beginMs and would otherwise float above the baseline.
    if (flowSegs.length && flowSegs[0].length && flowSegs[0][0].tMs > beginMs) {
      flowSegs[0].unshift({ tMs: beginMs, gps: 0 });
    }

    const hasPosFlow = flowMax > FLOW_VISIBLE_THRESHOLD;
    const hasNegFlow = flowMin < -FLOW_VISIBLE_THRESHOLD;
    const showFlow = hasPosFlow || hasNegFlow;
    const yMaxFlow = hasPosFlow ? Math.max(1, Math.ceil(flowMax * 2) / 2) : 0;
    const yMinFlow = hasNegFlow ? Math.min(-1, Math.floor(flowMin * 2) / 2) : 0;

    const annotations = [];
    if (extraction && extraction.events) {
      let sawLastPumpOff = false;
      for (const event of extraction.events) {
        if (event.kind === EVENT_PUMP_ON) {
          if (event.tMs >= wStart && event.tMs <= wEnd) {
            annotations.push({ type: 'pump-on', tMs: event.tMs });
          }
          continue;
        }
        if (event.kind !== EVENT_PUMP_OFF_CONFIRMED) continue;
        if (event.tMs === extraction.lastPumpOffConfirmedMs) {
          sawLastPumpOff = true;
        }
        if (event.signalDecayOnsetMs != null) {
          if (event.tMs >= wStart && event.signalDecayOnsetMs <= wEnd) {
            annotations.push({
              type: 'pump-off-transition',
              fromMs: Math.max(wStart, event.signalDecayOnsetMs),
              toMs: Math.min(wEnd, event.tMs),
            });
          }
          if (event.signalDecayOnsetMs >= wStart &&
              event.signalDecayOnsetMs <= wEnd) {
            annotations.push({
              type: 'pump-signal-decay-onset',
              tMs: event.signalDecayOnsetMs,
              confirmedMs: event.tMs,
            });
          }
        }
        if (event.tMs >= wStart && event.tMs <= wEnd) {
          annotations.push({
            type: 'pump-off-confirmed',
            tMs: event.tMs,
          });
        }
      }
      if (!sawLastPumpOff && extraction.lastPumpOffConfirmedMs &&
          extraction.lastPumpOffConfirmedMs >= wStart &&
          extraction.lastPumpOffConfirmedMs <= wEnd) {
        annotations.push({
          type: 'pump-off-confirmed',
          tMs: extraction.lastPumpOffConfirmedMs,
        });
      }
    }

    return {
      segments, flowSegs, pts,
      xDomain: [wStart, wEnd],
      yDomain: [yMin, yMax],
      yDomainFlow: [yMinFlow, yMaxFlow],
      showFlow,
      annotations,
      beginMs,
    };
  }

  function chartEndMs(beginMs, requestedEndMs) {
    const minEnd = beginMs + FLOOR_MS;
    const maxEnd = beginMs + MAX_CHART_SPAN_MS;
    if (!Number.isFinite(requestedEndMs)) return minEnd;
    if (requestedEndMs <= beginMs) return minEnd;
    return Math.min(Math.max(requestedEndMs, minEnd), maxEnd);
  }

  // ---------- Paint ----------

  function paint() {
    if (!root || dims.w <= 0 || dims.h <= 0) return;
    if (!latest.extraction) return;

    const m = buildModel(latest.extraction, latest.nowMs);
    const plotW = dims.w - PAD_L - PAD_R;
    const plotH = dims.h - PAD_T - PAD_B;
    if (plotW <= 0 || plotH <= 0) return;

    const [x0, x1] = m.xDomain;
    const [y0, y1] = m.yDomain;
    const [yf0, yf1] = m.yDomainFlow;
    const flowRange = (yf1 - yf0) || 1;
    const xS = (t) => PAD_L + (t - x0) / (x1 - x0) * plotW;
    const yS = (g) => PAD_T + (y1 - g) / (y1 - y0) * plotH;
    const yfS = (gps) => PAD_T + (yf1 - gps) / flowRange * plotH;

    // Weight: fill under curve down to zero (clamped into domain), then stroke.
    const baselineY = yS(Math.max(y0, Math.min(y1, 0)));
    e.weightFill.setAttribute('d', segmentsToFillD(m.segments, xS, yS, baselineY));
    e.weight.setAttribute('d', segmentsToD(m.segments, xS, yS, false));

    // Flow path
    if (m.showFlow) {
      e.flow.setAttribute('d', segmentsToFlowD(m.flowSegs, xS, yfS));
      e.flow.style.display = '';
    } else {
      e.flow.style.display = 'none';
    }

    // Zero line
    if (y0 <= 0 && y1 >= 0) {
      const yz = yS(0);
      e.zero.setAttribute('x1', PAD_L);
      e.zero.setAttribute('x2', PAD_L + plotW);
      e.zero.setAttribute('y1', yz);
      e.zero.setAttribute('y2', yz);
      e.zero.style.display = '';
    } else {
      e.zero.style.display = 'none';
    }

    renderYTicks(m, yS, plotW, plotH);
    renderFlowTicks(m, yfS, plotW);
    renderXTicks(m, xS, plotH);
    renderAnnotations(m, xS, plotH);
    renderCrosshair(m, xS, yS, plotW, plotH);
  }

  function segmentsToD(segs, xS, yS) {
    if (!segs.length) return '';
    const parts = [];
    for (const seg of segs) {
      if (!seg.length) continue;
      parts.push('M' + xS(seg[0].tMs).toFixed(1) + ' ' + yS(seg[0].g).toFixed(1));
      for (let i = 1; i < seg.length; i++) {
        parts.push('L' + xS(seg[i].tMs).toFixed(1) + ' ' + yS(seg[i].g).toFixed(1));
      }
    }
    return parts.join('');
  }
  function segmentsToFillD(segs, xS, yS, baselineY) {
    if (!segs.length) return '';
    const parts = [];
    for (const seg of segs) {
      if (!seg.length) continue;
      const x0 = xS(seg[0].tMs).toFixed(1);
      const xN = xS(seg[seg.length - 1].tMs).toFixed(1);
      const by = baselineY.toFixed(1);
      parts.push('M' + x0 + ' ' + by);
      for (let i = 0; i < seg.length; i++) {
        parts.push('L' + xS(seg[i].tMs).toFixed(1) + ' ' + yS(seg[i].g).toFixed(1));
      }
      parts.push('L' + xN + ' ' + by + 'Z');
    }
    return parts.join('');
  }
  function segmentsToFlowD(segs, xS, yfS) {
    if (!segs.length) return '';
    const parts = [];
    for (const seg of segs) {
      if (!seg.length) continue;
      parts.push('M' + xS(seg[0].tMs).toFixed(1) + ' ' + yfS(seg[0].gps).toFixed(1));
      for (let i = 1; i < seg.length; i++) {
        parts.push('L' + xS(seg[i].tMs).toFixed(1) + ' ' + yfS(seg[i].gps).toFixed(1));
      }
    }
    return parts.join('');
  }

  function renderYTicks(m, yS, plotW, plotH) {
    const [y0, y1] = m.yDomain;
    const step = chooseYStep(y1 - y0);
    const first = Math.ceil(y0 / step) * step;
    const ticks = [];
    for (let v = first; v <= y1 + 1e-6; v += step) ticks.push(Math.round(v));

    // Gridlines (SVG)
    while (e.gridY.firstChild) e.gridY.removeChild(e.gridY.firstChild);
    for (const v of ticks) {
      if (v === 0) continue; // zero line drawn separately
      const y = yS(v);
      const ln = svgEl('line', {
        x1: PAD_L, x2: PAD_L + plotW, y1: y, y2: y, class: 'chart-gridline',
      });
      e.gridY.appendChild(ln);
    }

    // HTML labels in overlay
    while (e.yLabels.firstChild) e.yLabels.removeChild(e.yLabels.firstChild);
    for (const v of ticks) {
      const label = htmlEl('div', 'chart-tick chart-tick-y');
      label.textContent = String(v);
      label.style.top = (yS(v)) + 'px';
      e.yLabels.appendChild(label);
    }
  }

  function renderFlowTicks(m, yfS, plotW) {
    while (e.yLabelsFlow.firstChild) e.yLabelsFlow.removeChild(e.yLabelsFlow.firstChild);
    if (!m.showFlow) return;
    const [yf0, yf1] = m.yDomainFlow;
    const range = yf1 - yf0;
    const step = range <= 2 ? 0.5 : (range <= 5 ? 1 : 2);
    const first = Math.ceil(yf0 / step) * step;
    for (let v = first; v <= yf1 + 1e-6; v += step) {
      const label = htmlEl('div', 'chart-tick chart-tick-yflow');
      label.textContent = (Math.round(v * 10) / 10).toString();
      label.style.top = yfS(v) + 'px';
      label.style.left = (PAD_L + plotW + 4) + 'px';
      e.yLabelsFlow.appendChild(label);
    }
  }

  function renderXTicks(m, xS, plotH) {
    const [x0, x1] = m.xDomain;
    const beginMs = m.beginMs;
    const stepMs = chooseXStepMs(x1 - x0);
    const firstShot = Math.ceil((x0 - beginMs) / stepMs) * stepMs;

    while (e.gridX.firstChild) e.gridX.removeChild(e.gridX.firstChild);
    while (e.xLabels.firstChild) e.xLabels.removeChild(e.xLabels.firstChild);

    let tickCount = 0;
    for (let t = firstShot;
         beginMs + t <= x1 + 1e-6 && tickCount < MAX_X_TICKS;
         t += stepMs, tickCount++) {
      if (t < 0) continue;
      const tMs = beginMs + t;
      if (tMs < x0) continue;
      const x = xS(tMs);
      const ln = svgEl('line', {
        x1: x, x2: x, y1: PAD_T, y2: PAD_T + plotH, class: 'chart-gridline chart-gridline-x',
      });
      e.gridX.appendChild(ln);

      const label = htmlEl('div', 'chart-tick chart-tick-x');
      const secs = Math.round(t / 1000);
      label.textContent = secs + 's';
      label.style.left = x + 'px';
      e.xLabels.appendChild(label);
    }
  }

  function chooseXStepMs(spanMs) {
    const steps = [5000, 10000, 15000, 30000, 60000];
    for (const step of steps) {
      if (spanMs / step <= MAX_X_TICKS) return step;
    }
    return 60000;
  }

  function renderAnnotations(m, xS, plotH) {
    while (e.annot.firstChild) e.annot.removeChild(e.annot.firstChild);
    for (const a of m.annotations) {
      if (a.type !== 'pump-off-transition') continue;
      e.annot.appendChild(svgEl('rect', {
        x: xS(a.fromMs),
        y: PAD_T,
        width: Math.max(1, xS(a.toMs) - xS(a.fromMs)),
        height: plotH,
        class: 'chart-annot-pump-off-transition',
      }));
    }
    for (const a of m.annotations) {
      if (a.type === 'pump-off-transition') continue;
      const x = xS(a.tMs);
      if (a.type === 'pump-on' ||
          a.type === 'pump-signal-decay-onset' ||
          a.type === 'pump-off-confirmed') {
        const ln = svgEl('line', {
          x1: x, x2: x, y1: PAD_T, y2: PAD_T + plotH,
          class: 'chart-annot-line chart-annot-' + a.type,
        });
        if (a.type === 'pump-signal-decay-onset') {
          const title = svgEl('title');
          title.textContent = 'Pump signal decay onset; off confirmed ' +
            ((a.confirmedMs - a.tMs) / 1000).toFixed(2) + ' s later';
          ln.appendChild(title);
        } else if (a.type === 'pump-off-confirmed') {
          const title = svgEl('title');
          title.textContent = 'Pump off confirmed';
          ln.appendChild(title);
        }
        e.annot.appendChild(ln);
      }
    }
  }

  function renderCrosshair(m, xS, yS, plotW, plotH) {
    const pinTms = scrub ? scrub.pinnedTms : (hover ? hover.tMs : null);
    if (pinTms == null) {
      e.crosshair.style.display = 'none';
      e.readout.style.display = 'none';
      return;
    }
    const [x0, x1] = m.xDomain;
    if (pinTms < x0 || pinTms > x1) {
      e.crosshair.style.display = 'none';
      e.readout.style.display = 'none';
      return;
    }
    const x = xS(pinTms);
    e.crosshair.setAttribute('x1', x);
    e.crosshair.setAttribute('x2', x);
    e.crosshair.setAttribute('y1', PAD_T);
    e.crosshair.setAttribute('y2', PAD_T + plotH);
    e.crosshair.style.display = '';

    const pts = m.pts;
    const nearIdx = nearestPtIdx(pts, pinTms);
    if (nearIdx < 0) {
      e.readout.style.display = 'none';
      return;
    }
    const near = pts[nearIdx];
    let flowText = '';
    const gps = RobustFlow.sampleFlow(pts, nearIdx, { before: 0 });
    if (gps !== null) {
      flowText = ' · ' + gps.toFixed(1) + ' g/s';
    }
    const secs = ((near.tMs - m.beginMs) / 1000).toFixed(2);
    e.readout.textContent = 't=' + secs + 's · ' + near.g.toFixed(1) + ' g' + flowText;
    e.readout.style.display = '';
    // Position above the touch point. Clamp into plot area.
    const rx = Math.max(PAD_L, Math.min(PAD_L + plotW - 120, x - 60));
    e.readout.style.left = rx + 'px';
    e.readout.style.top = (PAD_T + 4) + 'px';
  }

  function nearestPtIdx(pts, tMs) {
    if (!pts.length) return -1;
    let lo = 0, hi = pts.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (pts[mid].tMs < tMs) lo = mid + 1;
      else hi = mid;
    }
    const a = lo > 0 ? pts[lo - 1] : null;
    const b = lo < pts.length ? pts[lo] : null;
    if (a && b) return Math.abs(a.tMs - tMs) < Math.abs(b.tMs - tMs) ? lo - 1 : lo;
    return a ? lo - 1 : lo;
  }

  // ---------- Pointer interaction ----------

  function clientXToTms(clientX) {
    if (!latest.extraction) return null;
    const rect = svg.getBoundingClientRect();
    const plotW = dims.w - PAD_L - PAD_R;
    const xLocal = clientX - rect.left - PAD_L;
    if (plotW <= 0) return null;
    const frac = Math.max(0, Math.min(1, xLocal / plotW));
    const x0 = latest.extraction.beginMs;
    const x1 = chartEndMs(x0, latest.nowMs);
    return x0 + frac * (x1 - x0);
  }

  function attachPointer() {
    e.hit.addEventListener('pointerdown', onPointerDown);
    e.hit.addEventListener('pointermove', onHoverMove);
    e.hit.addEventListener('pointerleave', onHoverLeave);
  }

  function onHoverMove(ev) {
    if (scrub) return;             // scrub state takes precedence
    if (ev.pointerType !== 'mouse') return;  // hover is mouse-only
    const t = clientXToTms(ev.clientX);
    if (t == null) return;
    hover = { tMs: t };
    schedulePaint();
  }
  function onHoverLeave() {
    if (scrub) return;
    if (hover) { hover = null; schedulePaint(); }
  }

  function onPointerDown(ev) {
    ev.preventDefault();
    const t = clientXToTms(ev.clientX);
    if (t == null) return;
    scrub = { pointerId: ev.pointerId, pinnedTms: t };
    hover = null;
    windowHandlers = {
      move: (ev2) => {
        if (!scrub || ev2.pointerId !== scrub.pointerId) return;
        const tt = clientXToTms(ev2.clientX);
        if (tt == null) return;
        scrub.pinnedTms = tt;
        schedulePaint();
      },
      up: (ev2) => {
        if (!scrub || ev2.pointerId !== scrub.pointerId) return;
        releaseScrub();
      },
      cancel: () => releaseScrub(),
    };
    window.addEventListener('pointermove', windowHandlers.move);
    window.addEventListener('pointerup', windowHandlers.up);
    window.addEventListener('pointercancel', windowHandlers.cancel);
    schedulePaint();
  }

  function releaseScrub() {
    if (windowHandlers) {
      window.removeEventListener('pointermove', windowHandlers.move);
      window.removeEventListener('pointerup', windowHandlers.up);
      window.removeEventListener('pointercancel', windowHandlers.cancel);
      windowHandlers = null;
    }
    scrub = null;
    schedulePaint();
  }

  return { mount, update };
}

// The two charts the app uses: the in-flight curve in the live view's Now
// card, and the record chart shared by the Last-shot card and the history
// detail view (those two never show together).
const LiveChart = createChart();
const RecordChart = createChart();
