// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

function parseExtraction(buf) {
  return parseExtractionFromDv(new DataView(buf), 0);
}

const U32_WRAP_MS = 0x100000000;
const U32_HALF_WRAP_MS = 0x80000000;

// Wire timestamps are raw uint32 millis() values. Decode them into one
// monotonic shot-local timeline: values just after a millis() wrap move above
// 2^32, while genuinely stale pre-begin samples remain before beginMs.
function unwrapTimestamp(rawMs, beginMs) {
  const raw = rawMs >>> 0;
  const begin = beginMs >>> 0;
  const unsignedDelta = (raw - begin) >>> 0;
  return raw < begin && unsignedDelta < U32_HALF_WRAP_MS
    ? raw + U32_WRAP_MS
    : raw;
}

function unwrapOptionalTimestamp(rawMs, beginMs) {
  return rawMs === 0 ? 0 : unwrapTimestamp(rawMs, beginMs);
}

// Same as parseExtraction but reads from `dv` starting at `off` —
// lets us decode the EXTR blob nested inside an STKP SSE packet
// without copying out a sub-array.
function parseExtractionFromDv(dv, off) {
  if (dv.getUint8(off + 0) !== 0x45 || dv.getUint8(off + 1) !== 0x58 ||
      dv.getUint8(off + 2) !== 0x54 || dv.getUint8(off + 3) !== 0x52) {
    throw new Error('bad magic');
  }
  const version = dv.getUint8(off + 4);
  const kCompactVersion = 6;
  if (version !== kCompactVersion) {
    throw new Error('unsupported version ' + version);
  }

  const phase = dv.getUint8(off + 5);
  const endCause = dv.getUint8(off + 6);
  const flags = dv.getUint8(off + 7);
  const beginMs = dv.getUint32(off + 8, true);
  const lastPumpOffMs =
    unwrapOptionalTimestamp(dv.getUint32(off + 12, true), beginMs);
  const stableMs =
    unwrapOptionalTimestamp(dv.getUint32(off + 16, true), beginMs);
  const endMs =
    unwrapOptionalTimestamp(dv.getUint32(off + 20, true), beginMs);
  const totalPumpOnMs = dv.getUint32(off + 24, true);

  // Fixed header: raw samples, endpoint yield, graduation evidence.
  const startUtcSec = dv.getUint32(off + 28, true);
  const yieldCg = dv.getInt16(off + 32, true);
  const startRawCg = dv.getInt16(off + 34, true);
  const settledRawCg = dv.getInt16(off + 36, true);
  const observedSampleCount = dv.getUint16(off + 38, true);
  const eventCount = dv.getUint16(off + 40, true);
  const sampleCount = dv.getUint16(off + 42, true);
  const yieldStatus = dv.getUint8(off + 44);
  // off + 45 reserved
  const pourMs = dv.getUint32(off + 46, true);
  const decisionGainCg = dv.getInt32(off + 50, true);
  let p = off + 54;
  function uv() {
    let v = 0, s = 0, b;
    do { b = dv.getUint8(p++); v |= (b & 0x7f) << s; s += 7; }
    while (b & 0x80);
    return v >>> 0;
  }
  function sv() {
    const u = uv();
    return (u >>> 1) ^ -(u & 1);
  }

  const events = [];
  let tEv = beginMs;
  for (let i = 0; i < eventCount; i++) {
    tEv = (tEv + uv()) >>> 0;
    const k = dv.getUint8(p++);
    events.push({ tMs: unwrapTimestamp(tEv, beginMs), kind: k });
  }

  const samples = [];
  let tSa = beginMs;
  let cgSa = 0;
  const hasScaleTimerFields = (flags & RECORD_FLAG_SCALE_TIMERS) !== 0;
  for (let i = 0; i < sampleCount; i++) {
    tSa = (tSa + uv()) >>> 0;
    cgSa += sv();
    const scaleTimerMs = hasScaleTimerFields ? uv() : SCALE_TIMER_UNKNOWN_MS;
    samples.push({ tMs: unwrapTimestamp(tSa, beginMs), cg: cgSa, scaleTimerMs });
  }

  const target = {
    targetCg: dv.getUint16(p + 0, true),
    armed: dv.getUint8(p + 2) !== 0,
    tauMs: dv.getUint16(p + 3, true),
    cCg: dv.getInt16(p + 5, true),
    reactionLeadMs: dv.getUint16(p + 7, true),
  };
  const alarmTriggeredMs =
    unwrapOptionalTimestamp(dv.getUint32(p + 9, true), beginMs);
  const alarmYieldCg = dv.getInt16(p + 13, true);
  const alarmFlowCgPerS = dv.getInt16(p + 15, true);
  const alarmFlowValid = dv.getUint8(p + 17) !== 0;
  const alarmProjectedFinalCg = dv.getInt16(p + 18, true);

  return {
    version, phase, endCause, flags,
    beginMs, lastPumpOffMs, stableMs, endMs, totalPumpOnMs, startUtcSec,
    yieldCg, startRawCg, settledRawCg, yieldStatus, pourMs, decisionGainCg,
    observedSampleCount, eventCount, sampleCount,
    events, samples,
    target,
    alarmTriggeredMs, alarmYieldCg, alarmFlowCgPerS, alarmFlowValid,
    alarmProjectedFinalCg,
  };
}
