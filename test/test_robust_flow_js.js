#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only


const fs = require('fs');
const path = require('path');

global.SCALE_TIMER_UNKNOWN_MS = 0xffffffff;

const [referencePath, robustFlowPath] = process.argv.slice(2);
if (!referencePath || !robustFlowPath) {
  console.error('usage: test_robust_flow_js.js REFERENCE_TSV ROBUST_FLOW_JS');
  process.exit(2);
}

const RobustFlow = require(path.resolve(robustFlowPath));

const samples = [];
const expected = [];
let statsHave = null;
let sustainedPeak = null;

for (const line of fs.readFileSync(referencePath, 'utf8').split(/\r?\n/)) {
  if (!line) continue;
  if (line.startsWith('#')) {
    const fields = line.slice(1).trim().split(/\s+/);
    if (fields[0] === 'stats_have') statsHave = fields[1] === '1';
    if (fields[0] === 'sustained_peak_g_s') sustainedPeak = Number(fields[1]);
    continue;
  }
  if (line.startsWith('t_ms')) continue;
  const [tMs, cg, scaleTimerMs, flow] = line.split(/\t/);
  samples.push({
    tMs: Number(tMs),
    g: Number(cg) / 100,
    scaleTimerMs: scaleTimerMs === '-' ? SCALE_TIMER_UNKNOWN_MS : Number(scaleTimerMs),
  });
  expected.push(flow === 'null' ? null : Number(flow));
}

let failures = 0;
function check(cond, msg) {
  if (!cond) {
    console.error(`FAIL: ${msg}`);
    failures += 1;
  }
}

const hint = { before: 0 };
for (let i = 0; i < samples.length; i += 1) {
  const got = RobustFlow.sampleFlow(samples, i, hint);
  const want = expected[i];
  if (want === null) {
    check(got === null, `sample ${i}: expected null, got ${got}`);
  } else {
    check(got !== null, `sample ${i}: expected ${want}, got null`);
    if (got !== null) {
      check(Math.abs(got - want) <= 0.0001,
            `sample ${i}: expected ${want}, got ${got}`);
    }
  }
}

const stats = RobustFlow.computeFlowSeriesStats(samples);
check(stats.have === statsHave, `stats.have expected ${statsHave}, got ${stats.have}`);
if (statsHave) {
  check(Math.abs(stats.sustainedPeakGPerS - sustainedPeak) <= 0.0001,
        `sustained peak expected ${sustainedPeak}, got ${stats.sustainedPeakGPerS}`);
}

if (failures === 0) {
  console.log('OK: robust-flow JavaScript reference passed');
  process.exit(0);
}
console.error(`${failures} failure(s)`);
process.exit(1);
