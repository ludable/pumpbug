// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

function renderBanner() {
  // A recent one-off action message takes precedence over the status banner.
  if (bannerFlashActive()) return;
  if (protocolMismatch) {
    setBanner(protocolMismatch, 'bad');
    return;
  }
  if (!state) { setBanner('connecting…', 'idle'); return; }
  if (state.active) {
    setBanner(`device live · phase ${state.phase} · scale ${state.scale}`,
              state.phase === 'IDLE' ? 'idle' : 'live');
  } else {
    setBanner('device idle (extraction screen not foreground)', 'idle');
  }
}

function renderTabs(app) {
  const tabs = el('div', { class: 'tabs' },
    el('button', {
      class: view === 'live' ? 'active' : '',
      onclick: () => switchView('live'),
    }, 'Live'),
    el('button', {
      class: view === 'live' ? '' : 'active',
      onclick: () => switchView('list'),
    }, 'History'),
  );
  app.append(tabs);
}

// Product summary rows for a finished record: target and the cut-alert
// outcome (the whole point of the target feature), how the shot ended when
// that's news (a timeout; a normal settle is omitted), and time under pump.
// Yield and duration already lead as the big readouts, so they aren't
// repeated here.
function recordRows(e) {
  const rows = el('dl', { class: 'rows' },
    row('target', (e.target.targetCg > 0)
      ? fmtG(e.target.targetCg) + ' g' : 'none'));
  rows.append(row('cut alert', fmtCutAlert(e)));
  if (e.endCause in END_CAUSE_LABEL) {
    rows.append(row('ended', END_CAUSE_LABEL[e.endCause]));
  }
  rows.append(row('pump time', fmtSec(e.totalPumpOnMs) + ' s'));
  return rows;
}

// Diagnostics, folded away by default: raw enums, counts, the internal raw
// weights, and the graduation evidence. Off the product view but one click
// away for debugging.
function techFold(e) {
  let pumpOff = null;
  for (const event of e.events || []) {
    if (event.kind === 3 && event.tMs === e.lastPumpOffConfirmedMs) {
      pumpOff = event;
    }
  }
  const hasDecayEstimate = pumpOff && pumpOff.signalDecayOnsetMs != null;
  const pumpTransition = e.version < 7
    ? 'not recorded (EXTR v6)'
    : hasDecayEstimate ? 'signal decay → confirmed' : 'confirmation only';
  const tech = el('dl', { class: 'rows' },
    row('phase', PHASE[e.phase] ?? e.phase),
    row('endCause', END_CAUSE[e.endCause] ?? e.endCause),
    row('yield', fmtG(e.yieldCg) + ' g'),
    row('startRaw', (e.startRawCg != null && e.startRawCg !== NO_WEIGHT_CG)
      ? fmtG(e.startRawCg) + ' g' : '—'),
    row('settledRaw', fmtG(e.settledRawCg) + ' g'),
    row('samples', `${e.sampleCount} (observed ${e.observedSampleCount})`),
    row('events', String(e.eventCount)),
    row('event trace', (e.flags & 0x01) !== 0 ? 'incomplete' : 'complete'),
    row('EXTR version', String(e.version)),
    row('pump transition', pumpTransition),
    ...(hasDecayEstimate
      ? [row('confirmation delay',
          fmtSec(pumpOff.signalDecayLeadMs) + ' s')]
      : []),
    row('pourMs', String(e.pourMs)),
    row('decisionGain', fmtG(e.decisionGainCg) + ' g'),
    row('startUtcSec', fmtDate(e.startUtcSec) ||
      (e.startUtcSec ? String(e.startUtcSec) : 'unknown')),
  );
  return el('details', { class: 'tech' },
    el('summary', {}, 'Technical'), tech);
}

// The pour curve, mounted under `parent`. No-op until the first sample.
// For an in-flight shot (endMs == 0) the time axis runs to the live elapsed;
// for a finished one it runs to the recorded end.
function renderCurve(chart, parent, e) {
  if (e.sampleCount <= 0) return;
  const lastTms = e.samples.length ? e.samples[e.samples.length - 1].tMs : e.beginMs;
  const lastEventTms = e.events.length ? e.events[e.events.length - 1].tMs : e.beginMs;
  const stateTms = (state && state.elapsedMs != null) ? e.beginMs + state.elapsedMs : 0;
  const endTms = e.endMs;
  const nowMs = endTms > 0
    ? Math.max(endTms, lastTms, lastEventTms)
    : Math.max(stateTms, lastTms, lastEventTms);
  chart.mount(parent);
  chart.update(e, nowMs);
}

// One finished, graduated shot as a record card: title line, the big
// yield/duration readouts, the chart, the product summary, the technical
// fold. Shared by the live view's Last-shot card and the history detail view
// so a shot looks the same the moment it finishes and a week later.
function renderShotRecord(app, e, opts) {
  const card = el('section',
    { class: 'card record' + (opts.fresh ? ' fresh' : '') });
  const head = el('h2', {}, opts.title);
  if (opts.fresh) head.append(el('span', { class: 'chip' }, 'new'));
  card.append(head);
  card.append(bigWeight(e.yieldCg, rawFinalCg(e)));
  card.append(el('div', { class: 'big' }, fmtMMSS(finishedElapsedMs(e) ?? 0)));
  renderCurve(RecordChart, card, e);
  card.append(recordRows(e));
  card.append(techFold(e));
  app.append(card);
}

// Fixed-height slot for the in-flight curve. Exists for the whole pump
// window so the layout never jumps mid-activity: before samples arrive (or
// when the scale is gone) it holds a placeholder of the same size.
function renderLiveChartSlot(card) {
  const slot = el('div', { class: 'chartslot' });
  if (inflight && inflight.sampleCount > 0) {
    renderCurve(LiveChart, slot, inflight);
  } else {
    slot.append(el('p', { class: 'empty' },
      state.scale === 'connected' ? 'waiting for samples…'
                                  : 'waiting for scale…'));
  }
  card.append(slot);
}

// The pouring timer ticks locally between rate-limited STATE packets: each
// STATE anchors elapsedMs to its receipt time, and a light interval patches
// just the timer text so the tenths run smoothly without full re-renders.
// Extrapolation stops once packets go stale (TIMER_EXTRAPOLATE_MAX_MS), so a
// paused replay or a dropped stream shows the device's last reported time
// rather than a timer that keeps counting.
let liveTimerEl = null;
let liveTimerTicker = 0;
function liveElapsedNowMs() {
  const base = state.elapsedMs || 0;
  if (state.rxAtMs == null) return base;
  const age = performance.now() - state.rxAtMs;
  return age < TIMER_EXTRAPOLATE_MAX_MS ? base + age : base;
}
function ensureLiveTimerTicker() {
  if (liveTimerTicker) return;
  liveTimerTicker = setInterval(() => {
    if (view === 'live' && state.pouring && liveTimerEl) {
      liveTimerEl.textContent = fmtTimer(liveElapsedNowMs());
    } else {
      clearInterval(liveTimerTicker);
      liveTimerTicker = 0;
    }
  }, 100);
}

// The Now card shows live measurements rather than a stored result. It displays
// raw scale weight while idle or pumping without a pour, and self-tared yield
// during a pour. pumpDecayCandidate adds a temporary cue that pump-signal decay
// may have begun before pump-off is confirmed.
function renderNowCard(app) {
  const pumping = inPumpWindow(state);
  const pouring = state.pouring;
  const pumpStopping = state.active && state.phase === 'RUNNING' &&
    state.pumpDecayCandidate;

  const card = el('section', {
    class: pumpStopping ? 'now decay' : 'now',
  });
  card.append(el('h2', {},
    el('span', { class: 'livedot' + (state.active ? ' on' : '') }), 'Now'));

  let mode;
  if (pouring && state.currentYieldCg != null) {
    card.append(bigWeight(state.currentYieldCg, state.currentWeightCg));
    mode = pumpStopping ? 'yield · pump stopping' : 'yield';
  } else if (state.currentWeightCg != null) {
    card.append(bigWeight(state.currentWeightCg, null));
    mode = pumpStopping ? 'scale · pump stopping'
      : pumping ? 'scale · pump running' : 'scale';
  } else {
    card.append(bigWeight(NO_WEIGHT_CG, null));
    mode = state.active ? 'scale ' + state.scale : 'device idle';
  }
  card.append(el('div', { class: 'mode' }, mode));
  const timer = el('div', { class: 'big timer' + (pouring ? '' : ' dim') },
    pouring ? fmtTimer(liveElapsedNowMs()) : '0:00.0');
  card.append(timer);
  if (pouring) {
    liveTimerEl = timer;
    ensureLiveTimerTicker();
  } else {
    liveTimerEl = null;
  }

  if (pumping) renderLiveChartSlot(card);
  app.append(card);
}

// The Last-shot card: the device's display shot (FINAL_RECORD — the last
// graduated pull or a loaded stored shot), so a flush or grinder dose can
// never appear here. Collapses to a tap-to-expand header while the pump
// window is live so the pour owns the screen.
function renderLastShot(app) {
  if (!lastRecord) return;
  if (inPumpWindow(state) && !lastShotExpanded) {
    app.append(el('section', {
      class: 'card record collapsed',
      onclick: () => { lastShotExpanded = true; scheduleRender(); },
    }, '▸ last shot · ' + fmtG(lastRecord.yieldCg) + ' g · '
       + fmtMMSS(finishedElapsedMs(lastRecord) ?? 0)));
    return;
  }
  const fresh = freshFinalizedAt > 0 &&
      Date.now() - freshFinalizedAt < FRESH_SHOT_BADGE_MS &&
      lastRecord.beginMs === freshBeginMs;
  renderShotRecord(app, lastRecord, {
    title: 'Last shot · ' + (fmtDate(lastRecord.startUtcSec) || 'time unknown'),
    fresh,
  });
}

function renderLive(app) {
  renderNowCard(app);
  renderLastShot(app);
}
