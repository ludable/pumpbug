#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only


const fs = require('fs');
const vm = require('vm');

const [protocolPath] = process.argv.slice(2);
if (!protocolPath) {
  console.error('usage: test_extraction_protocol_js.js PROTOCOL_JS');
  process.exit(2);
}

const CURRENT_VERSION = 6;
const HEADER_BYTES = 54;
const TARGET_BLOCK_BYTES = 20;

const context = {
  DataView,
  NO_WEIGHT_CG: -32768,
  RECORD_FLAG_SCALE_TIMERS: 2,
  SCALE_TIMER_UNKNOWN_MS: 0xffffffff,
};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(protocolPath, 'utf8') +
    '\nglobalThis.parseExtraction = parseExtraction;',
  context,
);

const bytes = new Uint8Array(HEADER_BYTES + TARGET_BLOCK_BYTES);
bytes.set([0x45, 0x58, 0x54, 0x52, CURRENT_VERSION]);  // EXTR
const decoded = context.parseExtraction(bytes.buffer);
if (decoded.version !== CURRENT_VERSION) {
  throw new Error('current-version record did not decode');
}

for (const version of [CURRENT_VERSION - 1, CURRENT_VERSION + 1]) {
  bytes[4] = version;
  let rejected = false;
  try {
    context.parseExtraction(bytes.buffer);
  } catch (error) {
    rejected = new RegExp(`unsupported version ${version}`).test(String(error));
  }
  if (!rejected) throw new Error(`version ${version} was not rejected`);
}

console.log('OK: extraction protocol accepts only the current version');
