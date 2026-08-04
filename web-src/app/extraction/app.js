// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

let state = null;          // built from STATE packets
// The two record slots, matching the two live-view cards. `inflight` is the
// in-flight extraction (CURRENT_RECORD + appended SAMPLE_BATCHes, bound to
// its packet shotSeq); it either graduates into `lastRecord` (FINAL_RECORD)
// or is dropped. `lastRecord` is the device's display shot — the last
// graduated pull, or a stored shot loaded onto the device (boot restore,
// "set as last shot").
let inflight = null;
let lastRecord = null;
let protocolMismatch = null;
// The "new" badge on the Last-shot card: set when we watched a shot finish
// live, bound to that shot's beginMs so a different record arriving inside
// the badge window doesn't wear it. Both 0 for a record that arrived as
// history on connect.
let freshFinalizedAt = 0;
let freshBeginMs = 0;
// User tapped the collapsed Last-shot card open mid-pour. Reset when the
// pump window ends.
let lastShotExpanded = false;

// History view state. `shots` accumulates loaded pages (newest-first); null
// means never loaded.
let view = 'live';
let shots = null;
let shotsHasMore = false;
let shotsTotal = 0;
let lastShotsSavedSeq = -1;
let selectedShotId = null;
let selectedShot = null;
let selectedShotError = null;
let renderPending = false;

function fmtMMSS(ms) {
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  return String(m).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
}
// Live timer format, M:SS.D — same resolution as the on-device timer cell.
function fmtTimer(ms) {
  const tenths = Math.floor(ms / 100);
  const m = Math.floor(tenths / 600);
  const s = Math.floor((tenths % 600) / 10);
  return m + ':' + String(s).padStart(2, '0') + '.' + (tenths % 10);
}
// The pump window (RUNNING/POST_PUMP on the foreground extraction screen) is
// when live activity exists: the live curve draws and the Last-shot card
// yields the screen.
function inPumpWindow(s) {
  return !!s && s.active && (s.phase === 'RUNNING' || s.phase === 'POST_PUMP');
}
function fmtG(cg) {
  if (cg === -32768) return '—';
  return (cg / 100).toFixed(1);
}
// Below this the scale was effectively pre-tared, so the raw reading and the
// self-tared yield are the same number and we show just one. Matches the
// on-device gauge threshold (ExtractionScreen: 0.2 g, just above BLE jitter).
const WEIGHT_DIFF_CG = 20;  // 0.2 g

// A .big weight readout: the self-tared yield, plus the raw scale weight in
// small type when the two diverge substantially (the operator left the cup
// weight on the scale). Mirrors the on-device gauge. rawCg may be null or
// NO_WEIGHT when no raw reading is available, leaving only the yield.
function bigWeight(yieldCg, rawCg) {
  const kids = [fmtG(yieldCg), el('span', { class: 'unit' }, 'g')];
  if (rawCg != null && rawCg !== NO_WEIGHT_CG && yieldCg !== NO_WEIGHT_CG &&
      Math.abs(rawCg - yieldCg) > WEIGHT_DIFF_CG) {
    kids.push(el('span', { class: 'sub' }, fmtG(rawCg) + ' g scale'));
  }
  return el('div', { class: 'big' }, ...kids);
}

// Raw final scale reading of a finished shot: the stored settledRawCg.
function rawFinalCg(e) {
  if (e.settledRawCg != null && e.settledRawCg !== NO_WEIGHT_CG) {
    return e.settledRawCg;
  }
  return null;
}
function fmtSec(ms) { return (ms / 1000).toFixed(2); }
function finishedElapsedMs(e) {
  if (!e || e.endMs === 0) return null;
  return Math.max(0, e.endMs - e.beginMs);
}
// Memoized: toLocaleString builds a fresh Intl.DateTimeFormat per call — the
// most expensive single call on the live view's per-STATE render path, for a
// string that only changes when a new record arrives. Keyed by the timestamp;
// bounded by the handful of distinct shots viewed per page load.
const fmtDateCache = new Map();
function fmtDate(utcSec) {
  if (!utcSec) return null;
  let s = fmtDateCache.get(utcSec);
  if (s === undefined) {
    try {
      s = new Date(utcSec * 1000)
        .toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
    } catch (_) {
      s = new Date(utcSec * 1000).toLocaleString();
    }
    fmtDateCache.set(utcSec, s);
  }
  return s;
}

function el(tag, attrs, ...kids) {
  attrs = attrs || {};
  const e = document.createElement(tag);
  for (const k in attrs) {
    if (k === 'class') e.className = attrs[k];
    else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), attrs[k]);
    else e.setAttribute(k, attrs[k]);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}

function appendStorageWarning(app, currentState) {
  // Keep this precedence aligned with ExtractionScreen::drawStorageWarning().
  if (currentState.storage === 'unavailable') {
    app.append(el('p', { class: 'storage-warning' },
      'Shot history unavailable. Check the device screen.'));
  } else if (currentState.acceptedSeq !== currentState.savedSeq) {
    app.append(el('p', { class: 'storage-warning' },
      'Couldn’t save the latest shot.'));
  }
}

// One key/value line for a details list (dl.rows).
function row(k, v) {
  return el('div', { class: 'row' },
    el('span', { class: 'k' }, k), el('span', { class: 'v' }, v));
}

// Human summary of the target-alert outcome. Once the STOP-NOW alert has
// fired, alarmTriggeredMs is set and we report when, the yield at that moment,
// and the projected final; otherwise it's armed-but-not-yet or off. The alarm
// time is shot-relative (measured from beginMs).
function fmtCutAlert(e) {
  if (e.alarmTriggeredMs > 0) {
    const at = fmtMMSS(Math.max(0, e.alarmTriggeredMs - e.beginMs));
    let s = 'fired at ' + at + ' · ' + fmtG(e.alarmYieldCg) + ' g';
    if (e.alarmProjectedFinalCg !== NO_WEIGHT_CG) {
      s += ' → ' + fmtG(e.alarmProjectedFinalCg) + ' g projected';
    }
    return s;
  }
  return e.target.armed ? 'armed · didn’t fire' : 'off';
}

function setBanner(text, cls) {
  const b = document.getElementById('banner');
  b.textContent = text;
  b.className = 'banner ' + cls;
}

// A short-lived banner message that survives the next few renders before the
// status banner resumes — renderBanner() rewrites the banner every render, so a
// plain setBanner() would otherwise vanish on the next SSE tick. Use it for
// one-off action feedback (e.g. "loaded shot N").
let bannerFlash = null;  // { text, cls, until } | null
function flashBanner(text, cls, ms = 4000) {
  bannerFlash = { text, cls, until: Date.now() + ms };
  setBanner(text, cls);
  // Re-render once it expires so the status banner resumes even if no SSE tick
  // happens to repaint in the meantime.
  setTimeout(() => scheduleRender(), ms);
}
// True while a flash is showing; renderBanner() defers to it. Clears itself when
// expired so the status banner takes over again.
function bannerFlashActive() {
  if (bannerFlash && Date.now() < bannerFlash.until) {
    setBanner(bannerFlash.text, bannerFlash.cls);
    return true;
  }
  bannerFlash = null;
  return false;
}

function render() {
  const app = document.getElementById('app');
  app.replaceChildren();
  renderBanner();
  if (!state) return;
  renderTabs(app);
  appendStorageWarning(app, state);
  if (view === 'live') renderLive(app);
  else if (view === 'list') renderList(app);
  else if (view === 'detail') renderDetail(app);
}

function scheduleRender() {
  if (renderPending) return;
  renderPending = true;
  requestAnimationFrame(() => {
    renderPending = false;
    render();
  });
}

function scheduleLiveRender() {
  if (view === 'live') {
    scheduleRender();
  } else {
    renderBanner();
  }
}

connect();
