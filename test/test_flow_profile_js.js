#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only


const fs = require('fs');
const vm = require('vm');

const [robustFlowPath, flowProfilePath] = process.argv.slice(2);
if (!robustFlowPath || !flowProfilePath) {
  console.error('usage: test_flow_profile_js.js ROBUST_FLOW_JS FLOW_PROFILE_JS');
  process.exit(2);
}

const context = {
  console,
  NO_WEIGHT_CG: -32768,
  SCALE_TIMER_UNKNOWN_MS: 0xffffffff,
};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(robustFlowPath, 'utf8') +
    '\nglobalThis.RobustFlow = RobustFlow;',
  context,
);
vm.runInContext(
  fs.readFileSync(flowProfilePath, 'utf8') +
    '\nglobalThis.FlowProfile = FlowProfile;',
  context,
);

const samples = [];
for (let i = 0; i <= 20; i += 1) {
  samples.push({ tMs: i * 500, rawCg: i * 200 });
}
samples.push({ tMs: 10500, rawCg: 9410 });

const legacy = context.FlowProfile.compute({
  samples,
  startRawCg: 0,
  yieldCg: 0,
  decisionGainCg: 4000,
});
if (!legacy.valid || legacy.yieldCg !== 4000) {
  console.error(
    `FAIL: expected decision gain 4000, got ${legacy.valid ? legacy.yieldCg : legacy.reason}`,
  );
  process.exit(1);
}

const stored = context.FlowProfile.compute({
  samples,
  startRawCg: 0,
  yieldCg: 4100,
  decisionGainCg: 4000,
});
if (!stored.valid || stored.yieldCg !== 4100) {
  console.error(
    `FAIL: expected stored yield 4100, got ${stored.valid ? stored.yieldCg : stored.reason}`,
  );
  process.exit(1);
}

const samplesOnly = context.FlowProfile.compute({
  samples,
  startRawCg: 0,
  yieldCg: 0,
  decisionGainCg: 0,
});
if (!samplesOnly.valid || samplesOnly.yieldCg !== 9410) {
  console.error(
    `FAIL: expected sample maximum 9410, got ${samplesOnly.valid ? samplesOnly.yieldCg : samplesOnly.reason}`,
  );
  process.exit(1);
}

console.log('OK: flow-profile final yield selection passed');
