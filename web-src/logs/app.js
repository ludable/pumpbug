// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Log viewer. Fetches one log at a time — only the visible tab.
// The active tab is polled on an interval (conditional GET, so quiet logs cost
// a 304); switching tabs renders the cached log immediately, then refetches.
// Polling pauses while the page is hidden to spare the device's radio.

const POLL_MS = 5000;
const COPY_BUTTON_LABEL = 'Copy current log';

const TABS = [
  // `key` is the wire name (?log=) and COLS/cache key; `label` is the UI title.
  { key: 'extraction', label: 'Shots' },
  { key: 'pump', label: 'Pump detect' },
  { key: 'net', label: 'Net' },
  { key: 'blescan', label: 'BLE scan' },
  { key: 'scalemsg', label: 'Scale msgs' },
  { key: 'power', label: 'Power' },
  { key: 'heap', label: 'Memory' },
  { key: 'panic', label: 'Crash' },
];

// Numeric-enum → label maps. These mirror the device's terse on-screen labels
// (LogsScreen) but spelled out for the larger screen. Keep in sync with
// pump_scale::EndCause and esp_reset_reason_t ordering.
const END_CAUSE = { 0: '—', 1: 'stable', 2: 'timeout' };
const RESET_REASON = {
  0: '—', 1: 'power-on', 2: 'ext', 3: 'sw', 4: 'panic', 5: 'int-wdt',
  6: 'task-wdt', 7: 'wdt', 8: 'deep-sleep', 9: 'brownout', 10: 'sdio',
};

// Xtensa EXCCAUSE → label (common subset; falls back to the raw number). These
// are the values esp_core_dump_get_summary() reports for a crash.
const EXC_CAUSE = {
  0: 'IllegalInstruction', 1: 'Syscall', 2: 'InstrFetchError',
  3: 'LoadStoreError', 5: 'Alloca', 6: 'DivideByZero', 8: 'Privileged',
  9: 'LoadStoreAlignment', 20: 'InstrFetchProhibited',
  28: 'LoadProhibited', 29: 'StoreProhibited',
};
const EXC_CAUSE_MASK = 0x3f;

const logs = {};     // per-log payload cache, keyed by tab
let tab = 'extraction';
let authed = true;
let pollTimer = null;
let pollPromise = null;
let pollQueued = false;
let copyResetTimer = null;

// --- formatting -------------------------------------------------------------
function fmtTime(utcSec, ms) {
  if (utcSec && utcSec > 0) {
    return new Date(utcSec * 1000).toLocaleTimeString([], { hour12: false });
  }
  if (ms != null) return '+' + Math.floor(ms / 1000) + 's';
  return '—';
}
function fmtDateTime(utcSec) {
  if (!utcSec) return '—';
  return new Date(utcSec * 1000).toLocaleString([], { hour12: false });
}
function fmtWeight(cg) {
  return cg == null ? '—' : (cg / 100).toFixed(1) + 'g';
}
function secs(ms) { return Math.round(ms / 1000) + 's'; }
function hex(n) { return '0x' + (n >>> 0).toString(16).padStart(8, '0'); }
function kb(bytes) {
  if (bytes == null) return '—';
  return (bytes / 1024).toFixed(bytes < 10240 ? 1 : 0) + 'k';
}
function fixed(n, digits) {
  return n == null ? '—' : Number(n).toFixed(digits);
}
function tenths(ms) {
  return ms == null ? '—' : (ms / 1000).toFixed(1) + 's';
}
function pumpFailures(mask) {
  // Keep these bits in sync with VibrationWindowTrigger::Failure.
  const parts = [];
  if (mask & (1 << 0)) parts.push('moving');
  if (mask & (1 << 1)) parts.push('low SNR');
  if (mask & (1 << 2)) parts.push('invalid SNR');
  if (mask & (1 << 3)) parts.push('frequency');
  if (mask & (1 << 4)) parts.push('invalid frequency');
  return parts.join(', ');
}
function fmtExcCause(rawCause) {
  const raw = Number(rawCause) >>> 0;
  const masked = raw & EXC_CAUSE_MASK;
  const label = EXC_CAUSE[masked] || ('cause ' + masked);
  if (raw === masked) return label + ' (' + hex(masked) + ')';
  return label + ' (raw ' + hex(raw) + ', masked ' + hex(masked) + ')';
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
  ));
}

const POWER_BOOT = {
  BUS: 1 << 0,
  TX_STARTED: 1 << 1,
  TX_SUCCEEDED: 1 << 2,
  DISPLAY: 1 << 3,
  PM1: 1 << 4,
  WAKE_VALID: 1 << 5,
  GPIO_VALID: 1 << 6,
  RAIL_READ: 1 << 7,
  RAIL_ON: 1 << 8,
  I2C_CFG_READ: 1 << 9,
  I2C_SLEEP_DISABLED: 1 << 10,
};

function powerBootSummary(e) {
  if (e.kind !== 'wake') return '';
  const f = e.bootFlags || 0;
  const parts = [
    'bus:' + ((f & POWER_BOOT.BUS) ? 'ok' : 'fail'),
    'pulse:' + ((f & POWER_BOOT.TX_SUCCEEDED) ? 'ok' :
      ((f & POWER_BOOT.TX_STARTED) ? 'nack' : 'fail')),
    'display:' + ((f & POWER_BOOT.DISPLAY) ? 'ok' : 'fail'),
    'pm1:' + ((f & POWER_BOOT.PM1) ? 'ok' : 'fail'),
  ];
  if (f & POWER_BOOT.I2C_CFG_READ)
    parts.push('cfg:0x' + e.pm1BootI2cConfig.toString(16).padStart(2, '0'));
  else
    parts.push('cfg:?');
  parts.push('sleep-off:' +
    ((f & POWER_BOOT.I2C_SLEEP_DISABLED) ? 'ok' : 'fail'));
  if (f & POWER_BOOT.RAIL_READ)
    parts.push('lcd:' + ((f & POWER_BOOT.RAIL_ON) ? 'on' : 'off'));
  else
    parts.push('lcd:?');
  return parts.join(' ');
}

function powerWakeSummary(e) {
  if (e.kind !== 'wake') return '';
  const parts = [];
  // Mirrors M5PM1_WAKE_SRC_EXT_WAKE (0x20) and M5PM1_IRQ_GPIO4 (0x10).
  // Together they identify the PM1 GPIO4 input connected to BMI270 INT1.
  if (e.pm1WakeSource != null && e.pm1GpioIrq != null &&
      (e.pm1WakeSource & 0x20) && (e.pm1GpioIrq & 0x10))
    parts.push('motion');
  if (e.pm1WakeSource != null)
    parts.push('src=0x' + e.pm1WakeSource.toString(16).padStart(2, '0'));
  if (e.pm1GpioIrq != null)
    parts.push('gpio=0x' + e.pm1GpioIrq.toString(16).padStart(2, '0'));
  return parts.join(' ');
}

function powerI2cSummary(e) {
  if (e.kind !== 'sleep' || e.pm1I2cConfig == null) return '';
  const raw = e.pm1I2cConfig;
  return '0x' + raw.toString(16).padStart(2, '0') +
    ' (sleep ' + ((raw & 0x0f) ? (raw & 0x0f) + ' s' : 'off') + ')';
}

// Per-tab columns: [heading, value(entry), optional cell class].
const COLS = {
  pump: [
    ['Time', e => fmtTime(e.utcSec, e.ms)],
    ['Event', e => e.event],
    ['Detected', e => e.event === 'off' ? tenths(e.detectedForMs) : '—'],
    ['Raw SNR', e => fixed(e.rawSnrDb, 1)],
    ['Smooth SNR', e => fixed(e.smoothedSnrDb, 1)],
    ['Peak Hz', e => fixed(e.peakHz, 0)],
    ['Dominant Hz', e => fixed(e.dominantPeakHz, 0)],
    ['Flux', e => fixed(e.spectralFlux, 2)],
    ['Motion', e => e.stationary ? 'stationary' : 'moving'],
    ['Close reason', e => pumpFailures(e.closeFailureMask), 'msg'],
  ],
  extraction: [
    ['Time', e => fmtTime(e.startUtcSec, e.beginMs)],
    ['Dur', e => secs(e.endMs - e.beginMs)],
    ['Pump', e => secs(e.totalPumpOnMs)],
    ['Weight', e => fmtWeight(e.yieldCg)],
    ['End', e => END_CAUSE[e.endCause] || '?'],
    ['Real', e => e.isLikelyRealShot ? '✓' : ''],
  ],
  net: [
    ['Time', e => fmtTime(e.utcSec, e.ms)],
    ['Source', e => e.source],
    ['Code', e => e.code],
    ['Message', e => e.msg || '', 'msg'],
  ],
  power: [
    ['Time', e => fmtDateTime(e.utcSec)],
    ['Event', e => e.kind],
    ['Battery', e => e.batteryPct == null ? '—' : e.batteryPct + '%'],
    ['Power', e => e.hasExternalPower ? 'external' : 'battery'],
    ['Reason', e => RESET_REASON[e.resetReason] || '?'],
    ['Boot', powerBootSummary, 'power-diag'],
    ['PM1 wake', powerWakeSummary, 'power-diag'],
    ['PM1 I2C', powerI2cSummary, 'power-diag'],
  ],
  // Each heap row is a time bucket; the values are the worst case (minimum
  // free / minimum largest block = peak usage and peak fragmentation) over that
  // window. A falling Int-free trend is a leak; falling Int-block with steady
  // Int-free is fragmentation.
  heap: [
    ['Time', e => fmtTime(e.utcSec, e.ms)],
    ['Int free', e => kb(e.intFree)],
    ['Int block', e => kb(e.intLargest)],
    ['DMA block', e => kb(e.dmaLargest)],
    ['PSRAM', e => kb(e.psramFree)],
  ],
};

// Single-record field lists — the transposed analog of COLS: [label,
// value(obj), optional cell class], rendered as label|value rows by
// kvTableHtml(). Used by the tabs whose data is one object (the live heap
// snapshot, the single stored crash) rather than a list of homogeneous rows.
const FIELDS = {
  panic: [
    ['Cause', e => fmtExcCause(e.excCause)],
    ['Task', e => e.task || '?'],
    ['PC', e => hex(e.excPc)],
    ['vaddr', e => hex(e.excVaddr)],
    ['Backtrace', e => (e.bt || []).map(hex).join(' ') +
      (e.btCorrupted ? ' (corrupt)' : ''), 'msg'],
  ],
  heapLive: [
    ['Int free', L => kb(L.internalFree)],
    ['Int block', L => kb(L.internalLargest)],
    ['Int min ever', L => kb(L.internalMinEver)],
    ['DMA block', L => kb(L.dmaLargest)],
    ['PSRAM free', L => kb(L.psramFree)],
    ['Alloc fails', L => L.allocFailCount || 0],
    ['Last fail size', L => L.lastFailSize ? L.lastFailSize + ' B' : ''],
    ['Last fail caps', L => L.lastFailCaps ? hex(L.lastFailCaps) : ''],
    ['Last fail task', L => L.lastFailTask || ''],
    ['Last fail fn', L => L.lastFailFn || ''],
  ],
  powerLive: [
    ['Battery', L => L.batteryPct == null ? '—' : L.batteryPct + '%'],
    ['Voltage', L => L.batteryMv == null ? '—' : L.batteryMv + ' mV'],
    ['Power', L => L.hasExternalPower ? 'external' : 'battery'],
  ],
};

function rowsFor(key) { const l = logs[key]; return (l && l.entries) || []; }

// --- rendering --------------------------------------------------------------
// The "Not paired" notice for the content area. Tabs stay visible so the Panic
// tab — whose endpoint needs no pairing (a crash can be what broke auth) —
// remains reachable; only the authenticated tabs show this.
function renderUnauthedContent() {
  document.getElementById('app').innerHTML =
    '<p class="muted">Not paired. ' +
    '<a href="/config/">Pair on the setup page →</a></p>';
}

function renderTabs() {
  const nav = document.getElementById('tabs');
  nav.innerHTML = '';
  TABS.forEach(t => {
    const b = document.createElement('button');
    b.className = 'tab' + (t.key === tab ? ' active' : '');
    b.textContent = t.label;
    b.onclick = () => selectTab(t.key);
    nav.appendChild(b);
  });
}

// The action bar that heads a history list: an "N / cap <noun>" count and a
// Clear button. On single-section tabs it's the page header; on tabs with a live
// block above (power/memory) it heads the History section, sitting right above
// the table so the count and Clear read as history-scoped, not page-scoped.
function barHtml(log, count, noun) {
  return '<div class="bar"><span class="muted">' +
    count + ' / ' + log.cap + ' ' + noun + '</span>' +
    '<button class="clear" data-log="' + tab + '">Clear</button></div>';
}

// A small uppercase section label, separating the "Now" (live) and "History"
// blocks on tabs that carry both.
function sectionLabel(text) {
  return '<div class="section">' + text + '</div>';
}

// Generic table body driven by COLS[tab]. Used by every tab except panic/heap,
// which carry their own shapes.
function tableHtml(cols, entries) {
  if (!entries.length) return '<p class="muted empty">No entries.</p>';
  let html = '<table><thead><tr>';
  cols.forEach(c => { html += '<th>' + c[0] + '</th>'; });
  html += '</tr></thead><tbody>';
  entries.forEach(e => {
    const dim = (tab === 'extraction' && !e.isLikelyRealShot) ? ' class="dim"' : '';
    html += '<tr' + dim + '>';
    cols.forEach(c => {
      const cls = c[2] ? ' class="' + c[2] + '"' : '';
      html += '<td' + cls + '>' + escapeHtml(String(c[1](e))) + '</td>';
    });
    html += '</tr>';
  });
  return html + '</tbody></table>';
}

// Transposed table for a single record: one label|value row per field. The
// FIELDS analog of tableHtml — same [label, value(obj), cellClass?] shape, same
// th/td styling, just rotated.
function kvTableHtml(fields, obj) {
  let html = '<table><tbody>';
  fields.forEach(f => {
    const cls = f[2] ? ' class="' + f[2] + '"' : '';
    html += '<tr><th>' + f[0] + '</th><td' + cls + '>' +
      escapeHtml(String(f[1](obj))) + '</td></tr>';
  });
  return html + '</tbody></table>';
}

// Panic: the single crash decoded from the coredump partition (0 or 1 entry —
// the partition holds only the most recent). The backtrace is raw PCs; decode
// them offline against the matching firmware.elf with addr2line. When it
// happened and the reset reason live in the Power tab (the panic reboot).
function panicHtml(entries) {
  if (!entries.length) {
    return '<p class="muted empty">No crash stored. 🎉</p>';
  }
  return kvTableHtml(FIELDS.panic, entries[0]) +
    '<p class="muted hint">When it happened: see the Power tab (panic reboot). ' +
    'Decode PCs offline: ' +
    '<code>xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf &lt;pc&gt;…</code>' +
    '</p>';
}

// Heap: a "Now" section (current allocator state + since-boot low-water mark and
// allocation-failure counters) and a "History" section (per-bucket worst case).
function heapHtml(log) {
  const L = log.live || {};
  let html = sectionLabel('Now') + kvTableHtml(FIELDS.heapLive, L);
  if (L.allocFailCount) {
    html += '<p class="muted">Last failure: ' + L.lastFailSize +
      ' bytes, caps ' + hex(L.lastFailCaps) +
      (L.lastFailTask ? ', task ' + escapeHtml(L.lastFailTask) : '') +
      (L.lastFailFn ? ', fn ' + escapeHtml(L.lastFailFn) : '') + '</p>';
  }
  const period = log.bucketMs ? Math.round(log.bucketMs / 1000) + 's' : '';
  html += sectionLabel('History');
  html += barHtml(log, (log.entries || []).length, 'buckets');
  html += '<p class="muted hint">Worst case per ' + period +
    ' bucket (lower = more pressure)</p>';
  html += tableHtml(COLS.heap, log.entries || []);
  return html;
}

// Power: a "Now" section (current charge + plugged-in state) and a "History"
// section (wake/sleep events).
function powerHtml(log) {
  let html = sectionLabel('Now') + kvTableHtml(FIELDS.powerLive, log.live || {});
  html += sectionLabel('History (wake / sleep events)');
  html += barHtml(log, (log.entries || []).length, 'entries');
  html += tableHtml(COLS.power, log.entries || []);
  return html;
}

// BLE scan: the live DiagScan device table. Unlike the other tabs this is not a
// ring — it's a snapshot of what the radio currently sees, re-fetched each poll
// (no ETag). `busy` means an on-device app is holding the radio, so scanning is
// paused; recognized scales are highlighted and floated to the top.
function bleScanHtml(d) {
  if (d.busy) {
    return '<p class="muted empty">Scale in use — scan unavailable.</p>';
  }
  // The scan is driven by the on-device firmware; the web view is read-only.
  if (!d.active) {
    return '<p class="muted empty">Open <strong>Diagnostics → BLE scan</strong> ' +
      'on the device to start scanning.</p>';
  }
  const devs = (d.devices || []).slice();
  let html = '<div class="bar"><span class="muted">' +
    'scanning… ' + devs.length + ' device(s)</span></div>';
  if (!devs.length) return html + '<p class="muted empty">No devices yet.</p>';
  // Recognized first, then strongest signal.
  devs.sort((a, b) => (b.recognized - a.recognized) || (b.rssi - a.rssi));
  html += '<table><thead><tr>' +
    '<th>Device</th><th>RSSI</th><th>Adv interval</th>' +
    '</tr></thead><tbody>';
  devs.forEach(e => {
    const cls = e.recognized ? ' class="recognized"' : '';
    const label = e.name || e.addr;
    const iv = e.intervalMs == null ? '~?' : '~' + e.intervalMs + 'ms';
    html += '<tr' + cls + '><td>' + escapeHtml(label) + '</td><td>' +
      e.rssi + '</td><td>' + iv + '</td></tr>';
  });
  return html + '</tbody></table>';
}

// Labels for the numeric MsgTag, index-aligned with the device's MsgTag enum
// (RX tags, then TX tags). Kept here so the device emits only the numeric tag.
const MSG_TAGS = [
  'WEIGHT', 'BATTERY', 'TIMER', 'KEY', 'UNKNOWN', 'MIXED', 'REJECTED', 'SETTINGS',
  'HEARTBEAT', 'TARE', 'TIMER', 'IDENTIFY', 'SUBSCRIBE', 'OTHER',
];
const MSG_TAG_REJECTED = 6;
const MSG_TAG_MIXED = 5;

// Scale messages: the live capture-tap ring (newest-first), driven by the firmware.
// Read-only here; when not armed we point the user at the device.
function scaleMsgHtml(d) {
  if (!d.armed) {
    return '<p class="muted empty">Open <strong>Diagnostics → Scale msgs</strong> ' +
      'on the device to capture traffic.</p>';
  }
  const counts = d.counts || [];
  const parts = [];
  MSG_TAGS.forEach((lbl, i) => { if (counts[i]) parts.push(lbl + ' ' + counts[i]); });
  let html = '<div class="bar"><span class="muted">' +
    (parts.length ? parts.join(' · ') : 'no messages yet') + '</span></div>';
  const recs = d.records || [];
  if (!recs.length) return html + '<p class="muted empty">No messages yet.</p>';
  html += '<table><thead><tr>' +
    '<th>Uptime</th><th>Dir</th><th>Type</th><th>Hex</th>' +
    '</tr></thead><tbody>';
  recs.forEach(r => {
    const cls = r.tag === MSG_TAG_REJECTED ? ' class="rej"'
      : r.tag === MSG_TAG_MIXED ? ' class="mixed"'
      : r.dir === 1 ? ' class="tx"' : '';
    const hex = escapeHtml(r.hex || '') + (r.truncated ? ' …' : '');
    html += '<tr' + cls + '><td>' + (r.ms / 1000).toFixed(1) + 's</td><td>' +
      (r.dir === 1 ? 'TX' : 'RX') + '</td><td>' + (MSG_TAGS[r.tag] || r.tag) +
      '</td><td class="hex">' + hex + '</td></tr>';
  });
  return html + '</tbody></table>';
}

function renderTable() {
  const app = document.getElementById('app');
  const log = logs[tab];

  if (!log) {
    app.innerHTML = '<p class="muted empty">Loading…</p>';
    return;
  }

  const entries = log.entries || [];
  let html;
  if (tab === 'panic') {
    // No cap/count: the partition stores one crash. Clear erases it, so only
    // offer the button when there's something to erase.
    const clear = entries.length
      ? '<button class="clear" data-log="panic">Clear</button>' : '';
    html = '<div class="bar"><span class="muted">most recent crash</span>' +
      clear + '</div>' + panicHtml(entries);
  } else if (tab === 'heap') {
    html = heapHtml(log);   // owns its Now/History sections + the history bar
  } else if (tab === 'blescan') {
    html = bleScanHtml(log);  // live snapshot, owns its own status bar
  } else if (tab === 'scalemsg') {
    html = scaleMsgHtml(log);  // live capture-tap ring, owns its own status bar
  } else if (tab === 'power') {
    html = powerHtml(log);  // owns its Now/History sections + the history bar
  } else {
    html = barHtml(log, entries.length, 'entries') + tableHtml(COLS[tab], entries);
  }

  app.innerHTML = html;
  const btn = app.querySelector('button.clear');
  if (btn) btn.onclick = onClear;
}

function render() {
  renderTabs();
  // Panic is readable unpaired; every other tab needs auth.
  if (!authed && tab !== 'panic') {
    renderUnauthedContent();
    syncCopyButton();
    return;
  }
  renderTable();
  syncCopyButton();
}

// --- actions ----------------------------------------------------------------
function syncCopyButton() {
  const btn = document.getElementById('copy-log');
  btn.disabled = !logs[tab] || (!authed && tab !== 'panic');
}

function currentLogText() {
  const title = TABS.find(t => t.key === tab).label;
  const lines = ['Pump Bug logs: ' + title];
  let tables = 0;

  document.querySelectorAll('#app > .section, #app > .bar, #app > table').forEach(el => {
    if (el.classList.contains('section')) {
      lines.push('', '[' + el.textContent.trim() + ']');
      return;
    }
    if (el.classList.contains('bar')) {
      const summary = el.querySelector('.muted');
      if (lines.length === 1) lines.push('');
      if (summary) lines.push(summary.textContent.trim());
      return;
    }

    ++tables;
    if (lines.length === 1) lines.push('');
    el.querySelectorAll('tr').forEach(row => {
      const cells = Array.from(row.querySelectorAll('th, td'));
      lines.push(cells.map(cell => cell.textContent.trim()).join('\t'));
    });
  });

  if (!tables) {
    const status = document.querySelector('#app .empty') ||
      document.querySelector('#app .muted');
    if (status) {
      if (lines.length === 1) lines.push('');
      lines.push(status.textContent.trim());
    }
  }

  lines.push('', '[Raw JSON]', JSON.stringify(logs[tab], null, 2));
  return lines.join('\n').trimEnd() + '\n';
}

function fallbackCopy(text) {
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.top = '0';
  ta.style.left = '0';
  ta.style.width = '1px';
  ta.style.height = '1px';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.select();
  ta.setSelectionRange(0, ta.value.length);
  let copied = false;
  try {
    copied = document.execCommand('copy');
  } catch (_) {
    // Report failure through the button below.
  }
  ta.remove();
  return copied;
}

function showCopyResult(copied) {
  const btn = document.getElementById('copy-log');
  const status = document.getElementById('copy-status');
  btn.textContent = copied ? 'Copied' : 'Copy failed';
  status.textContent = copied ? 'Log copied to clipboard.' : 'Could not copy log.';
  clearTimeout(copyResetTimer);
  copyResetTimer = setTimeout(() => {
    btn.textContent = COPY_BUTTON_LABEL;
    status.textContent = '';
  }, 1600);
}

function onCopyLog() {
  const text = currentLogText();
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(
      () => showCopyResult(true),
      () => showCopyResult(fallbackCopy(text))
    );
  } else {
    showCopyResult(fallbackCopy(text));
  }
}

function selectTab(key) {
  if (key === tab) return;
  tab = key;
  render();   // show cached log (or "Loading…") immediately
  poll();     // refresh the newly-visible tab
}

async function onClear(ev) {
  const log = ev.currentTarget.getAttribute('data-log');
  if (!confirm('Clear the ' + log + ' log? This cannot be undone.')) return;
  if (await clearLog(log)) await poll();
}

// Fetch only the visible tab's log. Capture the tab up front: it can change
// while loadLog() is awaited (a tab switch fires its own poll), and we must
// store/render the response against the tab it was for, not whatever is showing
// when it resolves — otherwise a slow response lands in the wrong tab's cache.
async function pollOnce() {
  const key = tab;
  const r = await loadLog(key);
  if (r.status === 401) { authed = false; render(); return; }
  if (r.status === 0 || r.status >= 500) return;  // transient; keep last view
  // Panic needs no pairing, so a 200 there says nothing about auth state — only
  // an authenticated tab succeeding proves we're paired.
  if (key !== 'panic') authed = true;
  if (r.changed) {
    logs[key] = r.data;
    if (key === tab) render();  // only repaint if that tab is still showing
  }
}

// A Power refresh may require several requests. If another refresh is requested
// while one is running, run it once after the current sequence finishes.
function poll() {
  if (pollPromise) {
    pollQueued = true;
    return pollPromise;
  }
  pollPromise = (async () => {
    do {
      pollQueued = false;
      await pollOnce();
    } while (pollQueued);
  })().finally(() => {
    pollPromise = null;
  });
  return pollPromise;
}

function start() {
  document.getElementById('copy-log').onclick = onCopyLog;
  poll();
  pollTimer = setInterval(poll, POLL_MS);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      clearInterval(pollTimer);
      pollTimer = null;
    } else if (!pollTimer) {
      poll();
      pollTimer = setInterval(poll, POLL_MS);
    }
  });
}

start();
